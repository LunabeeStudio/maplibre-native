import re
from collections.abc import Iterable
from pathlib import Path

from mln.common.errors import MlnError

FLAVORS = ("vulkan", "opengl")
DEFAULT_FLAVOR = "vulkan"
TARGETS = ("local", "artifactory")
ALL_ABIS = "all"
PUBLISH_SCRIPT = "scripts/publish-cumbly.sh"
ARTIFACT_GROUP_PATH = ("org", "maplibre", "gl")

_PIN = re.compile(r'^\s*maplibre = "(?P<version>[^"]+)"\s*$')


def check_flavor(flavor: str) -> str:
    if flavor not in FLAVORS:
        raise MlnError(f"unknown flavor {flavor}: {' or '.join(FLAVORS)}")
    return flavor


def check_target(target: str) -> str:
    if target not in TARGETS:
        raise MlnError(f"unknown publish target {target}: {' or '.join(TARGETS)}")
    return target


def gradle_abis(abis: str | None) -> str:
    """`-Pmaplibre.abis` takes space-separated ABIs; commas are accepted too."""
    if not abis or not abis.strip():
        return ALL_ABIS
    return " ".join(abis.replace(",", " ").split())


def build_type(release: bool) -> str:
    return "release" if release else "debug"


def build_command(flavor: str, release: bool, abis: str | None) -> list[str]:
    task = f":MapLibreAndroid:assemble{flavor.capitalize()}{build_type(release).capitalize()}"
    return ["./gradlew", task, f"-Pmaplibre.abis={gradle_abis(abis)}", "--console=plain"]


def aar_path(android_dir: Path, flavor: str, release: bool) -> Path:
    name = f"MapLibreAndroid-{flavor}-{build_type(release)}.aar"
    return android_dir / "MapLibreAndroid" / "build" / "outputs" / "aar" / name


def publish_command(target: str, flavor: str, abis: str | None) -> list[str]:
    return [PUBLISH_SCRIPT, target, "--flavor", flavor, "--abis", gradle_abis(abis)]


def version_command() -> list[str]:
    return [PUBLISH_SCRIPT, "--print-version"]


def artifact(flavor: str) -> str:
    return f"org.maplibre.gl:android-sdk-{flavor}"


def published_version(lines: Iterable[str]) -> str | None:
    """The version the publish script printed as the `libs.versions.toml` pin, if it got there."""
    found = None
    for line in lines:
        match = _PIN.match(line)
        if match:
            found = match.group("version")
    return found


def maven_local_dir(m2_repository: Path, flavor: str, version: str) -> Path:
    return m2_repository.joinpath(*ARTIFACT_GROUP_PATH, f"android-sdk-{flavor}", version)


def check_publishable(target: str, version: str) -> None:
    """The publish script refuses a dirty tree for Artifactory; say so before asking to confirm."""
    if target == "artifactory" and version.endswith("-dirty"):
        raise MlnError(
            "uncommitted changes: commit them first, so the published version maps to a commit"
        )
