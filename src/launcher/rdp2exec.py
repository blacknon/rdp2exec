#!/usr/bin/env python3
from __future__ import annotations

import argparse
import getpass
import os
import select
import selectors
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import textwrap
import threading
import time
import tty
import uuid
from pathlib import Path

DEFAULT_SOCKET = "/tmp/rdp2exec-stdio.sock"
DEFAULT_PLUGIN_DIR = "/opt/freerdp/lib/freerdp3"
DEFAULT_PLUGIN_NAME = "librdp2exec-client.so"
DEFAULT_XFREERDP = "/opt/freerdp/bin/xfreerdp"
DEFAULT_DISPLAY = os.environ.get("DISPLAY", ":99")
DEFAULT_DRIVE_NAME = "rdp2exec"
DEFAULT_HELPER_EXE = "/opt/rdp2exec/windows-x64/rdp2exec_conpty_bridge.exe"

FRAME_INPUT = 0x01
FRAME_RESIZE = 0x02
FRAME_CLOSE = 0x03
FRAME_READY = 0x81
FRAME_OUTPUT = 0x82
FRAME_EXIT = 0x83
FRAME_ERROR = 0x84


def debug_print(enabled: bool, *parts, **kwargs):
    if enabled:
        print(*parts, **kwargs)


def write_text(path: Path, text: str):
    path.write_text(text, encoding="utf-8", newline="\r\n")


def ensure_plugin(plugin_dir: str, plugin_name: str) -> Path:
    path = Path(plugin_dir) / plugin_name
    if not path.exists():
        raise FileNotFoundError(f"plugin not found: {path}")
    return path


def ensure_helper(helper_path: str) -> Path:
    path = Path(helper_path)
    if not path.exists():
        raise FileNotFoundError(f"Windows helper not found: {path}")
    return path


def resolve_password(args) -> str:
    if args.password:
        return args.password
    if sys.stdin.isatty() and sys.stderr.isatty():
        return getpass.getpass("RDP password: ")
    raise RuntimeError(
        "RDP password is required. Provide -P/--password, set RDP_PASSWORD, or run from an interactive terminal for prompt input."
    )


def parse_target(value: str) -> tuple[str, str]:
    username, sep, host = value.rpartition("@")
    if not sep or not username or not host:
        raise argparse.ArgumentTypeError(
            "target must be specified as user@hostname"
        )
    return username, host


def get_terminal_size() -> tuple[int, int]:
    try:
        cols, rows = os.get_terminal_size(sys.stdin.fileno())
        return cols, rows
    except OSError:
        return 120, 40


def quote_powershell_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def quote_cmd_token(value: str) -> str:
    operators = {"&", "&&", "||", "|", ">", ">>", "<", "2>", "2>>", "1>", "1>>"}
    if value in operators:
        return value
    if any(ch.isspace() or ch in '&|<>^()"%' for ch in value):
        return '"' + value.replace('"', '""') + '"'
    return value


def build_command_script(child: str, command: list[str]) -> tuple[str, str]:
    if not command:
        raise ValueError("command must not be empty")

    if child == "powershell":
        invocation = " ".join(quote_powershell_literal(part) for part in command)
        script = textwrap.dedent(
            f"""
            $ErrorActionPreference = 'Stop'
            & {invocation}
            if ($null -ne $LASTEXITCODE) {{
                exit $LASTEXITCODE
            }}
            exit 0
            """
        ).strip() + "\r\n"
        return "rdp2exec-command.ps1", script

    if child == "cmd":
        invocation = " ".join(quote_cmd_token(part) for part in command)
        script = textwrap.dedent(
            f"""
            @echo off
            setlocal enableextensions
            {invocation}
            exit /b %ERRORLEVEL%
            """
        ).strip() + "\r\n"
        return "rdp2exec-command.cmd", script

    raise ValueError("child must be powershell or cmd")


def build_run_wrapper(drive_name: str, child: str, cols: int, rows: int, command_file: str = "") -> str:
    exe_unc = f"\\\\tsclient\\{drive_name}\\rdp2exec_bridge.exe"
    child = child.lower().strip()
    if child not in {"powershell", "cmd"}:
        raise ValueError("child must be powershell or cmd")
    command_arg = f' --command-file "{command_file}"' if command_file else ""
    return textwrap.dedent(
        rf'''
@echo off
setlocal enableextensions
set "RDP2EXEC_EXE=%TEMP%\rdp2exec-conpty-%RANDOM%%RANDOM%.exe"
copy /Y "{exe_unc}" "%RDP2EXEC_EXE%" >nul
if errorlevel 1 exit /b 11
"%RDP2EXEC_EXE%" --channel rdp2exec --child {child} --cols {cols} --rows {rows}{command_arg}
set "RDP2EXEC_RC=%ERRORLEVEL%"
del /F /Q "%RDP2EXEC_EXE%" >nul 2>&1
exit /b %RDP2EXEC_RC%
'''
    ).strip() + "\r\n"


def prepare_drive_share(base_dir: Path, helper_exe: Path, child: str, drive_name: str, cols: int, rows: int,
                        command: list[str] | None = None) -> tuple[Path, str]:
    base_dir.mkdir(parents=True, exist_ok=True)
    staged_exe = base_dir / "rdp2exec_bridge.exe"
    staged_cmd = base_dir / "rdp2exec-run.cmd"
    staged_exe.write_bytes(helper_exe.read_bytes())
    command_file = ""
    if command:
        command_name, command_text = build_command_script(child, command)
        staged_command = base_dir / command_name
        write_text(staged_command, command_text)
        command_file = f"\\\\tsclient\\{drive_name}\\{command_name}"
    write_text(staged_cmd, build_run_wrapper(drive_name, child, cols, rows, command_file=command_file))
    run_command = f'cmd.exe /c "\\\\tsclient\\{drive_name}\\rdp2exec-run.cmd"'
    return base_dir, run_command


class UnixSocketServer:
    def __init__(self, path: str):
        self.path = path
        self.server = None

    def __enter__(self):
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path)
        self.server.listen(1)
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            if self.server:
                self.server.close()
        finally:
            try:
                os.unlink(self.path)
            except FileNotFoundError:
                pass

    def accept(self, timeout: float):
        self.server.settimeout(timeout)
        conn, _ = self.server.accept()
        conn.setblocking(False)
        return conn


class FrameParser:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        self.buf.extend(data)
        frames = []
        while len(self.buf) >= 5:
            frame_type = self.buf[0]
            length = struct.unpack_from("<I", self.buf, 1)[0]
            if len(self.buf) < 5 + length:
                break
            payload = bytes(self.buf[5:5 + length])
            del self.buf[:5 + length]
            frames.append((frame_type, payload))
        return frames


def send_frame(conn: socket.socket, frame_type: int, payload: bytes = b""):
    conn.sendall(bytes([frame_type]) + struct.pack("<I", len(payload)) + payload)


def wait_for_window(title: str, display: str, timeout: float):
    env = dict(os.environ)
    env["DISPLAY"] = display
    deadline = time.time() + timeout
    while time.time() < deadline:
        proc = subprocess.run(["xdotool", "search", "--name", title], env=env, capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip().splitlines()[0]
        time.sleep(0.2)
    raise TimeoutError(f"xfreerdp window with title '{title}' not found")


def focus_window(window_id: str, env: dict):
    for cmd in (
        ["xdotool", "windowactivate", "--sync", window_id],
        ["xdotool", "windowfocus", "--sync", window_id],
        ["xdotool", "windowraise", window_id],
    ):
        subprocess.run(cmd, env=env, check=False, capture_output=True, text=True)


def inject_command_via_run_dialog(title: str, command: str, display: str, window_timeout: float = 45.0,
                                  run_dialog_delay: float = 0.8, type_delay_ms: int = 8):
    window_id = wait_for_window(title, display, window_timeout)
    env = dict(os.environ)
    env["DISPLAY"] = display

    focus_window(window_id, env)
    time.sleep(0.3)
    subprocess.run(["xdotool", "key", "--window", window_id, "--clearmodifiers", "Super_L+r"], env=env,
                   check=True)
    time.sleep(run_dialog_delay)
    subprocess.run(["xdotool", "type", "--window", window_id, "--delay", str(type_delay_ms),
                    "--clearmodifiers", command], env=env, check=True)
    time.sleep(0.15)
    subprocess.run(["xdotool", "key", "--window", window_id, "Return"], env=env, check=True)


def build_xfreerdp_command(args, title: str, share_dir: Path):
    freerdp_log_level = "INFO" if args.debug else "OFF"
    cmd = [
        args.xfreerdp,
        f"/v:{args.host}",
        f"/port:{args.port}",
        f"/u:{args.username}",
        "/from-stdin:force",
        "/dvc:rdp2exec",
        f"/drive:{args.drive_name},{share_dir}",
        f"/title:{title}",
        f"/log-level:{freerdp_log_level}",
        "/size:1280x900",
        "/dynamic-resolution",
    ]
    if args.enable_clipboard:
        cmd.append("+clipboard")
    if args.domain:
        cmd.append(f"/d:{args.domain}")
    if args.cert_ignore:
        cmd.append("/cert:ignore")
    return cmd


class RawTerminal:
    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.is_tty = os.isatty(self.fd)
        self.old = None

    def __enter__(self):
        if self.is_tty:
            self.old = termios.tcgetattr(self.fd)
            tty.setraw(self.fd)
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.is_tty and self.old is not None:
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)


def interactive_bridge(conn: socket.socket, debug: bool = False):
    stop = threading.Event()
    parser = FrameParser()
    cols, rows = get_terminal_size()
    exit_code = 0

    def on_winch(signum, frame):
        if stop.is_set():
            return
        c, r = get_terminal_size()
        payload = struct.pack("<HH", c, r)
        try:
            send_frame(conn, FRAME_RESIZE, payload)
        except OSError:
            stop.set()

    prev_winch = signal.getsignal(signal.SIGWINCH)
    signal.signal(signal.SIGWINCH, on_winch)

    def recv_loop():
        nonlocal exit_code
        sel = selectors.DefaultSelector()
        sel.register(conn, selectors.EVENT_READ)
        while not stop.is_set():
            for key, _ in sel.select(timeout=0.2):
                data = key.fileobj.recv(8192)
                if not data:
                    stop.set()
                    return
                for frame_type, payload in parser.feed(data):
                    if frame_type == FRAME_READY:
                        debug_print(debug, "\r\n[rdp2exec] ConPTY bridge ready. Type `exit` to close. Ctrl-] detaches local client.\r\n", file=sys.stderr, flush=True)
                        try:
                            send_frame(conn, FRAME_INPUT, b"\r")
                        except OSError:
                            stop.set()
                            return
                    elif frame_type == FRAME_OUTPUT:
                        os.write(sys.stdout.fileno(), payload)
                    elif frame_type == FRAME_ERROR:
                        text = payload.decode("utf-8", errors="replace")
                        debug_print(debug, f"\r\n[rdp2exec] remote error: {text}\r", file=sys.stderr)
                    elif frame_type == FRAME_EXIT:
                        exit_code = struct.unpack("<I", payload[:4])[0] if len(payload) >= 4 else 0
                        debug_print(debug, f"\r\n[rdp2exec] remote exited with code {exit_code}\r", file=sys.stderr)
                        stop.set()
                        return
                    else:
                        debug_print(debug, f"\r\n[rdp2exec] unknown frame type {frame_type} len={len(payload)}\r", file=sys.stderr)

    t = threading.Thread(target=recv_loop, daemon=True)
    t.start()

    with RawTerminal():
        send_frame(conn, FRAME_RESIZE, struct.pack("<HH", cols, rows))
        try:
            fd = sys.stdin.fileno()
            while not stop.is_set():
                rr, _, _ = select.select([fd], [], [], 0.1)
                if not rr:
                    continue
                data = os.read(fd, 1024)
                if not data:
                    stop.set()
                    break
                if data == b"\x1d":
                    try:
                        send_frame(conn, FRAME_CLOSE)
                    except OSError:
                        pass
                    time.sleep(1.0)
                    stop.set()
                    break
                send_frame(conn, FRAME_INPUT, data)
        finally:
            try:
                send_frame(conn, FRAME_CLOSE)
            except OSError:
                pass
            stop.set()
            t.join(timeout=3)
            signal.signal(signal.SIGWINCH, prev_winch)
    return exit_code


def command_bridge(conn: socket.socket, debug: bool = False):
    parser = FrameParser()
    cols, rows = get_terminal_size()
    sel = selectors.DefaultSelector()
    sel.register(conn, selectors.EVENT_READ)
    send_frame(conn, FRAME_RESIZE, struct.pack("<HH", cols, rows))

    while True:
        events = sel.select(timeout=0.2)
        if not events:
            continue
        data = conn.recv(8192)
        if not data:
            return 0
        for frame_type, payload in parser.feed(data):
            if frame_type == FRAME_READY:
                continue
            if frame_type == FRAME_OUTPUT:
                os.write(sys.stdout.fileno(), payload)
            elif frame_type == FRAME_ERROR:
                text = payload.decode("utf-8", errors="replace")
                debug_print(debug, f"\n[rdp2exec] remote error: {text}", file=sys.stderr)
            elif frame_type == FRAME_EXIT:
                code = struct.unpack("<I", payload[:4])[0] if len(payload) >= 4 else 0
                debug_print(debug, f"\n[rdp2exec] remote exited with code {code}", file=sys.stderr)
                return code
            else:
                debug_print(debug, f"\n[rdp2exec] unknown frame type {frame_type} len={len(payload)}", file=sys.stderr)


class ProcessLogger:
    def __init__(self, enabled: bool, prefix: str = "", limit: int = 200):
        self.enabled = enabled
        self.prefix = prefix
        self.limit = limit
        self.lines = []
        self._lock = threading.Lock()
        self._thread = None

    def start(self, pipe):
        if pipe is None:
            return
        def _reader():
            try:
                for raw in iter(pipe.readline, b""):
                    if not raw:
                        break
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    with self._lock:
                        self.lines.append(line)
                        if len(self.lines) > self.limit:
                            self.lines = self.lines[-self.limit:]
                    if self.enabled:
                        if self.prefix:
                            print(f"{self.prefix}{line}", file=sys.stderr)
                        else:
                            print(line, file=sys.stderr)
            finally:
                try:
                    pipe.close()
                except Exception:
                    pass
        self._thread = threading.Thread(target=_reader, daemon=True)
        self._thread.start()

    def recent(self):
        with self._lock:
            return list(self.lines)

    def join(self, timeout: float = 1.0):
        if self._thread is not None:
            self._thread.join(timeout=timeout)


class nullcontext:
    def __init__(self, value):
        self.value = value

    def __enter__(self):
        return self.value

    def __exit__(self, exc_type, exc, tb):
        return False


def do_connect(args):
    ensure_plugin(args.plugin_dir, args.plugin_name)
    helper_exe = ensure_helper(args.helper_exe)
    password = resolve_password(args)

    display = args.display
    os.environ["DISPLAY"] = display

    socket_path = args.socket
    title = f"rdp2exec-{uuid.uuid4()}"
    env = dict(os.environ)
    env["RDP2EXEC_SOCKET"] = socket_path
    env["DISPLAY"] = display

    cols, rows = get_terminal_size()
    command = args.command if args.command else None
    with tempfile.TemporaryDirectory(prefix="rdp2exec-share-") if not args.share_dir else nullcontext(Path(args.share_dir)) as tmp:
        share_dir = Path(tmp) if isinstance(tmp, str) else tmp
        share_dir, run_dialog_command = prepare_drive_share(
            share_dir,
            helper_exe,
            args.child,
            args.drive_name,
            cols,
            rows,
            command=command,
        )

        cmd = build_xfreerdp_command(args, title, share_dir)
        debug_print(args.debug, "[rdp2exec] launching:", " ".join(shlex.quote(str(x)) for x in cmd), file=sys.stderr)

        with UnixSocketServer(socket_path) as server:
            popen_kwargs = {"env": env, "stdin": subprocess.PIPE}
            if args.debug:
                proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, **popen_kwargs)
                proc_logger = ProcessLogger(enabled=True, prefix="[xfreerdp] ")
                proc_logger.start(proc.stderr)
            else:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    **popen_kwargs,
                )
                proc_logger = ProcessLogger(enabled=False, prefix="[xfreerdp] ")
                proc_logger.start(proc.stderr)
            try:
                if proc.stdin is not None:
                    proc.stdin.write((password + "\n").encode("utf-8"))
                    proc.stdin.flush()
                    proc.stdin.close()
                time.sleep(args.bootstrap_delay)
                inject_command_via_run_dialog(
                    title=title,
                    command=run_dialog_command,
                    display=display,
                    window_timeout=args.window_timeout,
                    run_dialog_delay=args.run_dialog_delay,
                    type_delay_ms=args.type_delay_ms,
                )
                conn = server.accept(timeout=args.accept_timeout)
                try:
                    if command:
                        return command_bridge(conn, debug=args.debug)
                    return interactive_bridge(conn, debug=args.debug)
                finally:
                    conn.close()
            except Exception:
                if not args.debug and proc_logger is not None:
                    recent = [line for line in proc_logger.recent() if line.strip()]
                    if recent:
                        print("[rdp2exec] xfreerdp stderr (most recent):", file=sys.stderr)
                        for line in recent[-20:]:
                            print(f"[xfreerdp] {line}", file=sys.stderr)
                raise
            finally:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGTERM)
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                if proc_logger is not None:
                    proc_logger.join(timeout=1.0)


def parser():
    p = argparse.ArgumentParser(description="Linux FreeRDP rdp2exec ConPTY DVC PoC")
    p.add_argument("target", type=parse_target, help="Remote target in user@hostname format")
    p.add_argument("child", nargs="?", choices=["powershell", "cmd"], default="powershell")
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.add_argument("-p", "--port", type=int, default=int(os.environ.get("RDP_PORT", "3389")))
    p.add_argument("-P", "--password", default=os.environ.get("RDP_PASSWORD", ""))
    p.add_argument("-d", "--domain", default=os.environ.get("RDP_DOMAIN", ""))
    p.add_argument("--cert-ignore", action="store_true", default=True)
    p.add_argument("--xfreerdp", default=os.environ.get("XFREERDP", DEFAULT_XFREERDP))
    p.add_argument("--plugin-dir", default=os.environ.get("RDP2EXEC_PLUGIN_DIR", DEFAULT_PLUGIN_DIR))
    p.add_argument("--plugin-name", default=os.environ.get("RDP2EXEC_PLUGIN_NAME", DEFAULT_PLUGIN_NAME))
    p.add_argument("--helper-exe", default=os.environ.get("RDP2EXEC_HELPER_EXE", DEFAULT_HELPER_EXE))
    p.add_argument("--socket", default=os.environ.get("RDP2EXEC_SOCKET", DEFAULT_SOCKET))
    p.add_argument("--display", default=os.environ.get("DISPLAY", DEFAULT_DISPLAY))
    p.add_argument("--bootstrap-delay", type=float, default=float(os.environ.get("RDP2EXEC_BOOTSTRAP_DELAY", "8.0")))
    p.add_argument("--window-timeout", type=float, default=float(os.environ.get("RDP2EXEC_WINDOW_TIMEOUT", "45.0")))
    p.add_argument("--run-dialog-delay", type=float, default=float(os.environ.get("RDP2EXEC_RUN_DIALOG_DELAY", "0.8")))
    p.add_argument("--type-delay-ms", type=int, default=int(os.environ.get("RDP2EXEC_TYPE_DELAY_MS", "8")))
    p.add_argument("--accept-timeout", type=float, default=float(os.environ.get("RDP2EXEC_ACCEPT_TIMEOUT", "60.0")))
    p.add_argument("--drive-name", default=os.environ.get("RDP2EXEC_DRIVE_NAME", DEFAULT_DRIVE_NAME))
    p.add_argument("--share-dir", default=os.environ.get("RDP2EXEC_SHARE_DIR", ""))
    p.add_argument("--enable-clipboard", action="store_true", default=bool(int(os.environ.get("RDP2EXEC_ENABLE_CLIPBOARD", "0"))))
    p.add_argument("--debug", action="store_true", default=bool(int(os.environ.get("RDP2EXEC_DEBUG", "0"))))
    return p


def main():
    args = parser().parse_args()
    args.username, args.host = args.target
    raise SystemExit(do_connect(args) or 0)


if __name__ == "__main__":
    main()
