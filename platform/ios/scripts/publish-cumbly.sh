#!/usr/bin/env bash
# Builds this fork's MapLibre iOS XCFramework for the Cumbly app, and releases it.
#
#   scripts/publish-cumbly.sh local                  # build, print the XCFramework path
#   scripts/publish-cumbly.sh release [--prerelease] # build, zip, publish a GitHub release on this fork
#
# Options:
#   --prerelease     mark the GitHub release as a pre-release (a try-out before the fork PR is merged)
#   --print-version  print the version a build of this commit would get, and exit
#
# The version is `<platform/ios/VERSION>-cumbly-<HEAD commit time, Unix seconds>-<short HEAD sha>`,
# e.g. `7.1.0-cumbly-1795700000-33ea4fe`, so versions of one VERSION sort by commit time. A local build of a
# tree with uncommitted changes gets a `-dirty` suffix; a release refuses one, and refuses a commit that is
# not on the remote, so every release maps to one pushed commit.
#
# The build is Bazel's `//platform/ios:MapLibre.dynamic` with the Metal renderer (arm64 device and arm64
# simulator). The release is tagged `cumbly-ios-<version>` (never colliding with upstream's `ios-v*` tags)
# and holds two assets: `MapLibre.xcframework.zip`, the XCFramework Swift Package Manager downloads for a
# `binaryTarget`, and `MapLibre.xcframework.zip.sha256`, its checksum. Needs `bazelisk`, and `gh` for a release.
set -euo pipefail

usage() {
    awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
    exit "${1:-0}"
}

repository="LunabeeStudio/maplibre-native"
target=""
prerelease=false
print_version=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        local | release) target="$1" ;;
        --prerelease) prerelease=true ;;
        --print-version) print_version=true ;;
        -h | --help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done
[[ -n "$target" || "$print_version" == true ]] || usage 1
if [[ "$prerelease" == true && "$target" != "release" ]]; then
    echo "--prerelease only applies to release" >&2
    exit 1
fi

ios_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ios_dir"
root="$(git rev-parse --show-toplevel)"

sha="$(git rev-parse --short=7 HEAD)"
# Committer time, not author time: a rebase keeps the author time but gives each commit a new
# committer time, so a rebased commit still sorts after everything published before it.
timestamp="$(git log -1 --format=%ct HEAD)"
version="$(tr -d '[:space:]' < VERSION)-cumbly-${timestamp}-${sha}"
if [[ -n "$(git --no-optional-locks status --porcelain --untracked-files=no -- "$root")" ]]; then
    if [[ "$target" == "release" ]]; then
        echo "uncommitted changes: commit them first, so the release maps to a commit" >&2
        exit 1
    fi
    version="${version}-dirty"
fi
if [[ "$print_version" == true ]]; then
    echo "$version"
    exit 0
fi

tag="cumbly-ios-${version}"
if [[ "$target" == "release" ]]; then
    command -v gh > /dev/null || { echo "gh missing: brew install gh" >&2; exit 1; }
    if [[ -z "$(git branch --remotes --contains HEAD)" ]]; then
        echo "HEAD ${sha} is not on the remote: push it first, the release is built from a pushed commit" >&2
        exit 1
    fi
    if gh release view "$tag" --repo "$repository" > /dev/null 2>&1; then
        echo "${tag} already exists: commit something new to release again" >&2
        exit 1
    fi
fi

command -v bazelisk > /dev/null || { echo "bazelisk missing: brew install bazelisk" >&2; exit 1; }
config="${root}/platform/darwin/bazel/config.bzl"
[[ -f "$config" ]] || cp "${root}/platform/darwin/bazel/example_config.bzl" "$config"

echo "building MapLibre iOS ${version} (Metal, arm64 device and simulator)" >&2
(cd "$root" && bazelisk build //platform/ios:MapLibre.dynamic --compilation_mode=opt --//:renderer=metal) >&2
archive="${root}/bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip"
[[ -f "$archive" ]] || { echo "Bazel produced no ${archive}" >&2; exit 1; }

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
ditto -x -k "$archive" "${staging}/unpacked"
framework="${staging}/unpacked/MapLibre.xcframework"
[[ -d "$framework" ]] || { echo "${archive} holds no MapLibre.xcframework at its root" >&2; exit 1; }

if [[ "$target" == "local" ]]; then
    # Bazel's output is read-only and replaced by the next build, so hand out a copy that stays.
    out="${root}/build/cumbly-ios/${version}"
    rm -rf "$out"
    mkdir -p "$out"
    cp -R "$framework" "${out}/MapLibre.xcframework"
    echo "MapLibre iOS ${version}" >&2
    echo "${out}/MapLibre.xcframework"
    exit 0
fi

# What Swift Package Manager downloads: the XCFramework at the root of the zip, not Bazel's own wrapping.
zip_path="${staging}/MapLibre.xcframework.zip"
(cd "${staging}/unpacked" && ditto -c -k --keepParent MapLibre.xcframework "$zip_path")
checksum="$(shasum -a 256 "$zip_path" | cut -d ' ' -f 1)"
printf '%s\n' "$checksum" > "${zip_path}.sha256"

flags=()
[[ "$prerelease" == true ]] && flags+=(--prerelease)
gh release create "$tag" "$zip_path" "${zip_path}.sha256" \
    --repo "$repository" \
    --target "$(git rev-parse HEAD)" \
    --title "MapLibre iOS ${version}" \
    --notes "MapLibre Native iOS XCFramework (Metal, arm64 device and arm64 simulator, iOS 16+) built from commit ${sha} of the cumbly branch, for the Cumbly app." \
    ${flags[@]+"${flags[@]}"}

echo
echo "published ${tag}. In Cumbly, pin it:"
echo "  ./cumbly ios maplibre ${tag}"
