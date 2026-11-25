#!/usr/bin/env bash
# Publishes this fork's MapLibre Android AAR for the Cumbly app.
#
#   scripts/publish-cumbly.sh local         # into ~/.m2/repository (Maven Local)
#   scripts/publish-cumbly.sh artifactory   # into Lunabee's Artifactory, libs-release-local
#
# Options:
#   --flavor opengl|vulkan   renderer flavor (default: opengl)
#   --abis "<list>"          space-separated ABIs, e.g. "arm64-v8a x86_64", or "all" (default: all)
#
# The version is `<platform/android/VERSION>-cumbly-<short HEAD sha>`, e.g. `13.6.1-cumbly-7843af5`,
# and the artifact is `org.maplibre.gl:android-sdk-<flavor>`. A local build of a tree with
# uncommitted changes gets a `-dirty` suffix; an Artifactory build refuses one, and refuses a
# version already there, so every published version maps to one commit.
#
# Artifactory credentials: ARTIFACTORY_DEPLOYER_USERNAME and ARTIFACTORY_DEPLOYER_API_KEY when set,
# otherwise `artifactory_deployer_release_username` and `artifactory_deployer_release_api_key`
# from ~/.gradle/gradle.properties (the ones Lunabee's library projects publish with).
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
        local | artifactory) target="$1" ;;
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

# A key's value in ~/.gradle/gradle.properties (or $GRADLE_USER_HOME's), empty when absent.
gradle_property() {
    local file="${GRADLE_USER_HOME:-$HOME/.gradle}/gradle.properties"
    [[ -f "$file" ]] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*[=:][[:space:]]*//p" "$file" | head -n 1 | tr -d '\r'
}

android_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$android_dir"

sha="$(git rev-parse --short=7 HEAD)"
version="$(tr -d '[:space:]' < VERSION)-cumbly-${sha}"
if [[ -n "$(git status --porcelain --untracked-files=no -- "$(git rev-parse --show-toplevel)")" ]]; then
    if [[ "$target" == "artifactory" ]]; then
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
    artifactory)
        export MAVEN_REPOSITORY_URL="https://artifactory.lunabee.studio/artifactory/libs-release-local"
        MAVEN_REPOSITORY_USERNAME="${ARTIFACTORY_DEPLOYER_USERNAME:-$(gradle_property artifactory_deployer_release_username)}"
        MAVEN_REPOSITORY_PASSWORD="${ARTIFACTORY_DEPLOYER_API_KEY:-$(gradle_property artifactory_deployer_release_api_key)}"
        if [[ -z "$MAVEN_REPOSITORY_USERNAME" || -z "$MAVEN_REPOSITORY_PASSWORD" ]]; then
            echo "no Artifactory deployer credentials: set artifactory_deployer_release_username and" >&2
            echo "artifactory_deployer_release_api_key in ~/.gradle/gradle.properties" >&2
            exit 1
        fi
        export MAVEN_REPOSITORY_USERNAME MAVEN_REPOSITORY_PASSWORD
        pom="${MAVEN_REPOSITORY_URL}/org/maplibre/gl/android-sdk-${flavor}/${version}/android-sdk-${flavor}-${version}.pom"
        # -K - keeps the credentials off the command line.
        status="$(curl -s -o /dev/null -w '%{http_code}' -K - "$pom" <<< "user = \"${MAVEN_REPOSITORY_USERNAME}:${MAVEN_REPOSITORY_PASSWORD}\"")"
        case "$status" in
            200) echo "${version} is already on Artifactory: commit something new to publish again" >&2; exit 1 ;;
            404) ;;
            *) echo "Artifactory answered HTTP ${status} for ${pom}: check the credentials" >&2; exit 1 ;;
        esac
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
