#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <wtsapi32.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WTS_CHANNEL_OPTION_DYNAMIC
#define WTS_CHANNEL_OPTION_DYNAMIC 0x00000001
#endif

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef HANDLE HPCON;
typedef HRESULT(WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef void(WINAPI *ClosePseudoConsoleFn)(HPCON);
typedef HRESULT(WINAPI *ResizePseudoConsoleFn)(HPCON, COORD);

namespace rdp2exec
{
  namespace frame
  {
    constexpr uint8_t kInput = 0x01;
    constexpr uint8_t kResize = 0x02;
    constexpr uint8_t kClose = 0x03;
    constexpr uint8_t kReady = 0x81;
    constexpr uint8_t kOutput = 0x82;
    constexpr uint8_t kExit = 0x83;
    constexpr uint8_t kError = 0x84;
  } // namespace frame
} // namespace rdp2exec

struct ConptyApi
{
  CreatePseudoConsoleFn create = nullptr;
  ClosePseudoConsoleFn close = nullptr;
  ResizePseudoConsoleFn resize = nullptr;
};

struct WriteGuard
{
  std::mutex mu;
};

static bool load_conpty_api(ConptyApi &api)
{
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  if (!kernel)
  {
    return false;
  }
  api.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
  api.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
  api.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
  return api.create && api.close && api.resize;
}

static bool write_all(HANDLE h, const uint8_t *data, size_t size)
{
  size_t total = 0;
  while (total < size)
  {
    ULONG written = 0;
    if (!WTSVirtualChannelWrite(h, const_cast<LPSTR>(reinterpret_cast<const char *>(data + total)),
                                static_cast<ULONG>(size - total), &written))
    {
      return false;
    }
    if (written == 0)
    {
      return false;
    }
    total += written;
  }
  return true;
}

static bool send_frame(HANDLE channel, WriteGuard &guard, uint8_t type, const void *payload, uint32_t size)
{
  uint8_t hdr[5];
  hdr[0] = type;
  hdr[1] = static_cast<uint8_t>(size & 0xFF);
  hdr[2] = static_cast<uint8_t>((size >> 8) & 0xFF);
  hdr[3] = static_cast<uint8_t>((size >> 16) & 0xFF);
  hdr[4] = static_cast<uint8_t>((size >> 24) & 0xFF);

  std::lock_guard<std::mutex> lock(guard.mu);
  if (!write_all(channel, hdr, sizeof(hdr)))
  {
    return false;
  }
  if (size && payload)
  {
    return write_all(channel, reinterpret_cast<const uint8_t *>(payload), size);
  }
  return true;
}

static bool send_text(HANDLE channel, WriteGuard &guard, uint8_t type, const std::string &text)
{
  return send_frame(channel, guard, type, text.data(), static_cast<uint32_t>(text.size()));
}

static bool is_channel_gone_error(DWORD err)
{
  switch (err)
  {
  case ERROR_INVALID_HANDLE:
  case ERROR_BROKEN_PIPE:
  case ERROR_NO_DATA:
  case ERROR_OPERATION_ABORTED:
  case ERROR_GEN_FAILURE:
  case ERROR_DEVICE_NOT_CONNECTED:
    return true;
  default:
    return false;
  }
}

struct FrameParser
{
  std::vector<uint8_t> buf;

  template <typename Fn>
  void feed(const uint8_t *data, size_t size, Fn fn)
  {
    buf.insert(buf.end(), data, data + size);
    while (buf.size() >= 5)
    {
      const uint8_t type = buf[0];
      const uint32_t len = static_cast<uint32_t>(buf[1]) |
                           (static_cast<uint32_t>(buf[2]) << 8) |
                           (static_cast<uint32_t>(buf[3]) << 16) |
                           (static_cast<uint32_t>(buf[4]) << 24);
      if (buf.size() < (5u + len))
      {
        return;
      }
      std::vector<uint8_t> payload;
      if (len)
      {
        payload.assign(buf.begin() + 5, buf.begin() + 5 + len);
      }
      buf.erase(buf.begin(), buf.begin() + 5 + len);
      fn(type, payload);
    }
  }
};

struct Args
{
  std::string channel = "rdp2exec";
  std::wstring child = L"powershell";
  short cols = 120;
  short rows = 40;
};

static Args parse_args(int argc, wchar_t **argv)
{
  Args args;
  for (int i = 1; i < argc; ++i)
  {
    const std::wstring a = argv[i];
    if (a == L"--channel" && i + 1 < argc)
    {
      std::wstring v = argv[++i];
      args.channel.assign(v.begin(), v.end());
    }
    else if (a == L"--child" && i + 1 < argc)
    {
      args.child = argv[++i];
    }
    else if (a == L"--cols" && i + 1 < argc)
    {
      args.cols = static_cast<short>(_wtoi(argv[++i]));
    }
    else if (a == L"--rows" && i + 1 < argc)
    {
      args.rows = static_cast<short>(_wtoi(argv[++i]));
    }
  }
  if (args.cols <= 0)
    args.cols = 120;
  if (args.rows <= 0)
    args.rows = 40;
  return args;
}

static std::wstring build_command_line(const Args &args)
{
  if (_wcsicmp(args.child.c_str(), L"cmd") == 0)
  {
    return L"\"C:\\Windows\\System32\\cmd.exe\" /Q /K";
  }

  return L"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" "
         L"-NoLogo -NoProfile";
}

static uint32_t load_le32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static constexpr size_t kChannelPduLength = 8;

int wmain(int argc, wchar_t **argv)
{
  Args args = parse_args(argc, argv);

  HANDLE channel = WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, const_cast<LPSTR>(args.channel.c_str()),
                                           WTS_CHANNEL_OPTION_DYNAMIC);
  if (!channel)
  {
    return static_cast<int>(GetLastError());
  }

  WriteGuard write_guard;
  ConptyApi conpty{};
  if (!load_conpty_api(conpty))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError,
              "ConPTY API unavailable on this Windows build/session");
    WTSVirtualChannelClose(channel);
    return 20;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE pty_in_read = nullptr, pty_in_write = nullptr;
  HANDLE pty_out_read = nullptr, pty_out_write = nullptr;
  if (!CreatePipe(&pty_in_read, &pty_in_write, &sa, 0))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "CreatePipe(input) failed");
    WTSVirtualChannelClose(channel);
    return 21;
  }
  if (!CreatePipe(&pty_out_read, &pty_out_write, &sa, 0))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "CreatePipe(output) failed");
    CloseHandle(pty_in_read);
    CloseHandle(pty_in_write);
    WTSVirtualChannelClose(channel);
    return 22;
  }

  COORD size{args.cols, args.rows};
  HPCON hpc = nullptr;
  HRESULT hr = conpty.create(size, pty_in_read, pty_out_write, 0, &hpc);
  CloseHandle(pty_in_read);
  CloseHandle(pty_out_write);
  if (FAILED(hr))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "CreatePseudoConsole failed");
    CloseHandle(pty_in_write);
    CloseHandle(pty_out_read);
    WTSVirtualChannelClose(channel);
    return 23;
  }

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  auto *attr_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attr_size));
  if (!attr_list)
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "HeapAlloc(attr_list) failed");
    conpty.close(hpc);
    CloseHandle(pty_in_write);
    CloseHandle(pty_out_read);
    WTSVirtualChannelClose(channel);
    return 24;
  }
  if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "InitializeProcThreadAttributeList failed");
    HeapFree(GetProcessHeap(), 0, attr_list);
    conpty.close(hpc);
    CloseHandle(pty_in_write);
    CloseHandle(pty_out_read);
    WTSVirtualChannelClose(channel);
    return 25;
  }
  if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(hpc), nullptr,
                                 nullptr))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "UpdateProcThreadAttribute(PSEUDOCONSOLE) failed");
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    conpty.close(hpc);
    CloseHandle(pty_in_write);
    CloseHandle(pty_out_read);
    WTSVirtualChannelClose(channel);
    return 26;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.lpAttributeList = attr_list;
  PROCESS_INFORMATION pi{};

  std::wstring cmdline = build_command_line(args);
  std::vector<wchar_t> cmd_mut(cmdline.begin(), cmdline.end());
  cmd_mut.push_back(L'\0');

  if (!CreateProcessW(nullptr, cmd_mut.data(), nullptr, nullptr, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                      &si.StartupInfo, &pi))
  {
    send_text(channel, write_guard, rdp2exec::frame::kError, "CreateProcessW child failed");
    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
    conpty.close(hpc);
    CloseHandle(pty_in_write);
    CloseHandle(pty_out_read);
    WTSVirtualChannelClose(channel);
    return 27;
  }

  CloseHandle(pi.hThread);
  send_frame(channel, write_guard, rdp2exec::frame::kReady, nullptr, 0);

  std::atomic<bool> running{true};
  std::thread out_thread([&]()
                         {
    std::vector<uint8_t> out(8192);
    while (running.load()) {
      DWORD n = 0;
      if (!ReadFile(pty_out_read, out.data(), static_cast<DWORD>(out.size()), &n, nullptr)) {
        break;
      }
      if (n == 0) {
        break;
      }
      if (!send_frame(channel, write_guard, rdp2exec::frame::kOutput, out.data(), static_cast<uint32_t>(n))) {
        running.store(false);
        TerminateProcess(pi.hProcess, 0);
        break;
      }
    } });

  FrameParser parser;
  std::vector<uint8_t> rx(8192);
  while (running.load())
  {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 0);
    if (wait == WAIT_OBJECT_0)
    {
      break;
    }

    ULONG read = 0;
    if (!WTSVirtualChannelRead(channel, 200, reinterpret_cast<LPSTR>(rx.data()), static_cast<ULONG>(rx.size()),
                               &read))
    {
      const DWORD err = GetLastError();
      if (is_channel_gone_error(err))
      {
        running.store(false);
        TerminateProcess(pi.hProcess, 0);
        break;
      }
      continue;
    }
    if (read < kChannelPduLength)
    {
      continue;
    }

    const uint32_t pdu_len = load_le32(rx.data());
    (void)pdu_len;
    const uint8_t *pdu_payload = rx.data() + kChannelPduLength;
    const size_t pdu_payload_size = static_cast<size_t>(read) - kChannelPduLength;
    if (pdu_payload_size == 0)
    {
      continue;
    }

    parser.feed(pdu_payload, pdu_payload_size, [&](uint8_t type, const std::vector<uint8_t> &payload)
                {
      if (type == rdp2exec::frame::kInput) {
        if (!payload.empty()) {
          DWORD written = 0;
          if (!WriteFile(pty_in_write, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
            running.store(false);
            TerminateProcess(pi.hProcess, 0);
          }
        }
      } else if (type == rdp2exec::frame::kResize) {
        if (payload.size() >= 4) {
          const short cols = static_cast<short>(payload[0] | (payload[1] << 8));
          const short rows = static_cast<short>(payload[2] | (payload[3] << 8));
          COORD new_size{static_cast<SHORT>(cols > 0 ? cols : 120),
                         static_cast<SHORT>(rows > 0 ? rows : 40)};
          conpty.resize(hpc, new_size);
        }
      } else if (type == rdp2exec::frame::kClose) {
        running.store(false);
        TerminateProcess(pi.hProcess, 0);
      } });
  }

  running.store(false);
  if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
  {
    TerminateProcess(pi.hProcess, 0);
  }
  CloseHandle(pty_in_write);
  CloseHandle(pty_out_read);
  WaitForSingleObject(pi.hProcess, 3000);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  send_frame(channel, write_guard, rdp2exec::frame::kExit, &exit_code, sizeof(exit_code));

  if (out_thread.joinable())
  {
    out_thread.join();
  }

  CloseHandle(pi.hProcess);
  DeleteProcThreadAttributeList(attr_list);
  HeapFree(GetProcessHeap(), 0, attr_list);
  conpty.close(hpc);
  WTSVirtualChannelClose(channel);
  return static_cast<int>(exit_code);
}
