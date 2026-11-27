#!/usr/bin/env bash
# Publishes this fork's MapLibre Android AAR for the Cumbly app.
#
#   scripts/publish-cumbly.sh local         # into ~/.m2/repository (Maven Local)
#   scripts/publish-cumbly.sh artifactory   # into Lunabee's Artifactory, maplibre-native-local
#
# Options:
#   --flavor opengl|vulkan   renderer flavor (default: vulkan)
#   --abis "<list>"          space-separated ABIs, e.g. "arm64-v8a x86_64", or "all" (default: all)
#   --print-version          print the version a local publish would get, and exit
#
# The version is `<platform/android/VERSION>-<HEAD commit time, Unix seconds>-cumbly-<short HEAD sha>`,
# e.g. `13.8.0-1795767599-cumbly-7de4555`, so versions of one VERSION sort by commit time, above the
# plain upstream release of that number under both Gradle and Maven ordering (the 10-digit time
# segment comes first), and the artifact is `org.maplibre.gl:android-sdk-<flavor>`. A local build of a
# tree with uncommitted changes gets a `-dirty` suffix; an Artifactory build refuses one, and refuses a
# version already there, so every published version maps to one commit.
#
# An Artifactory publish also pushes the tag `cumbly-android-<version>` on HEAD (`cumbly-android-<flavor>-<version>`
# for a flavor other than vulkan), the twin of the iOS release tag `cumbly-ios-<version>`: it refuses a HEAD
# that is not on origin/cumbly, or a tag that exists.
#
# Artifactory credentials: ARTIFACTORY_DEPLOYER_USERNAME and ARTIFACTORY_DEPLOYER_API_KEY when set,
# otherwise `artifactory_deployer_release_username` and `artifactory_deployer_release_api_key`
# from ~/.gradle/gradle.properties (the ones Lunabee's library projects publish with).
set -euo pipefail

usage() {
    awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"
    exit "${1:-0}"
}

target=""
flavor="vulkan"
abis="all"
print_version=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        local | artifactory) target="$1" ;;
        --flavor) flavor="${2:?--flavor needs a value}"; shift ;;
        --abis) abis="${2:?--abis needs a value}"; shift ;;
        --print-version) print_version=true ;;
        -h | --help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done
[[ -n "$target" || "$print_version" == true ]] || usage 1

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
# Committer time, not author time: a rebase keeps the author time but gives each commit a new
# committer time, so a rebased commit still sorts after everything published before it.
timestamp="$(git log -1 --format=%ct HEAD)"
version="$(tr -d '[:space:]' < VERSION)-${timestamp}-cumbly-${sha}"
if [[ -n "$(git --no-optional-locks status --porcelain --untracked-files=no -- "$(git rev-parse --show-toplevel)")" ]]; then
    if [[ "$target" == "artifactory" ]]; then
        echo "uncommitted changes: commit them first, so the published version maps to a commit" >&2
        exit 1
    fi
    version="${version}-dirty"
fi
if [[ "$print_version" == true ]]; then
    echo "$version"
    exit 0
fi
if [[ "$flavor" == "vulkan" ]]; then
    tag="cumbly-android-${version}"
else
    tag="cumbly-android-${flavor}-${version}"
fi

case "$target" in
    local)
        export MAVEN_REPOSITORY_URL="file://${HOME}/.m2/repository"
        unset MAVEN_REPOSITORY_USERNAME MAVEN_REPOSITORY_PASSWORD
        ;;
    artifactory)
        export MAVEN_REPOSITORY_URL="https://artifactory.lunabee.studio/artifactory/maplibre-native-local"
        MAVEN_REPOSITORY_USERNAME="${ARTIFACTORY_DEPLOYER_USERNAME:-$(gradle_property artifactory_deployer_release_username)}"
        MAVEN_REPOSITORY_PASSWORD="${ARTIFACTORY_DEPLOYER_API_KEY:-$(gradle_property artifactory_deployer_release_api_key)}"
        if [[ -z "$MAVEN_REPOSITORY_USERNAME" || -z "$MAVEN_REPOSITORY_PASSWORD" ]]; then
            echo "no Artifactory deployer credentials: set artifactory_deployer_release_username and" >&2
            echo "artifactory_deployer_release_api_key in ~/.gradle/gradle.properties" >&2
            exit 1
        fi
        export MAVEN_REPOSITORY_USERNAME MAVEN_REPOSITORY_PASSWORD
        git fetch --quiet origin cumbly
        if ! git merge-base --is-ancestor HEAD origin/cumbly; then
            echo "HEAD ${sha} is not on origin/cumbly: merge it first" >&2
            exit 1
        fi
        if ! remote_tag="$(git ls-remote --tags origin "refs/tags/${tag}")"; then
            echo "cannot check whether ${tag} exists on origin" >&2
            exit 1
        elif [[ -n "$remote_tag" ]]; then
            echo "${tag} already exists on origin: commit something new to publish again" >&2
            exit 1
        fi
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

if [[ "$target" == "artifactory" ]]; then
    git tag -f "$tag" HEAD > /dev/null
    if ! git push --quiet origin "refs/tags/${tag}"; then
        echo "published, but pushing ${tag} failed: git push origin refs/tags/${tag}" >&2
        exit 1
    fi
    echo "tagged ${tag}"
fi

echo
echo "published. In Cumbly, set in gradle/libs.versions.toml:"
echo "  maplibre = \"${version}\""
