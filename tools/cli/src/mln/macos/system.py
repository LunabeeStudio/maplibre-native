import os
from collections.abc import Sequence
from pathlib import Path

from mln.common import proc, repo
from mln.common.errors import MlnError
from mln.common.output import OnLine
from mln.macos import core

SDK_HOME = Path.home() / "VulkanSDK"


def sdk() -> Path | None:
    installed = [p.name for p in SDK_HOME.iterdir()] if SDK_HOME.is_dir() else []
    return core.pick_sdk(os.environ.get("VULKAN_SDK"), SDK_HOME, installed)


def binary(backend_key: str) -> Path:
    return core.binary(repo.REPO_ROOT, core.backend(backend_key))


def log_path(backend_key: str) -> Path:
    return repo.run_dir() / core.log_name(core.backend(backend_key))


def _pid_path(backend_key: str) -> Path:
    return repo.run_dir() / core.pid_name(core.backend(backend_key))


def _label_path(backend_key: str) -> Path:
    return _pid_path(backend_key).with_suffix(".label")


def running(backend_key: str) -> tuple[int, str] | None:
    """The pid and label (`vulkan · kosmickrisp`) of the app the dashboard started, if alive."""
    pid = proc.pid_alive(_pid_path(backend_key))
    if pid is None:
        return None
    try:
        label = _label_path(backend_key).read_text().strip()
    except OSError:
        label = backend_key
    return pid, label


def build(backend_key: str, on_line: OnLine) -> Path:
    backend = core.backend(backend_key)
    env = dict(os.environ)
    if core.needs_configure(repo.REPO_ROOT, backend):
        on_line(f"configuring {core.build_dir(repo.REPO_ROOT, backend).name}")
        code = proc.stream(
            core.configure_command(backend), cwd=repo.REPO_ROOT, env=env, on_line=on_line
        )
        if code != 0:
            raise MlnError(f"cmake --preset {backend.preset} exited {code}")
    on_line(f"building {core.GLFW_TARGET} ({backend.key})")
    code = proc.stream(
        core.build_command(repo.REPO_ROOT, backend), cwd=repo.REPO_ROOT, env=env, on_line=on_line
    )
    if code != 0:
        raise MlnError(f"cmake --build exited {code}")
    return binary(backend_key)


def _prepare(
    backend_key: str,
    driver: str,
    style: str | None,
    extra: Sequence[str],
    build_first: bool,
    on_line: OnLine,
) -> tuple[list[str], dict[str, str]]:
    backend = core.backend(backend_key)
    core.check_driver(driver)
    env = core.run_env(os.environ, backend, sdk() if backend.key == "vulkan" else None, driver)
    if backend.key == "vulkan" and not Path(env["VK_DRIVER_FILES"]).is_file():
        raise MlnError(f"{env['VK_DRIVER_FILES']} is missing: this SDK does not ship {driver}")
    path = build(backend_key, on_line) if build_first else binary(backend_key)
    if not path.is_file():
        raise MlnError(f"{path} is missing: build it first (mln macos build)")
    for note in core.run_notes(backend, driver, env):
        on_line(note)
    return core.run_command(path, style, extra), env


def run_foreground(
    backend_key: str,
    driver: str,
    style: str | None,
    extra: Sequence[str],
    build_first: bool,
    on_line: OnLine,
) -> int:
    """Runs the app until its window closes, its output streamed to `on_line`."""
    command, env = _prepare(backend_key, driver, style, extra, build_first, on_line)
    return proc.stream(command, cwd=repo.REPO_ROOT, env=env, on_line=on_line)


def start(
    backend_key: str, driver: str, style: str | None, build_first: bool, on_line: OnLine
) -> int:
    """Starts the app detached, its output in `log_path`; stops a previous one of that backend."""
    command, env = _prepare(backend_key, driver, style, (), build_first, on_line)
    stop(backend_key, on_line)
    pid = proc.spawn_detached(
        command,
        cwd=repo.REPO_ROOT,
        env=env,
        log_path=log_path(backend_key),
        pid_path=_pid_path(backend_key),
    )
    label = core.label(core.backend(backend_key), driver)
    _label_path(backend_key).write_text(label)
    on_line(f"started {core.GLFW_TARGET} ({label}), pid {pid}")
    return pid


def stop(backend_key: str, on_line: OnLine) -> None:
    pid = proc.kill_group(_pid_path(backend_key))
    _label_path(backend_key).unlink(missing_ok=True)
    if pid is not None:
        on_line(f"stopped {core.GLFW_TARGET} ({backend_key}), pid {pid}")
