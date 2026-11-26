from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from mln.common.errors import MlnError

GLFW_TARGET = "mbgl-glfw"
SDK_URL = "https://vulkan.lunarg.com/sdk/home"
API_KEY_ENV = "MLN_API_KEY"


@dataclass(frozen=True)
class Backend:
    key: str
    preset: str


BACKENDS: dict[str, Backend] = {
    "vulkan": Backend("vulkan", "macos-vulkan"),
    "metal": Backend("metal", "macos-metal"),
}
DEFAULT_BACKEND = "vulkan"

DRIVER_ICDS: dict[str, str] = {
    "kosmickrisp": "libkosmickrisp_icd.json",
    "moltenvk": "MoltenVK_icd.json",
}
DEFAULT_DRIVER = "kosmickrisp"

# Loader variables `setup-env.sh` exports that would override the one driver picked here.
_STALE_LOADER_VARS = ("VK_ICD_FILENAMES",)


def backend(key: str) -> Backend:
    if key not in BACKENDS:
        raise MlnError(f"unknown backend {key}: {' or '.join(BACKENDS)}")
    return BACKENDS[key]


def check_driver(driver: str) -> str:
    if driver not in DRIVER_ICDS:
        raise MlnError(f"unknown Vulkan driver {driver}: {' or '.join(DRIVER_ICDS)}")
    return driver


def build_dir(root: Path, backend: Backend) -> Path:
    return root / f"build-{backend.preset}"


def binary(root: Path, backend: Backend) -> Path:
    return build_dir(root, backend) / "platform" / "glfw" / GLFW_TARGET


def needs_configure(root: Path, backend: Backend) -> bool:
    return not (build_dir(root, backend) / "build.ninja").is_file()


def configure_command(backend: Backend) -> list[str]:
    return ["cmake", "--preset", backend.preset]


def build_command(root: Path, backend: Backend) -> list[str]:
    return ["cmake", "--build", str(build_dir(root, backend)), "--target", GLFW_TARGET]


def _version_key(name: str) -> tuple[int, ...]:
    return tuple(int(part) for part in name.split(".") if part.isdecimal())


def pick_sdk(env_sdk: str | None, sdk_home: Path, installed: Sequence[str]) -> Path | None:
    """`$VULKAN_SDK` when set, else the newest `~/VulkanSDK/<version>/macOS`."""
    if env_sdk:
        return Path(env_sdk)
    versions = sorted((name for name in installed if _version_key(name)), key=_version_key)
    return sdk_home / versions[-1] / "macOS" if versions else None


def icd_path(sdk: Path, driver: str) -> Path:
    return sdk / "share" / "vulkan" / "icd.d" / DRIVER_ICDS[check_driver(driver)]


def run_env(
    base: Mapping[str, str], backend: Backend, sdk: Path | None, driver: str
) -> dict[str, str]:
    """The GLFW app's environment: for Vulkan, the SDK's loader with exactly one driver."""
    env = dict(base)
    if backend.key != "vulkan":
        return env
    if sdk is None:
        raise MlnError(
            f"no LunarG Vulkan SDK in ~/VulkanSDK: install it from {SDK_URL},\n"
            "then check with scripts/vulkan-doctor.sh"
        )
    for name in _STALE_LOADER_VARS:
        env.pop(name, None)
    env["VULKAN_SDK"] = str(sdk)
    library_path = env.get("DYLD_LIBRARY_PATH", "")
    sdk_lib = str(sdk / "lib")
    if sdk_lib not in library_path.split(":"):
        env["DYLD_LIBRARY_PATH"] = f"{sdk_lib}:{library_path}" if library_path else sdk_lib
    env["VK_DRIVER_FILES"] = str(icd_path(sdk, driver))
    return env


def run_command(binary: Path, style: str | None, extra: Sequence[str]) -> list[str]:
    return [str(binary), *(["--style", style] if style else []), *extra]


def run_notes(backend: Backend, driver: str, env: Mapping[str, str]) -> list[str]:
    notes = []
    if backend.key == "vulkan":
        notes.append(f"Vulkan driver: {driver}")
        notes.append("press V in the window to add the SkyLayer and TrackLayer")
    if not env.get(API_KEY_ENV):
        notes.append(f"{API_KEY_ENV} is not set: MapTiler styles will not load")
    return notes


def label(backend: Backend, driver: str | None) -> str:
    return f"{backend.key} · {driver}" if backend.key == "vulkan" and driver else backend.key


def log_name(backend: Backend) -> str:
    return f"glfw-{backend.key}.log"


def pid_name(backend: Backend) -> str:
    return f"glfw-{backend.key}.pid"
