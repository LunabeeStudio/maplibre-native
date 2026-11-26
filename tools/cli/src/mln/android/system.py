import os
from pathlib import Path

from mln.android import core
from mln.common import proc, repo
from mln.common.errors import MlnError
from mln.common.output import OnLine


def m2_repository() -> Path:
    return Path.home() / ".m2" / "repository"


def next_version() -> str:
    """The version a local publish of this checkout gets now (`-dirty` with uncommitted changes)."""
    return proc.capture(core.version_command(), cwd=repo.ANDROID_DIR).strip()


def in_maven_local(flavor: str, version: str) -> bool:
    return core.maven_local_dir(m2_repository(), flavor, version).is_dir()


def aar(flavor: str, release: bool) -> Path:
    return core.aar_path(repo.ANDROID_DIR, flavor, release)


def build(flavor: str, release: bool, abis: str | None, on_line: OnLine) -> Path:
    core.check_flavor(flavor)
    kind = core.build_type(release)
    on_line(f"building {core.artifact(flavor)} {kind} (ABIs: {core.gradle_abis(abis)})")
    code = proc.stream(
        core.build_command(flavor, release, abis),
        cwd=repo.ANDROID_DIR,
        env=dict(os.environ),
        on_line=on_line,
    )
    if code != 0:
        raise MlnError(f"gradle exited {code}")
    path = aar(flavor, release)
    if not path.is_file():
        raise MlnError(f"gradle succeeded but {path} is missing")
    return path


def publish(target: str, flavor: str, abis: str | None, on_line: OnLine) -> str:
    """Runs the publish script, the one source of truth for versions and credentials."""
    core.check_target(target)
    core.check_flavor(flavor)
    seen: list[str] = []

    def relay(line: str) -> None:
        seen.append(line)
        on_line(line)

    code = proc.stream(
        core.publish_command(target, flavor, abis),
        cwd=repo.ANDROID_DIR,
        env=dict(os.environ),
        on_line=relay,
    )
    if code != 0:
        raise MlnError(f"publish to {target} failed (exit {code})")
    version = core.published_version(seen)
    if version is None:
        raise MlnError("publish finished without printing a version")
    return version
