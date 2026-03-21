"""Local launcher for sheaf API server."""

from __future__ import annotations

import pathlib
import signal
import subprocess
import sys
import time
import json

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_API_PORT = 2731

if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from sheaf.config.settings import REBOOT_REQUEST_FILE


def _terminate(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def _spawn_children(
    *,
    root: pathlib.Path,
    src: pathlib.Path,
    host: str,
    api_port: str,
) -> subprocess.Popen[bytes]:
    api_cmd = [
        sys.executable,
        "-m",
        "uvicorn",
        "sheaf.server.app:app",
        "--reload",
        "--host",
        host,
        "--port",
        api_port,
        "--app-dir",
        str(src),
    ]
    api_proc = subprocess.Popen(api_cmd, cwd=str(root))
    return api_proc


def _consume_reboot_request(path: pathlib.Path) -> bool:
    if not path.exists():
        return False
    try:
        path.unlink()
    except OSError:
        # If the file is still present due to transient FS issues, treat as pending.
        return True
    return True
def _parse_port(value: object, default: int, key_name: str) -> str:
    if value is None:
        return str(default)
    try:
        port = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"Invalid {key_name}: {value!r}") from exc
    if not (1 <= port <= 65535):
        raise ValueError(f"{key_name} must be between 1 and 65535: {port}")
    return str(port)


def _load_server_runtime_config(config_path: pathlib.Path) -> tuple[str, str]:
    if not config_path.exists():
        return DEFAULT_HOST, str(DEFAULT_API_PORT)

    raw = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"Invalid config format in {config_path}: expected JSON object")

    server = raw.get("server", {})
    if server is None:
        server = {}
    if not isinstance(server, dict):
        raise ValueError(f"Invalid server config in {config_path}: expected object")

    host_raw = server.get("host", DEFAULT_HOST)
    host = host_raw.strip() if isinstance(host_raw, str) else str(host_raw)
    if not host:
        host = DEFAULT_HOST

    api_port = _parse_port(server.get("api_port"), DEFAULT_API_PORT, "server.api_port")
    return host, api_port


def main() -> int:
    config_path = (ROOT / "sheaf_server.config").resolve()
    host, api_port = _load_server_runtime_config(config_path)
    reboot_file = REBOOT_REQUEST_FILE.resolve()
    reboot_file.parent.mkdir(parents=True, exist_ok=True)

    _consume_reboot_request(reboot_file)
    api_proc = _spawn_children(
        root=ROOT,
        src=SRC,
        host=host,
        api_port=api_port,
    )

    def _shutdown(_signum: int, _frame: object) -> None:
        _terminate(api_proc)
        raise SystemExit(0)

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    print(f"sheaf API  -> http://{host}:{api_port}")
    print(f"chat entry -> http://{host}:{api_port}/threads/<thread_id>/enter-chat")

    try:
        while True:
            if _consume_reboot_request(reboot_file):
                print("reboot requested, restarting API...")
                _terminate(api_proc)
                time.sleep(0.4)
                api_proc = _spawn_children(
                    root=ROOT,
                    src=SRC,
                    host=host,
                    api_port=api_port,
                )
                continue

            if api_proc.poll() is not None:
                return int(api_proc.returncode or 0)
            time.sleep(0.5)
    finally:
        _terminate(api_proc)


if __name__ == "__main__":
    raise SystemExit(main())
