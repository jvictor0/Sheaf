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
    enabled = raw.get("enabled", False)
    return isinstance(enabled, bool) and enabled


def main() -> int:
    api_port = os.getenv("SHEAF_PORT", "2731")
    ui_port = os.getenv("SHEAF_CHAINLIT_PORT", "2732")
    host = os.getenv("SHEAF_HOST", "127.0.0.1")
    zulip_config_path = (ROOT / "zulip_bot.config.json").resolve()
    start_zulip_bot = _zulip_bot_enabled_from_config(zulip_config_path)
    runtime_dir = ROOT / ".runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    reboot_file = pathlib.Path(
        os.getenv("SHEAF_REBOOT_FILE", str(runtime_dir / "reboot.request"))
    ).resolve()

    env = os.environ.copy()
    env["PYTHONPATH"] = f"{SRC}{os.pathsep}{env.get('PYTHONPATH', '')}".rstrip(os.pathsep)
    env.setdefault("SHEAF_API_BASE_URL", f"http://{host}:{api_port}")
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
        bot_proc = _spawn_zulip_bot(root=ROOT, env=env, config_path=zulip_config_path)

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
        print(f"zulip bot  -> enabled ({zulip_config_path})")
    else:
        print(f"zulip bot  -> disabled (set \"enabled\": true in {zulip_config_path})")

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
                        config_path=zulip_config_path,
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
