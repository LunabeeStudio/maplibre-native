from pathlib import Path

import pytest

from mln.common.errors import MlnError
from mln.macos import core

VULKAN = core.BACKENDS["vulkan"]
METAL = core.BACKENDS["metal"]
SDK = Path("/sdk/1.4/macOS")


def test_paths_follow_the_preset():
    assert core.binary(Path("/r"), VULKAN) == Path("/r/build-macos-vulkan/platform/glfw/mbgl-glfw")
    assert core.configure_command(METAL) == ["cmake", "--preset", "macos-metal"]
    assert core.build_command(Path("/r"), VULKAN) == [
        "cmake",
        "--build",
        "/r/build-macos-vulkan",
        "--target",
        "mbgl-glfw",
    ]


def test_pick_sdk_prefers_the_environment_then_the_newest_version():
    assert core.pick_sdk("/env/sdk", Path("/h"), ["1.4.1"]) == Path("/env/sdk")
    picked = core.pick_sdk(None, Path("/h"), ["1.3.290.0", "1.4.10.0", "1.4.9.1", "notes"])
    assert picked == Path("/h/1.4.10.0/macOS")
    assert core.pick_sdk(None, Path("/h"), []) is None


def test_vulkan_env_picks_one_driver_and_the_sdk_loader():
    base = {"VK_ICD_FILENAMES": "/old/", "DYLD_LIBRARY_PATH": "/x", "MLN_API_KEY": "k"}
    env = core.run_env(base, VULKAN, SDK, "moltenvk")
    assert env["VK_DRIVER_FILES"] == "/sdk/1.4/macOS/share/vulkan/icd.d/MoltenVK_icd.json"
    assert "VK_ICD_FILENAMES" not in env
    assert env["DYLD_LIBRARY_PATH"] == "/sdk/1.4/macOS/lib:/x"
    assert env["VULKAN_SDK"] == str(SDK)


def test_vulkan_env_keeps_the_sdk_lib_once():
    env = core.run_env({"DYLD_LIBRARY_PATH": "/sdk/1.4/macOS/lib:/x"}, VULKAN, SDK, "kosmickrisp")
    assert env["DYLD_LIBRARY_PATH"] == "/sdk/1.4/macOS/lib:/x"
    assert env["VK_DRIVER_FILES"].endswith("libkosmickrisp_icd.json")


def test_vulkan_without_sdk_is_refused_metal_is_untouched():
    with pytest.raises(MlnError, match="vulkan.lunarg.com"):
        core.run_env({}, VULKAN, None, "kosmickrisp")
    assert core.run_env({"A": "1"}, METAL, None, "kosmickrisp") == {"A": "1"}


def test_unknown_backend_and_driver_are_refused():
    with pytest.raises(MlnError):
        core.backend("opengl")
    with pytest.raises(MlnError):
        core.check_driver("swiftshader")


def test_run_command_and_notes():
    assert core.run_command(Path("/b"), "https://s", ["-z", "3"]) == [
        "/b",
        "--style",
        "https://s",
        "-z",
        "3",
    ]
    notes = core.run_notes(VULKAN, "kosmickrisp", {})
    assert "Vulkan driver: kosmickrisp" in notes
    assert any("MLN_API_KEY" in note for note in notes)
    assert core.run_notes(METAL, "kosmickrisp", {"MLN_API_KEY": "k"}) == []


def test_label():
    assert core.label(VULKAN, "moltenvk") == "vulkan · moltenvk"
    assert core.label(METAL, "moltenvk") == "metal"
