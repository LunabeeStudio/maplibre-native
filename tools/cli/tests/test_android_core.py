from pathlib import Path

import pytest

from mln.android import core
from mln.common.errors import MlnError


def test_abis_default_to_all_and_accept_commas():
    assert core.gradle_abis(None) == "all"
    assert core.gradle_abis("  ") == "all"
    assert core.gradle_abis("arm64-v8a, x86_64") == "arm64-v8a x86_64"


def test_build_command_names_the_flavor_and_type_task():
    assert core.build_command("vulkan", True, "arm64-v8a") == [
        "./gradlew",
        ":MapLibreAndroid:assembleVulkanRelease",
        "-Pmaplibre.abis=arm64-v8a",
        "--console=plain",
    ]


def test_aar_path():
    path = core.aar_path(Path("/a"), "opengl", False)
    assert path == Path("/a/MapLibreAndroid/build/outputs/aar/MapLibreAndroid-opengl-debug.aar")


def test_publish_command_goes_through_the_script():
    assert core.publish_command("artifactory", "vulkan", None) == [
        "scripts/publish-cumbly.sh",
        "artifactory",
        "--flavor",
        "vulkan",
        "--abis",
        "all",
    ]


def test_published_version_reads_the_pin_line():
    lines = ["BUILD SUCCESSFUL", "published. In Cumbly, set:", '  maplibre = "13.7.1-cumbly-1-abc"']
    assert core.published_version(lines) == "13.7.1-cumbly-1-abc"
    assert core.published_version(["BUILD FAILED"]) is None


def test_maven_local_dir():
    path = core.maven_local_dir(Path("/m2"), "vulkan", "1.0")
    assert path == Path("/m2/org/maplibre/gl/android-sdk-vulkan/1.0")


def test_unknown_flavor_and_target_are_refused():
    with pytest.raises(MlnError):
        core.check_flavor("metal")
    with pytest.raises(MlnError):
        core.check_target("maven")


def test_artifactory_refuses_a_dirty_version_local_does_not():
    with pytest.raises(MlnError, match="uncommitted"):
        core.check_publishable("artifactory", "1.0-cumbly-1-abc-dirty")
    core.check_publishable("local", "1.0-cumbly-1-abc-dirty")
    core.check_publishable("artifactory", "1.0-cumbly-1-abc")
