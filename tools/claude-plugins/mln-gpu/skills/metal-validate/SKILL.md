---
name: metal-validate
description: Compiles every MapLibre Metal shader offline, then runs the Metal backend with Apple's API validation and shader validation (macOS render tests, iOS simulator or device) and reads what they report. Use after every change to src/mln/mtl, the Metal shaders or the darwin Metal glue, when a "Validation" message, "failed assertion" or shader "compile failed" appears, and to prove a Metal change is finished.
---

# Metal validate

The target is **quiet**: a Metal change is done when the shaders it touches compile offline, API validation and
shader validation report nothing from the code it touches and nothing the base commit did not report, on the
screens it affects, and the frame is right. A clean build proves none of this: MSL is compiled at run time.

`man MetalValidation | col -b` is the reference for every variable below (macOS 27). Read it before using one
not listed here.

## 1. Offline shader compile (seconds, no app)

```bash
${CLAUDE_SKILL_DIR}/scripts/msl-check.py              # from the fork root; all shaders, iOS + macOS
${CLAUDE_SKILL_DIR}/scripts/msl-check.py Terrain Symbol --keep "$OUT/msl"   # some, keep the .metal files
```

It assembles each shader exactly like `Context::createProgram` (common prelude + shader prelude + source), with the
`MTL::LanguageVersion` read from `src/mln/mtl/context.cpp` (2.4 today), fast math, `DEVICE_PIXEL_RATIO`, the iOS
16.0 and macOS 14.3 targets, and two variants per shader: every paint property data-driven (no `HAS_UNIFORM_u_*`)
and every one constant (all defined). The runtime picks one of the 2^n combinations per style, so a variant that
only fails with some properties defined can still slip through: add `-D HAS_UNIFORM_u_x` to test one.

Baseline (untouched tree, Xcode 27.2): 35 shaders, 2 failures, both `SymbolTextAndIconShader` in the
**attributes** variant (`symbol.hpp` ~line 775: `.fill_color = half(...);` inside a designated initializer,
`half` instead of `half4` and `;` instead of `,`). It comes from upstream (`cbe4681ef1ee`). It would bite at run
time only for a style whose text-and-icon symbol layer has data-driven `text-color` etc. Judge a change against
this baseline, not against zero.

## 2. API and shader validation on macOS (fastest loop)

The headless render-test runner exercises the same backend as iOS. Build the `macos-metal` preset (Debug by
default here) and run with validation on:

```bash
cmake --preset macos-metal && cmake --build build-macos-metal --target mbgl-render-test-runner
OUT=$(mktemp -d)        # logs, traces and PNGs go here, not in the repo
MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_WARNING_MODE=nslog \
MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build-macos-metal/mbgl-render-test-runner --manifestPath=metrics/macos-xcode11-release-style.json \
  -f 'terrain/' > "$OUT/render.log" 2>&1; echo "exit $?"
mv metrics/macos-xcode11-release-style.html "$OUT/"   # the runner writes its report into the repo (git-ignored)
```

`-f` is a regex on the test path; a pattern that matches nothing runs nothing and exits 0 (`terrain/default$`
matches nothing). Check `grep -c '^\* passed' "$OUT/render.log"`.

- `Metal API Validation Enabled` and `Metal GPU Validation Enabled` at the top of the log prove both layers
  loaded. Variables are read when the `MTLDevice` is created: set them in the environment of the process, before
  launch.
- **Errors**: `MTL_DEBUG_LAYER_ERROR_MODE` defaults to `assert`: the first API misuse aborts the process with
  `failed assertion` and the call that caused it. That is the wanted behaviour in a check run: a non-zero exit
  plus the assertion text is the finding. Use `=nslog` only to collect several errors in one run.
- **Warnings** (`MTL_DEBUG_LAYER_WARNING_MODE=nslog`): each is a header line (`Set Fragment Bytes Validation`,
  `End Encoding Validation`, ...) followed by the message. Count them by message:
  ```bash
  grep -v 'Resource not found\|^\* ' "$OUT/render.log" | sed -E 's/^[0-9-]+ [0-9:.]+ [^ ]+ //; s/index [0-9]+/index N/' \
    | sort | uniq -c | sort -rn | head -20
  ```
- **Shader validation** reports out-of-bounds memory, texture misuse, NaN/Inf in interpolants and stack overflow
  from the GPU. Without `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` the reports go to the system log only
  (`log stream --predicate 'subsystem == "com.apple.Metal"'`). It is slow: scope it to one shader with
  `MTL_SHADER_VALIDATION_DEFAULT_STATE=none MTL_SHADER_VALIDATION_ENABLE_PIPELINES=<label>` (pipeline labels are
  `<ShaderName>#<hash>`, set in `src/mln/shaders/mtl/shader_program.cpp`;
  `MTL_SHADER_VALIDATION_DUMP_PIPELINES=1` prints them).
- `MTL_DEBUG_LAYER_VALIDATE_LOAD_ACTIONS=1` / `_STORE_ACTIONS=1` paint `DontCare` loads fuchsia and `DontCare`
  stores as a red/white checkerboard. They change pixels on purpose: use them to find an attachment read after
  a `DontCare` (the offscreen depth uses `DontCare`, `offscreen_texture.cpp`), never during a pixel comparison.

Baseline (untouched tree, Apple M4 Pro, macOS 27.2, `-f 'terrain/'`): 14 passed, 29 ignored, **no API error and
no shader-validation report**. About 18 000 warnings, all `unused vertex|fragment binding in render encoder at
Buffer|Texture|Sampler index N`: the backend binds UBOs and textures to both stages whatever the shader uses
(`uniform_buffer.cpp`, `texture2d.cpp`). They are advice, not misuse: report a new kind, never chase these.

Known failing Metal render tests are in `metrics/ignores/platform-ios-metal.json` (and the shared
`platform-all.json`, `platform-macos.json`). `ignore` in the log is not `failed`.

## 3. iOS

Pixel and driver behaviour differ from macOS (tile memory, memoryless targets, simulator formats), so an iOS
change ends with a run on iOS. Bazel needs `platform/darwin/bazel/config.bzl` once (git-ignored):
`cp platform/darwin/bazel/example_config.bzl platform/darwin/bazel/config.bzl`.

**Simulator, API validation: the iOS unit tests** (verified, Xcode 27.2, iOS 27.2 simulator). `ios_test` needs no
provisioning profile, and `--test_env` reaches the test process. Run outside the command sandbox; rules_apple
creates and boots its own `BAZEL_TEST_*` simulator (not a `berth` lease):

```bash
bazel test //platform/ios/test:ios_test --//:renderer=metal --ios_multi_cpus=sim_arm64 --test_output=all \
  --test_filter=MLNCustomStyleLayerTerrainTests,MLNMapViewTerrainElevationTests \
  --test_env=MTL_DEBUG_LAYER=1 --test_env=MTL_DEBUG_LAYER_WARNING_MODE=nslog > "$OUT/ios.log" 2>&1
grep -E 'Validation Enabled|Assertion|failed assertion|Executed [0-9]+ tests' "$OUT/ios.log" | grep -v 'simctl\|xcodebuild' | sort | uniq -c
```

About 3 min. Baseline (untouched tree): 11 tests, 0 failures, no API error; warnings are `unused ... binding`
(about 1 900) and 12 `redundant setViewport`. Without `--test_filter` the whole suite runs. `--test_filter` takes
`Class` or `Class/testMethod`, comma-separated.

**Never turn shader validation on in the iOS simulator.** With `MTL_SHADER_VALIDATION=1` there, a minimal
standalone pipeline crashes (`signal 11`) and MapLibre's `newRenderPipelineState` fails, so every test that draws
dies on `Assertion failed: (false), function getRenderPipelineState, file shader_program.cpp, line 183`. That is
the simulator, not the code: the same tests pass with API validation alone and with none. Run shader validation on
the macOS runner (section 2) or a device.

MapLibre's own errors (`Log::Error`, e.g. `<Shader> newRenderPipelineState failed: <reason>`) go to os_log,
subsystem `org.maplibre.Native` (`platform/darwin/src/MLNLoggingConfiguration.mm`), not to stderr: the test log
shows only the assertion. The reason was not found in the simulator's `log show` during the run above; reproduce
on the macOS runner to read it.

**The app** (`//platform/ios:App`, bundle id `<BUNDLE_ID_PREFIX>.maplibre.app`, a "Terrain" style entry in
`platform/ios/app/MBXViewController.mm`, no dedicated terrain screen) and the iOS render tests
(`//render-test/ios:RenderTest`) need a provisioning profile even for the simulator: without one the build stops
on `no provisioning profile was found named 'iOS Team Provisioning Profile: *'`. Set
`APPLE_MOBILE_PROVISIONING_PROFILE_NAME` / `_TEAM_ID` in `config.bzl` to a profile installed on the Mac (Xcode
creates one when a team is signed in). With it, on a simulator leased through the `berth` skill (not run: no
profile on the Mac this was written on; rules_apple normally outputs an `.ipa`, check `bazel-bin/platform/ios/`):

```bash
bazel build //platform/ios:App --//:renderer=metal --ios_multi_cpus=sim_arm64
unzip -o -q bazel-bin/platform/ios/App.ipa -d "$OUT/app"
UDID=$(berth claim --ios --label "<task>" --timeout 110)
xcrun simctl install "$UDID" "$OUT/app/Payload/App.app"
SIMCTL_CHILD_MTL_DEBUG_LAYER=1 SIMCTL_CHILD_MTL_DEBUG_LAYER_WARNING_MODE=nslog \
  xcrun simctl launch --console-pty --terminate-running-process "$UDID" <bundle id> 2>&1 | tee "$OUT/ios.log"
berth release "$UDID"
```

`SIMCTL_CHILD_` passes a variable to the launched process (verified with `simctl spawn`). The simulator runs
Metal on the Mac's GPU with simulator-only paths in the backend (`TARGET_OS_SIMULATOR`: `Depth32Float_Stencil8`
and private storage instead of memoryless), so it is not the device.

**Device** (not run: needs the profile and a device): `DEVICECTL_CHILD_MTL_DEBUG_LAYER=1 ... xcrun devicectl
device process launch --device <id> --console <bundle id>`, or `--environment-variables '{"MTL_DEBUG_LAYER":"1",
"MTL_SHADER_VALIDATION":"1","MTL_SHADER_VALIDATION_REPORT_TO_STDERR":"1"}'` (it overrides the `DEVICECTL_CHILD_`
ones). Or the Xcode scheme from `bazel run //platform/ios:xcodeproj
--@rules_xcodeproj//xcodeproj:extra_common_flags="--//:renderer=metal"`: Edit Scheme > Run > Diagnostics.

## 4. Run-time shader errors

A shader that does not compile logs `Log::Error(Event::Shader, "<name> compile failed: ...")` then hits
`assert(false)` (`src/mln/mtl/context.cpp`, `createProgram`); a missing entry point or a failed pipeline
(`newRenderPipelineState failed`, `shader_program.cpp`) does the same. In release the draw is skipped with a
warning. The message line numbers count from the top of the assembled source: reproduce with
`msl-check.py <Name> --keep DIR` and read the line in `DIR/<Name>.metal`.

## 5. Fix one finding at a time

Take the first error (the assertion that aborted, or the earliest message). For each:

1. Look the API up: `${CLAUDE_PLUGIN_ROOT}/skills/metal-spec-lookup/scripts/mtl-lookup.py <ObjC or MTL:: name>`, the
   doc comment and the availability there are Apple's.
2. Find the call in `src/mln/mtl/` that issued it (the assertion names the selector; pipeline and debug-group
   labels name the shader and layer).
3. Explain the mismatch in one sentence before editing.
4. Fix where the state is created (pipeline descriptor, render pass descriptor, depth-stencil state, texture
   descriptor), not at the call site. Then check what else shares it: pipelines are cached by hash
   (`shader_program.cpp`), depth-stencil states per drawable and per layer group.
5. Rerun the same tests and gestures: the finding is gone and nothing new appeared.

## Report

List: the offline compile result, the build, the platform (macOS runner, simulator model, device), the tests or
screens and gestures, the validation findings before and after with counts, and anything that could not run.
"API and shader validation quiet on `terrain/` (14 passed), msl-check unchanged from baseline" is a result.
"Should be fine" is not.
