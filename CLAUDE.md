# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this branch is

`cumbly` is Lunabee's fork branch (`origin` = `LunabeeStudio/maplibre-native`) that adapts MapLibre Native for the
[Cumbly](https://github.com/LunabeeStudio/Cumbly) app, with the goal of running its 3D terrain map on the **Vulkan**
(Android) and **Metal** (iOS) backends.

It is built from two layers:

1. **Upstream `feature/terrain-3d`** (the 3D terrain work, not yet in upstream `main`), at the commit Cumbly pins:
   `2ef164d154f0`. Everything below that commit is upstream history.
2. **Cumbly changes**, one commit each, first ported from Cumbly's `tools/cli/patches/maplibre/NNNN-*.patch`. Each
   commit body says `Cumbly patch NNNN-<name>.patch.` Names containing `-android-` or `-ios-` concern that platform
   only; the rest are shared core changes.

### How Cumbly consumes this branch

**Android: this repo is the source of truth.** Cumbly carries no Android MapLibre code, patch or AAR. Publish from
here and pin the printed version in Cumbly's `gradle/libs.versions.toml` (`maplibre = "…"`):

```bash
cd platform/android
scripts/publish-cumbly.sh local         # into ~/.m2/repository, to try a change in Cumbly
scripts/publish-cumbly.sh artifactory   # into Lunabee's Artifactory (maplibre-native-local), for teammates and CI
# both default to --flavor vulkan
```

- Artifact `org.maplibre.gl:android-sdk-vulkan` (`--flavor opengl` gives `android-sdk-opengl`, which no longer carries TrackLayer / SkyLayer), version
  `<platform/android/VERSION>-cumbly-<HEAD committer time, Unix seconds>-<short HEAD sha>`, e.g.
  `13.7.1-cumbly-1795615450-608efb0`, so versions sort by commit time. A local build of a dirty tree
  gets `-dirty`; `artifactory` refuses one. `--abis "arm64-v8a x86_64"` (space-separated) narrows a local build.
- The version comes from `-Pmaplibre.versionName` (read in `buildSrc/.../maplibre.artifact-settings.gradle.kts`);
  publishing goes through the existing `publicationRepositoryUrl` path of `maplibre.gradle-publish.gradle.kts`,
  which skips signing.
- `artifactory` deploys to `https://artifactory.lunabee.studio/artifactory/maplibre-native-local` with
  `artifactory_deployer_release_username` / `artifactory_deployer_release_api_key` from `~/.gradle/gradle.properties`
  (or `ARTIFACTORY_DEPLOYER_USERNAME` / `ARTIFACTORY_DEPLOYER_API_KEY`). It refuses a version already there, so
  publish each commit once.
- Cumbly's `settings.gradle.kts` resolves that one module from Maven Local first, then Artifactory, with the
  consumer credentials every Lunabee machine and TeamCity already have.
- Requires NDK `28.2.13676358` and SDK CMake 3.x (`sdkmanager "ndk;28.2.13676358" "cmake;3.31.6"`).

**iOS: still built by Cumbly.** Its CLI (`./cumbly ios maplibre`) checks out upstream at `2ef164d`, applies the shared
and `-ios-` patches kept in Cumbly, runs `bazel build //platform/ios:MapLibre.dynamic --compilation_mode=opt
--//:renderer=metal` and commits the XCFramework under `Cumbly_iOS/maplibre-terrain/`. iOS changes made here must
still be mirrored as patches there until iOS moves to this repo too.

The Cumbly app ships the **Vulkan** flavor (`android-sdk-vulkan`). The Android changes that moved to Vulkan, on top of
the upstream terrain branch:

- `TrackLayer` / `SkyLayer` (`platform/android/MapLibreAndroid/src/cpp/style/layers/`) are raw Vulkan custom layers:
  `{track,sky}_layer.cpp` keep the JNI entry points and state, `{track,sky}_layer_vulkan.cpp` the hosts, and
  `custom_layer_vk.hpp` the shared helpers (run-time GLSL 450 compile with glslang, host-visible buffers, pipeline per
  render pass and sample count, deferred destruction). They are built under `MLN_WITH_VULKAN` only; on the other
  flavors the Java classes exist and calling them throws `UnsatisfiedLinkError`. The earlier GLES3 hosts are deleted.
- The surface render pass is multisampled 4x (colour and depth/stencil, colour resolved into the swapchain image),
  falling back to 1 sample when the device lacks 4x for those formats. Headless output and offscreen passes stay 1
  sample; the sample count is part of the pipeline hash.
- `style::vulkan::CustomLayerRenderParameters` also carries `sampleCount`, `extent` (pixels) and `minDepth` /
  `maxDepth` (the terrain depth range of the viewport).
- A descriptor set that was already bound in the frame's command buffer is retired and replaced before it is written
  (the layer reported "updated without UPDATE_AFTER_BIND" every frame), and offscreen target framebuffers and render
  passes are destroyed through the frame deletion queue.
- History: the GL-only changes (4x MSAA `EGLConfigChooser`, "discard stale GL errors") were reverted, and the GLES3
  custom layer hosts deleted, when Cumbly moved to Vulkan. `aa97643842fc` (custom layers in the terrain's 3D depth
  range) and `e49c000c0798` (768 px drape targets) are core changes and apply to every backend.

## Terrain docs

- `TERRAIN.md` is the main reference for the terrain implementation: architecture, key files, the **backend parity
  table** (GL / Vulkan / Metal / WebGPU), known divergences from maplibre-gl-js, and the Android terrain test
  activities. Read it before touching terrain code. When in doubt, match maplibre-gl-js behaviour.
- `TERRAIN_INSTANCING_EXPERIMENT.md` holds design notes for instancing the terrain draws (experiment, not wired up).

## Vulkan work

Vulkan backend work uses the user-level tools, not files in this repo: the `vulkan-engineer` agent
(`~/.claude/agents/vulkan-engineer.md`) carries the backend map and the conventions that differ from GL, and three
skills (`~/.claude/skills/`) give it evidence instead of recall:

- `vulkan-spec-lookup`: `vk-lookup.py <name-or-VUID>` reads the vendored registry (`vendor/Vulkan-Headers`); it finds
  the fork from the git top level, or from `MLN_REPO`. The backend is Vulkan **1.0**; the script flags anything newer.
- `vulkan-validate`: build with the validation layer and read VUIDs from the device. Validation is off by default
  on Android; `-Pmaplibre.vulkanValidation=true` (CMake `-DMLN_VULKAN_VALIDATION=ON`) turns it on.
- `vulkan-capture`: screenshots, GFXReconstruct traces and call logs for a frame that is wrong while validation is
  quiet.

`scripts/vulkan-doctor.sh` lists the host tools that are missing; `--fix` installs the ones that can be scripted
(submodules, `sdkmanager` packages, Homebrew formulae).

## Commands

### Android (`cd platform/android`)

Build one renderer flavor, never plain `assembleDebug` (it fans out to every flavor and fails on the WebGPU Gradle
config). Pass `-Pmaplibre.abis=arm64-v8a` for a phone or `x86_64` for the emulator.

```bash
./gradlew :MapLibreAndroidTestApp:installVulkanDebug -Pmaplibre.abis=arm64-v8a   # or installOpenglDebug
./gradlew :MapLibreAndroid:assembleVulkanDebug                                   # library only
./gradlew checkStyle        # ktlint
./gradlew formatKotlin
```

Flavors: `opengl`, `vulkan`, `webgpuDawn`, `webgpuWgpu`, `multiBackend` (GL + Vulkan, chosen at runtime). The test
app needs a MapTiler key in the `MLN_API_KEY` env var (or `developer-config.xml`). Terrain is checked by hand in the
test app's "3D Terrain" activities (Style section), on a physical device.

### iOS / macOS (Bazel)

```bash
cp platform/darwin/bazel/example_config.bzl platform/darwin/bazel/config.bzl   # once
bazel build //platform/ios:MapLibre.dynamic --compilation_mode=opt --//:renderer=metal
bazel run //platform/ios:xcodeproj --@rules_xcodeproj//xcodeproj:extra_common_flags="--//:renderer=metal"
```

Bazel files are the source of truth for build settings; do not change them in Xcode.

### Core C++ (CMake, macOS host)

```bash
cmake --preset macos-metal          # or macos-vulkan; presets in CMakePresets.json
cmake --build build-macos-metal --target mbgl-test-runner mbgl-render-test-runner

# Unit tests (gtest) need the test server running in another terminal:
npm install && node test/storage/server.js
build-macos-metal/mbgl-test-runner --gtest_filter='Terrain*'

# Render tests: -f is a regex on the test path
build-macos-metal/mbgl-render-test-runner --manifestPath=metrics/macos-xcode11-release-style.json -f 'terrain/.*'
```

Render tests live in `metrics/integration/render-tests/<property>/<test>/` (`style.json` + `expected.png`);
manifests are `metrics/*.json` (per platform/backend, e.g. `android-vulkan-render-test-runner-style.json`,
`ios-metal-render-test-runner-style.json`). Android and iOS render-test apps live under `render-test/`; see
`docs/mdbook/src/render-tests.md` and `docs/mdbook/src/platforms/*/` for running them on device.

### Formatting

`pre-commit install` enables the hooks: clang-format (C++), buildifier (Bazel), swiftformat, rustfmt, actionlint.

## Architecture

- **Namespace is `mln`** (upstream renamed `mbgl` -> `mln`; sources are in `src/mln/` and `include/mln/`). CMake
  target names still use the old `mbgl-` prefix (`mbgl-core`, `mbgl-test-runner`, ...).
- **Core** (`src/mln`, `include/mln`): platform-independent C++ — style parsing (`style/`), tile loading
  (`tile/`), and the drawable renderer (`renderer/`). Each layer type has a `RenderLayer` that builds drawables plus
  a **layer tweaker** (`renderer/layers/*_layer_tweaker.cpp`) that fills its uniform buffers per frame.
- **Backends**: `src/mln/{gl,vulkan,mtl,webgpu}` implement the `gfx` abstraction (`src/mln/gfx`). Backend is a
  build-time choice: CMake `MLN_WITH_OPENGL|VULKAN|METAL|WEBGPU`, Bazel `--//:renderer=...`, Android Gradle flavor.
  Backend-specific code in shared files is guarded by `MLN_RENDER_BACKEND_*` macros.
- **Shaders**: GL sources are `shaders/*.glsl`, compiled into headers under `include/mln/shaders/gl/` by
  `node shaders/generate_shader_code.mjs`. Metal, Vulkan and WebGPU shaders are hand-written C++ headers in
  `include/mln/shaders/{mtl,vulkan,webgpu}/`. UBO layouts (`include/mln/shaders/*_ubo.hpp`) are shared by all
  backends, so a uniform change touches every backend's shader.
- **Style properties** are code-generated from the spec in `scripts/style-spec.mjs`: core by
  `node scripts/generate-style-code.mjs`, bindings by `platform/android/scripts/generate-style-code.mjs` and
  `platform/darwin/scripts/generate-style-code.mjs`. Add a property in the spec and regenerate rather than
  hand-editing the generated files.
- **Source lists are duplicated**: a new `src/`/`include/` file must be added to both `CMakeLists.txt` and
  `bazel/core.bzl`, otherwise one of the two builds (Android/CMake or iOS/Bazel) breaks.
- **Platforms**: `platform/android` (Gradle; JNI glue in `MapLibreAndroid/src/cpp`), `platform/darwin` (shared
  iOS/macOS Obj-C, `MLN*` public API), `platform/ios`, `platform/macos`, `platform/glfw`, `platform/linux`.
- Further design docs: `ARCHITECTURE.md`, `docs/mdbook/src/design/`.

## Conventions on this branch

- Commit subjects are `area: lowercase summary` (`core:`, `android:`, `ios:`, `test:`, `docs:`, `vulkan:`, ...).
- Keep each Cumbly patch as its own commit, so the branch can be exported back to Cumbly's patch files.
- Upstream (`CONTRIBUTING.md`) asks for disclosure of AI assistance in PRs sent to maplibre/maplibre-native.
