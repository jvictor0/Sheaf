"""Local launcher for sheaf API server and Chainlit UI."""

from __future__ import annotations

import os
import pathlib
import signal
import subprocess
import sys
import time
from typing import Tuple

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


def _consume_reboot_request(path: pathlib.Path) -> bool:
    if not path.exists():
        return False
    try:
        path.unlink()
    except OSError:
        # If the file is still present due to transient FS issues, treat as pending.
        return True
    return True


def main() -> int:
    api_port = os.getenv("SHEAF_PORT", "2731")
    ui_port = os.getenv("SHEAF_CHAINLIT_PORT", "2732")
    host = os.getenv("SHEAF_HOST", "127.0.0.1")
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

    def _shutdown(_signum: int, _frame: object) -> None:
        _terminate(ui_proc)
        _terminate(api_proc)
        raise SystemExit(0)

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    print(f"sheaf API  -> http://{host}:{api_port}")
    print(f"chainlit   -> http://{host}:{ui_port}")
    print(f"reboot API -> http://{host}:{api_port}/admin/reboot")

    try:
        while True:
            if _consume_reboot_request(reboot_file):
                print("reboot requested, restarting API and Chainlit...")
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
                continue

            if api_proc.poll() is not None:
                _terminate(ui_proc)
                return int(api_proc.returncode or 0)
            if ui_proc.poll() is not None:
                _terminate(api_proc)
                return int(ui_proc.returncode or 0)
            time.sleep(0.5)
    finally:
        _terminate(ui_proc)
        _terminate(api_proc)


if __name__ == "__main__":
    raise SystemExit(main())
