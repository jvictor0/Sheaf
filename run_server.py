"""Local launcher for sheaf API server, Chainlit UI, and optional Zulip poll bot."""

from __future__ import annotations

import json
import os
import pathlib
import signal
import subprocess
import sys
import time
from typing import Optional, Tuple

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_API_PORT = 2731
DEFAULT_CHAINLIT_PORT = 2732

if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))


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
    env: dict[str, str],
    host: str,
    api_port: str,
    ui_port: str,
) -> Tuple[subprocess.Popen[bytes], subprocess.Popen[bytes]]:
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
    ui_cmd = [
        sys.executable,
        "-m",
        "chainlit",
        "run",
        str(root / "chainlit_app.py"),
        "-w",
        "--headless",
        "--host",
        host,
        "--port",
        ui_port,
    ]
    api_proc = subprocess.Popen(api_cmd, cwd=str(root), env=env)
    ui_proc = subprocess.Popen(ui_cmd, cwd=str(root), env=env)
    return api_proc, ui_proc


def _spawn_zulip_bot(
    *,
    root: pathlib.Path,
    env: dict[str, str],
    config_path: pathlib.Path,
) -> subprocess.Popen[bytes]:
    bot_cmd = [
        sys.executable,
        str(root / "scripts" / "zulip_poll_bot.py"),
        "--config",
        str(config_path),
    ]
    return subprocess.Popen(bot_cmd, cwd=str(root), env=env)


def _consume_reboot_request(path: pathlib.Path) -> bool:
    if not path.exists():
        return False
    try:
        path.unlink()
    except OSError:
        # If the file is still present due to transient FS issues, treat as pending.
        return True
    return True


def _zulip_bot_enabled_from_config(config_path: pathlib.Path) -> bool:
    if not config_path.exists():
        return False
    raw = json.loads(config_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        return False
    zulip_enabled = raw.get("zulip_enabled", False)
    return isinstance(zulip_enabled, bool) and zulip_enabled


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


def _load_server_runtime_config(config_path: pathlib.Path) -> tuple[str, str, str]:
    if not config_path.exists():
        return DEFAULT_HOST, str(DEFAULT_API_PORT), str(DEFAULT_CHAINLIT_PORT)

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
    chainlit_port = _parse_port(
        server.get("chainlit_port"),
        DEFAULT_CHAINLIT_PORT,
        "server.chainlit_port",
    )
    return host, api_port, chainlit_port


def main() -> int:
    config_path = (ROOT / "sheaf_server.config").resolve()
    host, api_port, ui_port = _load_server_runtime_config(config_path)
    start_zulip_bot = _zulip_bot_enabled_from_config(config_path)
    runtime_dir = ROOT / ".runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    reboot_file = (runtime_dir / "reboot.request").resolve()

    env = os.environ.copy()
    env["PYTHONPATH"] = f"{SRC}{os.pathsep}{env.get('PYTHONPATH', '')}".rstrip(os.pathsep)
    env["SHEAF_REBOOT_FILE"] = str(reboot_file)

    _consume_reboot_request(reboot_file)
    api_proc, ui_proc = _spawn_children(
        root=ROOT,
        src=SRC,
        env=env,
        host=host,
        api_port=api_port,
        ui_port=ui_port,
    )
    bot_proc: Optional[subprocess.Popen[bytes]] = None
    if start_zulip_bot:
        bot_proc = _spawn_zulip_bot(root=ROOT, env=env, config_path=config_path)

    def _shutdown(_signum: int, _frame: object) -> None:
        if bot_proc is not None:
            _terminate(bot_proc)
        _terminate(ui_proc)
        _terminate(api_proc)
        raise SystemExit(0)

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    print(f"sheaf API  -> http://{host}:{api_port}")
    print(f"chainlit   -> http://{host}:{ui_port}")
    print(f"reboot API -> http://{host}:{api_port}/admin/reboot")
    if start_zulip_bot:
        print(f"zulip bot  -> enabled ({config_path})")
    else:
        print(f"zulip bot  -> disabled (set \"zulip_enabled\": true in {config_path})")

    try:
        while True:
            if _consume_reboot_request(reboot_file):
                print("reboot requested, restarting API and Chainlit...")
                if bot_proc is not None:
                    _terminate(bot_proc)
                _terminate(ui_proc)
                _terminate(api_proc)
                time.sleep(0.4)
                api_proc, ui_proc = _spawn_children(
                    root=ROOT,
                    src=SRC,
                    env=env,
                    host=host,
                    api_port=api_port,
                    ui_port=ui_port,
                )
                if start_zulip_bot:
                    bot_proc = _spawn_zulip_bot(
                        root=ROOT,
                        env=env,
                        config_path=config_path,
                    )
                continue

            if api_proc.poll() is not None:
                if bot_proc is not None:
                    _terminate(bot_proc)
                _terminate(ui_proc)
                return int(api_proc.returncode or 0)
            if ui_proc.poll() is not None:
                if bot_proc is not None:
                    _terminate(bot_proc)
                _terminate(api_proc)
                return int(ui_proc.returncode or 0)
            if bot_proc is not None and bot_proc.poll() is not None:
                _terminate(ui_proc)
                _terminate(api_proc)
                return int(bot_proc.returncode or 0)
            time.sleep(0.5)
    finally:
        if bot_proc is not None:
            _terminate(bot_proc)
        _terminate(ui_proc)
        _terminate(api_proc)


if __name__ == "__main__":
    raise SystemExit(main())
