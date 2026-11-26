import contextlib
import os
import signal
import subprocess
import time
from collections.abc import Callable, Iterator
from pathlib import Path

from mln.common.errors import MlnError
from mln.common.output import ChildLine, OnLine

FOLLOW_POLL_SECONDS = 0.3


def read_pid(pid_path: Path) -> int | None:
    try:
        return int(pid_path.read_text().strip())
    except (OSError, ValueError):
        return None


def pid_alive(pid_path: Path) -> int | None:
    """The tracked pid if the process is still alive, else None (and the pidfile is dropped)."""
    pid = read_pid(pid_path)
    if pid is None:
        return None
    with contextlib.suppress(ChildProcessError):
        reaped, _ = os.waitpid(pid, os.WNOHANG)
        if reaped == pid:
            pid_path.unlink(missing_ok=True)
            return None
    try:
        os.kill(pid, 0)
    except OSError:
        pid_path.unlink(missing_ok=True)
        return None
    return pid


def kill_group(pid_path: Path) -> int | None:
    """SIGTERM the tracked process group and drop the pidfile. Returns the pid it hit."""
    pid = pid_alive(pid_path)
    if pid is not None:
        try:
            os.killpg(os.getpgid(pid), signal.SIGTERM)
        except OSError:
            with contextlib.suppress(OSError):
                os.kill(pid, signal.SIGTERM)
    pid_path.unlink(missing_ok=True)
    return pid


def spawn_detached(
    argv: list[str], *, cwd: Path, env: dict[str, str], log_path: Path, pid_path: Path
) -> int:
    """Start `argv` in its own session, output truncated into `log_path`, and record its pid."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w") as handle:
        process = subprocess.Popen(
            argv,
            cwd=str(cwd),
            env=env,
            stdout=handle,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )
    pid_path.write_text(str(process.pid))
    return process.pid


def capture(command: list[str], *, cwd: Path, timeout: float = 30) -> str:
    """stdout of a quick command; a failure is an MlnError carrying its last stderr line."""
    try:
        result = subprocess.run(
            command, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False
        )
    except FileNotFoundError as error:
        raise MlnError(f"{command[0]} not found") from error
    except subprocess.TimeoutExpired as error:
        raise MlnError(f"{command[0]} timed out") from error
    if result.returncode != 0:
        detail = (result.stderr.strip().splitlines() or [f"exit {result.returncode}"])[-1]
        raise MlnError(f"{Path(command[0]).name}: {detail}")
    return result.stdout


def stream(command: list[str], *, cwd: Path, env: dict[str, str], on_line: OnLine) -> int:
    """Run `command` to completion, each output line handed to `on_line`; returns the exit code."""
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
    except FileNotFoundError as error:
        raise MlnError(f"{command[0]} not found") from error
    with process:
        assert process.stdout is not None
        try:
            for line in process.stdout:
                on_line(ChildLine(line.rstrip("\n")))
        except BaseException:
            process.terminate()
            raise
        return process.wait()


def tail_lines(path: Path, count: int) -> list[str]:
    try:
        return path.read_text(errors="replace").splitlines()[-count:]
    except OSError:
        return []


def follow(path: Path, cancelled: Callable[[], bool], count: int = 50) -> Iterator[str]:
    """`tail -f` as a generator: the last `count` lines, then each new one until `cancelled()`."""
    yield from tail_lines(path, count)
    try:
        handle = path.open(errors="replace")
    except OSError:
        return
    with handle:
        handle.seek(0, os.SEEK_END)
        while not cancelled():
            line = handle.readline()
            if line:
                yield line.rstrip("\n")
            else:
                time.sleep(FOLLOW_POLL_SECONDS)
