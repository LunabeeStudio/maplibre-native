---
name: vulkan-validate
description: Builds the Vulkan flavor with the Khronos validation layer, runs it on an Android device or on macOS, and reads the VUID errors and shader compile errors it produces. Use after every change to the Vulkan backend or its shaders, when a VUID or "Validation Error" appears in a log, and to prove a Vulkan change is finished.
---

# Vulkan validate

The target is **quiet**: a Vulkan change is done when the validation layer reports no error from the code it
touches and none that the base commit did not report, on the screens it affects, and the frame is right. A clean
build proves neither.

Host check, if a command below fails for a missing tool:
`scripts/vulkan-doctor.sh` reports, `--dry-run` previews the repairs, `--fix` runs them (submodules, `sdkmanager`
packages, Homebrew formulae). `--fix` installs software: run it only when the user asked for it. The LunarG Vulkan SDK, `MLN_API_KEY` and device authorization stay manual.

## 1. Build with validation

Validation, debug labels and object names are compiled out on Android unless asked for:

```bash
cd platform/android
./gradlew :MapLibreAndroidTestApp:assembleVulkanDebug -Pmaplibre.abis=arm64-v8a -Pmaplibre.vulkanValidation=true
```

`x86_64` for the emulator. The property sets CMake `MLN_VULKAN_VALIDATION=ON`, which defines
`ENABLE_VULKAN_VALIDATION` (`include/mln/vulkan/renderer_backend.hpp`). The layer binary
(`libVkLayer_khronos_validation.so`) is downloaded into the debug `jniLibs` by
`maplibre.download-vulkan-validation.gradle.kts`, so only **debug** builds can load it.
Published Cumbly AARs never set the property.

## 2. Install and run

Pick the device explicitly; ask the user when several are attached.

```bash
SERIAL=<serial from 'adb devices'>
OUT=$(mktemp -d)        # logs and screenshots go here, not in the repo
PKG=org.maplibre.android.testapp
adb -s "$SERIAL" install -r MapLibreAndroidTestApp/build/outputs/apk/vulkan/debug/MapLibreAndroidTestApp-vulkan-debug.apk
adb -s "$SERIAL" logcat -c
adb -s "$SERIAL" shell am start -n $PKG/.activity.style.TerrainActivity
```

Terrain activities: `TerrainActivity`, `TerrainVectorMapActivity`, `TerrainOsmRasterActivity`,
`TerrainDebugTilesActivity`, `TerrainFlightActivity` (all in `.activity.style`). Tiles need `MLN_API_KEY` at build
time; without it the map is empty, which still exercises instance, swapchain and render-pass setup.

Move the map before reading the log: pitch, rotate, zoom across a tile boundary, rotate the phone. Many errors
only fire on a state change (swapchain recreation, a new pipeline, a drape target resize).

## 3. Read the log

Everything the layer says goes through `mln::Log` with event `Render`, logcat tag `Mbgl`.

```bash
adb -s "$SERIAL" logcat -d -s Mbgl > "$OUT/vulkan.log"
grep -o 'VUID-[A-Za-z0-9_-]*' "$OUT/vulkan.log" | sort | uniq -c | sort -rn      # distinct VUIDs
grep -n 'Vulkan layers not found\|\[Shader\]' "$OUT/vulkan.log"
adb -s "$SERIAL" logcat -d -s vulkan | grep 'Loaded layer'                        # proof the layer is active
```

One message spans several logcat lines: the call and the objects (`E Mbgl : {RenderThread}[Render]: vkCmd...():`),
detail lines, then `The Vulkan spec states: ... #VUID-...`. Read the whole block, the cause is often on the second
line. Two things distort the counts:

- The layer stops reporting a VUID after 10 occurrences (`duplicate_message_limit`), so a count of 10 means "at
  least 10", and a VUID seen once at startup can matter more than one seen 10 times.
- **Cascades**: one invalid action poisons the command buffer, and every later command then reports
  `VUID-vkCmd*-commandBuffer-recording`. Those are one bug. The cause is the detail line, for example
  `VkDescriptorSet 0x... was destroyed or updated without UPDATE_AFTER_BIND` (a descriptor set written after it was
  bound in a command buffer still being recorded). Fix the earliest message in the log first, then re-run.

- `Vulkan layers not found`: the layer did not load. The build was not debug, the property was missing, or the
  ABI has no layer binary. Nothing below is meaningful until this line is gone.
- `[Shader]` errors: glslang failed at run time. The message has the GLSL line; the line number counts from the
  generated preamble (defines + `common.hpp` prelude + the shader's own prelude), not from the header file.
- Crash instead of log: `adb -s "$SERIAL" logcat -d -b crash`, and note that `TERRAIN.md` records one known
  intermittent render-thread SIGSEGV on Vulkan.

## 4. Fix one VUID at a time

Take the earliest error in the log, cascades set aside. For each:

1. Read its full statement: `${CLAUDE_PLUGIN_ROOT}/skills/vulkan-spec-lookup/scripts/vk-lookup.py VUID-...` (from the fork root)
2. The message names the objects involved (debug names set through `setDebugName`) and the command. Find the call
   in `src/mln/vulkan/` that issued it.
3. Explain the mismatch between the statement and that code in one sentence before editing. If you cannot, read
   the neighbouring VUIDs with `vk-lookup.py --vuids <command> <word>`; the cause is often one rule earlier.
4. Fix the cause where the state is created (layout, render pass, pipeline info), not by silencing the symptom at
   the call site. Then check what else shares that state: a descriptor layout change affects every shader using
   the set.
5. Rebuild, rerun the same gestures, confirm the VUID count for that id is zero and no new id appeared.

Warnings (`W Mbgl`, for example `Vertex attribute at location N not consumed by vertex shader`) are advice, not
spec violations: report them, fix only on request.

The backend is not quiet today: `TerrainActivity` already reports errors on an untouched tree. Record the baseline
before changing code (keep the log of a run on the base commit) and judge a change by the difference: no new
VUID, and the ones in the code you touched gone.

## 5. Desktop loop (optional, faster, needs the LunarG SDK)

Debug desktop builds have validation on without any flag.

```bash
cmake --preset macos-vulkan -DCMAKE_BUILD_TYPE=Debug -DMLN_WITH_GLFW=OFF   # GLFW is only for the demo app
cmake --build build-macos-vulkan --target mbgl-render-test-runner
build-macos-vulkan/mbgl-render-test-runner --manifestPath=metrics/macos-xcode11-release-style.json -f 'terrain/.*' 2>&1 | tee "$OUT/render.log"
grep -c 'VUID-' "$OUT/render.log"
mv metrics/macos-xcode11-release-style.html "$OUT/"   # the HTML report is written into the repo
```

`scripts/vulkan-doctor.sh` checks the Homebrew tools the `macos` preset needs (`ccache`, `pkg-config`, `libuv`).
This runs on MoltenVK, which is stricter in places and blind to others: no surface pre-rotation, different
supported formats. It is a fast first filter and a pixel check against `expected.png`, never the final proof for
Android. Known failing Vulkan render tests are listed in `metrics/ignores/platform-*-vulkan.json`.

Baseline on an untouched tree (Apple M5 Pro, LunarG SDK 1.4.363.1): all 6 `terrain/` tests fail the pixel check
(`terrain/default` diff 0.945) and the log holds 581 `VUID-` lines, the same
`VkDescriptorSet ... was destroyed or updated without UPDATE_AFTER_BIND` cascade as on Android. Compare against
this, not against zero.

One frame without a test: `cmake --build build-macos-vulkan --target mbgl-render`, then
`build-macos-vulkan/bin/mbgl-render -z 7 -x -74 -y 41 --style <style url>` writes `out.png`.

Pixel checks on Android itself use the render-test app (`render-test/android`, flavor `vulkan`, manifest
`metrics/android-vulkan-render-test-runner-style.json`). Running it, filtering tests and pulling the HTML report
are described in `docs/mdbook/src/platforms/android/android-tests.md`.

Deeper checks, both compile-time switches next to `ENABLE_VULKAN_VALIDATION` in `renderer_backend.hpp`:
`ENABLE_VULKAN_VALIDATION_FEATURES` (GPU-assisted, synchronization and best-practices validation; raises the
requested API version to 1.2) and `ENABLE_VMA_DEBUG` (allocation leaks). Enable locally, do not commit.

## Report

List: the build command, the device, the screens and gestures exercised, the distinct VUIDs before and after with
counts, and anything that could not be run. "Validation quiet on `TerrainActivity`, portrait and landscape,
after pitch and zoom" is a result. "Should be fine now" is not.
