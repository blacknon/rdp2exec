#include <freerdp/client/channels.h>
#include <freerdp/channels/log.h>
#include <freerdp/dvc.h>

#include <winpr/crt.h>
#include <winpr/stream.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/protocol.hpp"

#define TAG CHANNELS_TAG("rdp2exec.client")

namespace
{

  struct RDP2EXEC_PLUGIN
  {
    GENERIC_DYNVC_PLUGIN base;
  };

  struct RDP2EXEC_CHANNEL_CALLBACK
  {
    GENERIC_CHANNEL_CALLBACK generic;
    int sock_fd;
    int running;
    int reader_started;
    pthread_t reader;
    char socket_path[108];
  };

  std::string resolve_socket_path()
  {
    const char *env = std::getenv("RDP2EXEC_SOCKET");
    if (env && env[0] != '\0')
    {
      return env;
    }
    return rdp2exec::kDefaultSocketPath;
  }

  int connect_unix_socket(const std::string &path)
  {
    if (path.empty())
    {
      return -1;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
      WLog_ERR(TAG, "socket() failed: %d", errno);
      return -1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
      WLog_ERR(TAG, "socket path too long: %s", path.c_str());
      ::close(fd);
      return -1;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    constexpr int kAttempts = 100;
    for (int i = 0; i < kAttempts; ++i)
    {
      if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
      {
        return fd;
      }

      if ((errno == ENOENT) || (errno == ECONNREFUSED))
      {
        usleep(200 * 1000);
        continue;
      }

      WLog_ERR(TAG, "connect(%s) failed: %d", path.c_str(), errno);
      break;
    }

    ::close(fd);
    return -1;
  }

  void *socket_reader_thread(void *arg)
  {
    auto *cb = reinterpret_cast<RDP2EXEC_CHANNEL_CALLBACK *>(arg);
    std::vector<BYTE> buffer(8192);

    while (cb && cb->running)
    {
      const ssize_t rc = ::read(cb->sock_fd, buffer.data(), buffer.size());
      if (rc == 0)
      {
        break;
      }
      if (rc < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        WLog_ERR(TAG, "read(local socket) failed: %d", errno);
        break;
      }

      const UINT status = cb->generic.channel->Write(cb->generic.channel, static_cast<UINT32>(rc),
                                                     buffer.data(), nullptr);
      if (status != CHANNEL_RC_OK)
      {
        WLog_ERR(TAG, "channel->Write failed: %" PRIu32, status);
        break;
      }
    }

    return nullptr;
  }

  UINT rdp2exec_on_data_received(IWTSVirtualChannelCallback *pChannelCallback, wStream *data)
  {
    auto *cb = reinterpret_cast<RDP2EXEC_CHANNEL_CALLBACK *>(pChannelCallback);
    if (!cb || cb->sock_fd < 0 || !data)
    {
      return CHANNEL_RC_OK;
    }

    const BYTE *pBuffer = static_cast<const BYTE *>(Stream_Pointer(data));
    const UINT32 cbSize = Stream_GetRemainingLength(data);

    if (!pBuffer || cbSize == 0)
    {
      return CHANNEL_RC_OK;
    }

    ssize_t total = 0;
    while (total < static_cast<ssize_t>(cbSize))
    {
      const ssize_t rc = ::write(cb->sock_fd, pBuffer + total, cbSize - static_cast<UINT32>(total));
      if (rc <= 0)
      {
        WLog_ERR(TAG, "write(local socket) failed: %d", errno);
        return CHANNEL_RC_OK;
      }
      total += rc;
    }

    return CHANNEL_RC_OK;
  }

  UINT rdp2exec_on_open(IWTSVirtualChannelCallback *pChannelCallback)
  {
    auto *cb = reinterpret_cast<RDP2EXEC_CHANNEL_CALLBACK *>(pChannelCallback);
    if (!cb)
    {
      return ERROR_INVALID_DATA;
    }

    std::memset(cb->socket_path, 0, sizeof(cb->socket_path));
    const std::string socket_path = resolve_socket_path();
    if (socket_path.size() >= sizeof(cb->socket_path))
    {
      WLog_ERR(TAG, "socket path too long: %s", socket_path.c_str());
      return ERROR_BAD_ARGUMENTS;
    }
    std::snprintf(cb->socket_path, sizeof(cb->socket_path), "%s", socket_path.c_str());

    cb->sock_fd = connect_unix_socket(socket_path);
    if (cb->sock_fd < 0)
    {
      WLog_ERR(TAG, "failed to connect to launcher socket: %s", cb->socket_path);
      return ERROR_OPEN_FAILED;
    }

    cb->running = 1;
    cb->reader_started = 0;
    if (pthread_create(&cb->reader, nullptr, socket_reader_thread, cb) != 0)
    {
      WLog_ERR(TAG, "pthread_create failed");
      ::close(cb->sock_fd);
      cb->sock_fd = -1;
      cb->running = 0;
      return ERROR_INTERNAL_ERROR;
    }

    cb->reader_started = 1;
    WLog_INFO(TAG, "rdp2exec DVC opened; socket=%s", cb->socket_path);
    return CHANNEL_RC_OK;
  }

  UINT rdp2exec_on_close(IWTSVirtualChannelCallback *pChannelCallback)
  {
    auto *cb = reinterpret_cast<RDP2EXEC_CHANNEL_CALLBACK *>(pChannelCallback);
    if (!cb)
    {
      return CHANNEL_RC_OK;
    }

    cb->running = 0;
    if (cb->sock_fd >= 0)
    {
      ::shutdown(cb->sock_fd, SHUT_RDWR);
      ::close(cb->sock_fd);
      cb->sock_fd = -1;
    }

    if (cb->reader_started)
    {
      pthread_join(cb->reader, nullptr);
    }

    std::free(cb);
    return CHANNEL_RC_OK;
  }

  static const IWTSVirtualChannelCallback rdp2exec_callbacks = {
      rdp2exec_on_data_received,
      rdp2exec_on_open,
      rdp2exec_on_close,
  };

} // namespace

extern "C" UINT DVCPluginEntry(IDRDYNVC_ENTRY_POINTS *pEntryPoints)
{
  return freerdp_generic_DVCPluginEntry(pEntryPoints, TAG, rdp2exec::kChannelName, sizeof(RDP2EXEC_PLUGIN),
                                        sizeof(RDP2EXEC_CHANNEL_CALLBACK), &rdp2exec_callbacks, nullptr,
                                        nullptr);
}
