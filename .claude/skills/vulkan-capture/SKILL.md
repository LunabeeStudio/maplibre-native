---
name: vulkan-capture
description: Gets GPU-side evidence for a Vulkan rendering bug on Android or macOS as text an agent can read - device screenshots, GFXReconstruct API traces converted to JSON Lines, api_dump call logs, RenderDoc pipeline state. Use when the validation layer is quiet but the frame is wrong (black, flipped, missing draw, wrong depth or blending), or to check what calls and state actually reached the driver.
---

# Vulkan capture

A wrong frame with a quiet validation layer is a state bug: the calls are legal, the values are wrong. Read the
state the driver received instead of reasoning from the source alone. Go down this list and stop at the first
level that explains the frame.

## 1. Screenshot (always first)

```bash
SERIAL=<serial from 'adb devices'>; OUT=$(mktemp -d)   # evidence goes here, not in the repo
adb -s "$SERIAL" exec-out screencap -p > "$OUT/frame.png"   # then Read the PNG
```

Compare with the same activity on the `opengl` flavor, which is the reference: GL is the backend Cumbly ships.
State the question precisely before looking ("is the terrain mesh drawn but untextured, or not drawn?").
The symptom tables in `TERRAIN.md` map many pictures to a cause, check them before capturing anything.

## 2. Debug labels in the validation log

A validation build (`vulkan-validate` skill) names passes, pipelines and layouts through
`RendererBackend::setDebugName` and the `beginDebugLabel` calls in `src/mln/vulkan/{render_pass,command_encoder}.cpp`.
VUID messages then quote those names. Name the new objects you create: it is one line and it makes every later
message and capture readable.

## 3. GFXReconstruct trace (Android)

Records every Vulkan call of the app into a `.gfxr` file. The test app is debuggable, so no root is needed.
Reference: `USAGE_android.md` in the gfxreconstruct release. Tested with release `vulkan-sdk-1.4.363.0` on a
Galaxy S22 (API 36).

The layer comes from the Android release of gfxreconstruct (`gh release download --repo LunarG/gfxreconstruct
-p '*android.zip'`), inside `tools/replay-debug.apk` at `lib/arm64-v8a/libVkLayer_gfxreconstruct.so`. Copy that
`.so` into the app's data directory, where the Vulkan loader of a debuggable app looks first. The documented
`gpu_debug_layer_app com.lunarg.gfxreconstruct.replay` route fails here: package visibility (Android 11+) hides
the layer app from the test app (`AppsFilter ... BLOCKED`, `Debug layer app ... not installed`), and the app then
aborts in HWUI (`Assertion failed: err < 0`), because HWUI also renders with Vulkan and the requested layer is
missing.

```bash
PKG=org.maplibre.android.testapp
LAYER=libVkLayer_gfxreconstruct.so    # extracted from replay-debug.apk, see above
adb -s "$SERIAL" push "$LAYER" /data/local/tmp/
adb -s "$SERIAL" shell run-as $PKG cp /data/local/tmp/libVkLayer_gfxreconstruct.so .
adb -s "$SERIAL" shell settings put global enable_gpu_debug_layers 1
adb -s "$SERIAL" shell settings put global gpu_debug_app $PKG
adb -s "$SERIAL" shell settings put global gpu_debug_layers VK_LAYER_LUNARG_gfxreconstruct

# a few frames, in the app's internal data directory: /sdcard/Android/data/$PKG/files does not exist until the
# app creates it, and a directory created there by adb is not writable by the app (errno 13)
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_file '\${InternalDataPath}/terrain.gfxr'"
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_frames '120-122'"
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_file_timestamp 'false'"
```

Then start the activity, and check `adb logcat -s gfxrecon` for `Initializing GFXReconstruct capture layer`.
The map renders on demand, so a still map never reaches frame 120: pan it (`adb shell input swipe ...`) until
the log shows `Finished recording graphics API capture`. A frame range writes `terrain_frames_120_through_122.gfxr`,
not `terrain.gfxr`. `fopen(...) failed` means the capture path is not writable. To start and stop by hand instead
of by frame number, leave `capture_frames` empty and toggle `debug.gfxrecon.capture_android_trigger` between
`true` and `false` while the app runs. The trace holds HWUI's Vulkan calls too, on another thread.

Pull, convert, query:

```bash
adb -s "$SERIAL" exec-out run-as $PKG cat terrain_frames_120_through_122.gfxr > "$OUT/terrain.gfxr"
gfxrecon-info "$OUT/terrain.gfxr"                       # frame count, device, extensions used
gfxrecon-convert --format jsonl --output "$OUT/terrain.jsonl" "$OUT/terrain.gfxr"
gfxrecon-convert --help                                 # option names changed between releases: check, do not guess
```

Without `--format jsonl` the output is one indented JSON array, which the `jq` filters below cannot read. The JSON
Lines output has one call per line and is large. Query it with `jq`, never Read it whole:

```bash
jq -c 'select(.function.name? == "vkCmdBindPipeline")' "$OUT/terrain.jsonl" | head
jq -r '.function.name? // empty' "$OUT/terrain.jsonl" | sort | uniq -c | sort -rn | head -30
```

Field names differ between gfxreconstruct versions: print the first three lines to learn the shape before writing
a filter. Useful questions: which pipeline and descriptor sets are bound before the draw that is missing, the
viewport and scissor values, the `VkAttachmentDescription` load/store ops and layouts of the render pass in use,
the image layout at each `vkCmdPipelineBarrier`.

Turn the layer off afterwards, otherwise every later run of the app records a trace:

```bash
adb -s "$SERIAL" shell am force-stop $PKG
adb -s "$SERIAL" shell settings delete global enable_gpu_debug_layers
adb -s "$SERIAL" shell settings delete global gpu_debug_app
adb -s "$SERIAL" shell settings delete global gpu_debug_layers
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_frames ''"
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_file ''"
adb -s "$SERIAL" shell "setprop debug.gfxrecon.capture_file_timestamp ''"
adb -s "$SERIAL" shell run-as $PKG rm -f libVkLayer_gfxreconstruct.so terrain_frames_120_through_122.gfxr
adb -s "$SERIAL" shell rm -f /data/local/tmp/libVkLayer_gfxreconstruct.so
```

`gfxrecon-info` and `gfxrecon-convert` are desktop tools from the LunarG Vulkan SDK.
`scripts/vulkan-doctor.sh` tells whether they are installed.

## 4. Call log on macOS

With the LunarG SDK installed, the desktop build can log every call without a capture file:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump build-macos-vulkan/mbgl-render-test-runner \
  --manifestPath=metrics/macos-xcode11-release-style.json -f 'terrain/default' > "$OUT/api_dump.txt"
grep -n 'vkCmdDraw\|vkCmdBindPipeline\|vkCmdBeginRenderPass' "$OUT/api_dump.txt" | head -60
mv metrics/macos-xcode11-release-style.html "$OUT/"   # the runner writes its report into the repo
```

The build is the one of the `vulkan-validate` desktop loop. One test gives about 1 MB of log.

macOS runs on MoltenVK: it proves call order and state, not Android driver behaviour or surface pre-rotation.

## 5. RenderDoc (not working with this app yet)

The only tool here that shows textures, depth buffers and per-pixel history. RenderDoc ships no macOS release,
but it builds from source on a Mac (upstream calls that unsupported): a `build-mac` tree with `qrenderdoc`,
`renderdoccmd` and the Python module `lib/renderdoc.so`, plus a `build-android-arm64` tree with the server APK
and `lib/libVkLayer_GLES_RenderDoc.so`. Python scripts drive it headless: `adb` device protocol,
`StartRemoteServer`, `CreateRemoteServerConnection`, `RemoteServer.ExecuteAndInject`, `CreateTargetControl`,
`TriggerCapture`, `CopyCaptureFromRemote` (see `docs/python_api/in_depth/remote_replay.rst` in the RenderDoc tree).

Tried on a Galaxy S22 (API 36) with RenderDoc v1.48 built from source, and it stopped short of a capture:

- On an arm64 device RenderDoc looks for an `arm32` APK first. With only `build-android-arm64`, install the
  server APK by hand (`adb install -r -g .../org.renderdoc.renderdoccmd.arm64.apk`): on Android 12+ an
  arm64-only install is accepted.
- The server waits on Android's "All files access" screen until it has `MANAGE_EXTERNAL_STORAGE`. On API 36
  `pm grant` refuses it; `adb shell appops set --uid org.renderdoc.renderdoccmd.arm64 MANAGE_EXTERNAL_STORAGE
  allow` works. Then remove the task left on that screen, or each later launch is swallowed (`result code=3`).
- Package visibility hides the layer app from the test app, exactly as for GFXReconstruct: copy
  `libVkLayer_GLES_RenderDoc.so` into the app's data directory with `run-as`.
- With that, injection succeeds and RenderDoc hooks both HWUI's and MapLibre's Vulkan devices, then MapLibre's
  `createDeviceUnique` throws `ErrorInitializationFailed` and the app exits, with and without
  `MLN_VULKAN_VALIDATION`. RenderDoc logs `Created capture device` before it and
  `Assertion failed: 'm_ResourceMap.empty()'` after it. The cause is not known.

RenderDoc leaves `enable_gpu_debug_layers`, `gpu_debug_app`, `gpu_debug_layers`, `gpu_debug_layer_app` and
`gpu_debug_layers_gles` set after a run: delete them as in the GFXReconstruct cleanup, or the next plain launch
of the app loads the layer again.

Once a `.rdc` exists, export state as text for the agent, on Windows or Linux:

```bash
rdc-cli replay --cmds --pipeline-state output_state.txt capture_frame.rdc
```

The in-app hooks in `src/mln/vulkan/renderer_backend.cpp` (`ENABLE_RENDERDOC_FRAME_CAPTURE`,
`triggerFrameCapture`) are dead code today: the macro is never defined and `renderdoc_app.h` is not vendored.
MCP servers that open `.rdc` files exist (`JiaboLi-GitHub/renderdoc-mcp`, `Linkingooo/renderdoc-mcp`,
`halby24/RenderDocMCP`); all document Windows or Linux only and none was tested with this project.

## Reporting

Quote the evidence: the screenshot path, the exact call or state line, and the source line that produced it.
"The capture shows X at call N, set by `file:line`" is a finding. A cause without a quoted line is a guess: say so.
