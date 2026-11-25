#!/usr/bin/env bash
# Checks the host for what Vulkan work on this repo needs.
#
# Usage: scripts/vulkan-doctor.sh [--fix | --dry-run]
#   (no flag)   report only, installs nothing
#   --dry-run   report, then print the commands --fix would run
#   --fix       report, run those commands, then report again
#
# --fix only runs what is safe to automate: submodule checkout, sdkmanager packages, Homebrew formulae.
# It never touches the LunarG Vulkan SDK (GUI installer), MLN_API_KEY (a secret) or device authorization.

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SELF="scripts/vulkan-doctor.sh"
SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
NDK_VERSION="28.2.13676358"
SDK_CMAKE_VERSION="3.31.6"
LUNARG_URL="https://vulkan.lunarg.com/sdk/home"

mode="report"
case "${1:-}" in
    "") ;;
    --fix) mode="fix" ;;
    --dry-run) mode="dry-run" ;;
    -h | --help)
        sed -n '2,10s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
        exit 0
        ;;
    *)
        echo "usage: $SELF [--fix | --dry-run]" >&2
        exit 2
        ;;
esac

# Colors only on a terminal, and never when NO_COLOR is set.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' GREEN=$'\033[32m' YELLOW=$'\033[33m' RESET=$'\033[0m'
else
    BOLD="" DIM="" RED="" GREEN="" YELLOW="" RESET=""
fi

fixes=()          # commands --fix may run
needs_lunarg=0    # print the LunarG install note once, at the end
section_names=()  # summary rows
section_missing=()
section_purpose=()
current=-1

have() { command -v "$1" >/dev/null 2>&1; }

section() { # <name> <what it is for>
    current=$((current + 1))
    section_names+=("$1")
    section_purpose+=("$2")
    section_missing+=(0)
    printf '\n%s%s%s  %s%s%s\n' "$BOLD" "$1" "$RESET" "$DIM" "$2" "$RESET"
}

ok() { # <what> [detail]
    printf '  %s✓%s %s' "$GREEN" "$RESET" "$1"
    if [ -n "${2:-}" ]; then printf '  %s%s%s' "$DIM" "$2" "$RESET"; fi
    printf '\n'
}

missing() { # <what> <how to fix, for a human> [command --fix may run]
    printf '  %s✗ %s%s\n      %s→ %s%s\n' "$RED" "$1" "$RESET" "$DIM" "$2" "$RESET"
    section_missing[current]=$((${section_missing[current]} + 1))
    if [ -n "${3:-}" ]; then add_fix "$3"; fi
}

optional() { # <what> <where to get it>
    printf '  %s○ %s%s  %s(optional) %s%s\n' "$YELLOW" "$1" "$RESET" "$DIM" "$2" "$RESET"
}

add_fix() {
    local known
    for known in ${fixes[@]+"${fixes[@]}"}; do
        if [ "$known" = "$1" ]; then return; fi
    done
    fixes+=("$1")
}

SDKMANAGER="$(command -v sdkmanager || true)"
if [ -z "$SDKMANAGER" ] && [ -x "$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager" ]; then
    SDKMANAGER="$SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
fi
# Print the command that installs an SDK package, or nothing when sdkmanager is not installed.
sdk_fix() { if [ -n "$SDKMANAGER" ]; then printf "'%s' '%s'" "$SDKMANAGER" "$1"; fi; }
# Same for a Homebrew formula.
brew_fix() { if have brew; then printf 'brew install %s' "$1"; fi; }
SUBMODULES_FIX="git -C '$REPO' submodule update --init --recursive --depth 1"

printf '%sVulkan doctor%s  %s%s%s\n' "$BOLD" "$RESET" "$DIM" "$REPO" "$RESET"

# ---------------------------------------------------------------------------------------------------------------------
section "Android device loop" "build the vulkan flavor, install it, read VUIDs from logcat"

if have adb; then ok "adb"; else missing "adb" "sdkmanager 'platform-tools'" "$(sdk_fix platform-tools)"; fi

if [ -d "$SDK_ROOT/ndk/$NDK_VERSION" ]; then
    ok "NDK $NDK_VERSION"
else
    missing "NDK $NDK_VERSION" "sdkmanager 'ndk;$NDK_VERSION'" "$(sdk_fix "ndk;$NDK_VERSION")"
fi

sdk_cmake="$(ls "$SDK_ROOT/cmake" 2>/dev/null | grep '^3\.' | tail -1)"
if [ -n "$sdk_cmake" ]; then
    ok "Android SDK CMake" "$sdk_cmake"
else
    missing "Android SDK CMake 3.x" "sdkmanager 'cmake;$SDK_CMAKE_VERSION'" "$(sdk_fix "cmake;$SDK_CMAKE_VERSION")"
fi

if have adb; then
    devices="$(adb devices | awk 'NR > 1 && $2 == "device" { print $1 }')"
    if [ -n "$devices" ]; then
        for serial in $devices; do
            model="$(adb -s "$serial" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
            api="$(adb -s "$serial" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r')"
            ok "device $serial" "$model, API $api"
        done
    elif adb devices | grep -q 'unauthorized'; then
        missing "authorized Android device" "accept the USB debugging prompt on the phone"
    else
        missing "Android device" "plug a phone in, or start an emulator"
    fi
fi

# The build writes developer-config.xml with a placeholder when MLN_API_KEY is unset: that is still "no key".
DEVELOPER_CONFIG="$REPO/platform/android/MapLibreAndroidTestApp/src/main/res/values/developer-config.xml"
if [ -n "${MLN_API_KEY:-}" ]; then
    ok "MapTiler key" "MLN_API_KEY"
elif [ -f "$DEVELOPER_CONFIG" ] && ! grep -q 'YOUR_API_KEY_GOES_HERE' "$DEVELOPER_CONFIG"; then
    ok "MapTiler key" "developer-config.xml"
else
    missing "MapTiler key" "export MLN_API_KEY=<key>   (the app builds without it, but MapTiler styles load no tile)"
fi

# ---------------------------------------------------------------------------------------------------------------------
section "Submodules" "vendored Vulkan headers, allocator and shader compiler"

for module in Vulkan-Headers VulkanMemoryAllocator glslang; do
    if [ -n "$(ls -A "$REPO/vendor/$module" 2>/dev/null)" ]; then
        ok "vendor/$module"
    else
        missing "vendor/$module" "git submodule update --init --recursive --depth 1" "$SUBMODULES_FIX"
    fi
done
if [ -f "$REPO/vendor/Vulkan-Headers/registry/validusage.json" ]; then
    ok "Vulkan registry" "used by vk-lookup.py"
else
    missing "Vulkan registry (vk.xml, validusage.json)" "git submodule update --init --depth 1 vendor/Vulkan-Headers" \
        "$SUBMODULES_FIX"
fi

# ---------------------------------------------------------------------------------------------------------------------
section "macOS Vulkan build" "cmake --preset macos-vulkan, runs on MoltenVK"

if [ -n "${VULKAN_SDK:-}" ] && [ -d "$VULKAN_SDK" ]; then
    ok "LunarG Vulkan SDK" "$VULKAN_SDK"
else
    missing "LunarG Vulkan SDK" "install it, then load its environment (see note below)"
    needs_lunarg=1
fi
# The macos preset compiles through ccache and links libuv, found with pkg-config (platform/darwin/darwin.cmake).
for tool in cmake ninja ccache; do
    if have "$tool"; then ok "$tool"; else missing "$tool" "brew install $tool" "$(brew_fix "$tool")"; fi
done
if have pkg-config; then ok "pkg-config"; else missing "pkg-config" "brew install pkgconf" "$(brew_fix pkgconf)"; fi
if { have pkg-config && pkg-config --exists libuv; } || { have brew && brew list --versions libuv >/dev/null 2>&1; }; then
    ok "libuv"
else
    missing "libuv" "brew install libuv" "$(brew_fix libuv)"
fi
# Only the GLFW demo app needs it (MLN_WITH_GLFW, on by default); the render tools build with -DMLN_WITH_GLFW=OFF.
if have pkg-config && pkg-config --exists glfw3; then
    ok "glfw"
else
    optional "glfw" "brew install glfw, or configure with -DMLN_WITH_GLFW=OFF"
fi

# ---------------------------------------------------------------------------------------------------------------------
section "Analysis tools" "offline shader checks and capture analysis, all optional"

sdk_tools=""
for tool in vulkaninfo glslangValidator spirv-val spirv-dis gfxrecon-info gfxrecon-convert; do
    if have "$tool"; then ok "$tool"; else sdk_tools="$sdk_tools${sdk_tools:+, }$tool"; fi
done
if [ -n "$sdk_tools" ]; then
    optional "$sdk_tools" "part of the LunarG Vulkan SDK"
    needs_lunarg=1
fi
if have jq; then
    ok "jq"
else
    optional "jq" "brew install jq"
    if have brew; then add_fix "brew install jq"; fi
fi
if have renderdoccmd || have rdc-cli; then
    ok "RenderDoc CLI"
else
    optional "RenderDoc CLI" "no macOS release: build it from source, see the vulkan-capture skill"
fi

# ---------------------------------------------------------------------------------------------------------------------
printf '\n%sSummary%s\n' "$BOLD" "$RESET"
total=0
for i in "${!section_names[@]}"; do
    count="${section_missing[i]}"
    total=$((total + count))
    if [ "$count" -eq 0 ]; then
        printf '  %s✓%s %-20s %sready%s\n' "$GREEN" "$RESET" "${section_names[i]}" "$GREEN" "$RESET"
    else
        printf '  %s✗%s %-20s %s%s missing%s\n' "$RED" "$RESET" "${section_names[i]}" "$RED" "$count" "$RESET"
    fi
done

if [ "$needs_lunarg" -eq 1 ]; then
    printf '\n%sLunarG Vulkan SDK%s  installer: %s\n' "$BOLD" "$RESET" "$LUNARG_URL"
    printf '  then load it in every shell, by adding this to ~/.zshrc:\n'
    printf '    () { local env=(~/VulkanSDK/*/setup-env.sh(N.nOn[1])); [[ -n $env ]] && source $env >/dev/null }\n'
fi

if [ "$mode" = "report" ]; then
    printf '\n'
    if [ "$total" -eq 0 ]; then
        printf '%sEverything required is installed.%s\n' "$GREEN" "$RESET"
    elif [ "${#fixes[@]}" -gt 0 ]; then
        printf '%s can be fixed automatically: %s%s --fix%s  (--dry-run to preview)\n' \
            "${#fixes[@]}" "$BOLD" "$SELF" "$RESET"
    else
        printf 'What is missing needs a manual step: follow the → lines above.\n'
    fi
    exit 0
fi

printf '\n%s%s%s\n' "$BOLD" "$([ "$mode" = "fix" ] && echo "Fixing" || echo "Dry run: --fix would run")" "$RESET"
if [ "${#fixes[@]}" -eq 0 ]; then
    printf '  nothing: what is missing needs a manual step, follow the → lines above.\n'
    exit 0
fi

failed=0
for command in "${fixes[@]}"; do
    printf '  %s$%s %s\n' "$DIM" "$RESET" "$command"
    if [ "$mode" = "fix" ] && ! bash -c "$command"; then
        printf '  %s✗ failed%s\n' "$RED" "$RESET" >&2
        failed=$((failed + 1))
    fi
done
if [ "$mode" = "dry-run" ]; then exit 0; fi

printf '\n%sChecking again%s\n' "$BOLD" "$RESET"
"${BASH_SOURCE[0]}"
exit "$failed"
