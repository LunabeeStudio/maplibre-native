#!/usr/bin/env bash
# Publishes this fork's MapLibre Android AAR for the Cumbly app.
#
#   scripts/publish-cumbly.sh local    # into ~/.m2/repository (Maven Local)
#   scripts/publish-cumbly.sh github   # into GitHub Packages of LunabeeStudio/maplibre-native
#
# Options:
#   --flavor opengl|vulkan   renderer flavor (default: opengl)
#   --abis "<list>"          space-separated ABIs, e.g. "arm64-v8a x86_64", or "all" (default: all)
#
# The version is `<platform/android/VERSION>-cumbly-<short HEAD sha>`, e.g. `13.6.1-cumbly-7843af5`,
# and the artifact is `org.maplibre.gl:android-sdk-<flavor>`. A local build of a tree with
# uncommitted changes gets a `-dirty` suffix; a GitHub build refuses one, so every published
# version maps to a commit.
#
# GitHub credentials: GITHUB_ACTOR and GITHUB_TOKEN when set, otherwise the `gh` CLI login.
# The token needs the `write:packages` scope (`gh auth refresh -s write:packages`).
set -euo pipefail

usage() {
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

target=""
flavor="opengl"
abis="all"
while [[ $# -gt 0 ]]; do
    case "$1" in
        local | github) target="$1" ;;
        --flavor) flavor="${2:?--flavor needs a value}"; shift ;;
        --abis) abis="${2:?--abis needs a value}"; shift ;;
        -h | --help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done
[[ -n "$target" ]] || usage 1

case "$flavor" in
    opengl) publication="Openglrelease" ;;
    vulkan) publication="Vulkanrelease" ;;
    *) echo "unsupported flavor: $flavor (opengl or vulkan)" >&2; exit 1 ;;
esac

android_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$android_dir"

sha="$(git rev-parse --short=7 HEAD)"
version="$(tr -d '[:space:]' < VERSION)-cumbly-${sha}"
if [[ -n "$(git status --porcelain --untracked-files=no -- "$(git rev-parse --show-toplevel)")" ]]; then
    if [[ "$target" == "github" ]]; then
        echo "uncommitted changes: commit them first, so the published version maps to a commit" >&2
        exit 1
    fi
    version="${version}-dirty"
fi

case "$target" in
    local)
        export MAVEN_REPOSITORY_URL="file://${HOME}/.m2/repository"
        unset MAVEN_REPOSITORY_USERNAME MAVEN_REPOSITORY_PASSWORD
        ;;
    github)
        export MAVEN_REPOSITORY_URL="https://maven.pkg.github.com/LunabeeStudio/maplibre-native"
        MAVEN_REPOSITORY_USERNAME="${GITHUB_ACTOR:-$(gh api user --jq .login)}"
        MAVEN_REPOSITORY_PASSWORD="${GITHUB_TOKEN:-$(gh auth token)}"
        export MAVEN_REPOSITORY_USERNAME MAVEN_REPOSITORY_PASSWORD
        ;;
esac

echo "publishing org.maplibre.gl:android-sdk-${flavor}:${version} (ABIs: ${abis}) to ${MAVEN_REPOSITORY_URL}"
# Credentials travel through the environment (read by maplibre.gradle-publish), never the command line.
./gradlew ":MapLibreAndroid:publish${publication}PublicationToConfiguredRepository" \
    "-Pmaplibre.versionName=${version}" \
    "-Pmaplibre.abis=${abis}" \
    --console=plain

echo
echo "published. In Cumbly, set in gradle/libs.versions.toml:"
echo "  maplibre = \"${version}\""
