---
name: metal-capture
description: Gets GPU-side evidence for a Metal rendering bug in MapLibre as text and PNGs an agent can read - screenshots, GPU traces taken with gpucapture and browsed with gpudebug (draw state, pipelines, bindings, attachment and texture contents), shader os_log output, command-buffer error status and Metal System Trace timings. Use when validation is quiet but the frame is wrong (black, flipped, missing draw, wrong depth or blending) on macOS or iOS, or to check what actually reached the GPU.
---

# Metal capture

A wrong frame with quiet validation is a state bug: the calls are legal, the values are wrong. Read the state the
GPU received instead of reasoning from the source. Go down this list and stop at the first level that explains
the frame. Evidence goes in `OUT=$(mktemp -d)`, never in the repo.

The `gpucapture`, `gpudebug` and `metalperftrace` CLIs ship with macOS 27 (`/usr/bin`). Their man pages are the
reference: `man gpudebug | col -b` before using a command not shown here.

## 1. Screenshot (always first)

- macOS render test: the runner writes `actual.png` next to `expected.png` in the test directory and a diff into
  the HTML report (`metrics/macos-xcode11-release-style.html`, move it to `$OUT`). One frame without a test:
  `cmake --build build-macos-metal --target mbgl-render`, then
  `build-macos-metal/bin/mbgl-render -z 7 -x -74 -y 41 --style <style url> -o "$OUT/frame.png"`.
- iOS simulator: `xcrun simctl io "$UDID" screenshot "$OUT/frame.png"` (UDID claimed through `berth`).
- Then Read the PNG. Compare with the same view on the `opengl` or `vulkan` build, and with `expected.png`.

State the question before going deeper ("is the terrain mesh drawn but untextured, or not drawn?"). `TERRAIN.md`
maps many terrain pictures to causes, including Metal ones (black bands = missing 3D depth state in
`mtl::LayerGroup`). Check it first.

## 2. GPU trace: capture with gpucapture

No code change: the process only needs `MTL_CAPTURE_ENABLED=1` at launch. `MTLCAPTURE_WAIT_FOR_SIGNAL=1` makes it
stop at `MTLDevice` creation until a capture starts, which is the way to catch a short-lived process.

**Run these outside the command sandbox** (Bash `dangerouslyDisableSandbox`): inside it the target never
registers with `gputoolsserviced` and `gpucapture list` says `no capturable processes available`.

macOS render test (verified on macOS 27.2, `terrain/default`, about 6 MB):

```bash
(MTL_CAPTURE_ENABLED=1 MTLCAPTURE_WAIT_FOR_SIGNAL=1 build-macos-metal/mbgl-render-test-runner \
   --manifestPath=metrics/macos-xcode11-release-style.json -f 'terrain/default' > "$OUT/run.log" 2>&1 &)
until gpucapture list >/dev/null 2>&1; do sleep 1; done   # the runner reaches device creation after ~10-20 s
P=$(pgrep -x mbgl-render-test-runner)                      # -x: 'pgrep -f' matches your own shell too
gpucapture boundaries --pid "$P"                           # Device, Queue, Layer, Scope boundaries
gpucapture start --pid "$P" --until-exit --output "$OUT/frame.gputrace"
```

`--until-exit` records every command buffer until the process exits: right for a test runner, which has no
`CAMetalLayer`. One test is one frame plus its offscreen passes (terrain drape, hillshade prepare), each its own
command buffer.

macOS app: launch it with `MTL_CAPTURE_ENABLED=1`, bring the map to the broken state, then
`gpucapture start --pid P --count 1 --output "$OUT/frame.gputrace"`. With a `CAMetalLayer` (`MTKView`) the default
boundary is the Layer: `--count N` is N presented frames.

**iOS simulator: `gpucapture` does not see simulator processes.** A process spawned with
`SIMCTL_CHILD_MTL_CAPTURE_ENABLED=1` never appears in `gpucapture list`, and `gpucapture --list-devices` shows
only the Mac with `Remote device browsing requires Xcode to be running`. That held with Xcode 27.2 closed, open,
and open with a workspace, from the agent's shell (outside the sandbox) and from the user's own terminal. The
simulator runtime has its own `gputoolsserviced` but no `gpucapture`. Do not spend time on it: capture from
inside the process instead. Verified with a standalone simulator
binary: `SIMCTL_CHILD_MTL_CAPTURE_ENABLED=1`, then `MTLCaptureManager` with destination `gpuTraceDocument` and an
`outputURL` on the Mac's filesystem (simulator processes write host paths), and `gpudebug` reads the trace on the
Mac. In MapLibre that is a **local, uncommitted** edit of the in-tree hook in `src/mln/renderer/renderer_impl.cpp`:

- set `EnableMetalCapture = 1`, `CaptureFrameStart` to the first frame to keep (> 0), `CaptureFrameCount`;
- replace `CaptureDestinationDeveloperTools` (it needs Xcode attached) with `CaptureDestinationGPUTraceDocument`
  and add `captureDesc->setOutputURL(NS::URL::fileURLWithPath(MTLSTR("/absolute/path/frame.gputrace")))`;
  check both names with `mtl-lookup.py` first.

Then run the iOS unit test or the app with `MTL_CAPTURE_ENABLED=1` (`--test_env=` / `SIMCTL_CHILD_`), and revert.
The same edit works on macOS when `gpucapture` is not an option.

Device: `gpucapture list` shows devices only while Xcode is running; not tried.

## 3. GPU trace: read it with gpudebug

Never start the interactive REPL. Open a session once, reuse it, terminate it:

```bash
gpudebug -q -t "$OUT/frame.gputrace" -o "$OUT/fetch" -c "list"    # prints "Session N created."
S=N
gpudebug -s $S -c "find TerrainShader"          # pipelines are labelled <ShaderName>#<hash>; finds the draws
gpudebug -s $S -c "go /commands/cb2/re0" -c "list"   # color0 / depth / stencil attachments, debug groups
gpudebug -s $S -c "go <draw path>" -c "info"    # selector + arguments, cull mode, winding, depth-stencil state
gpudebug -s $S -c "go <draw path>" -c "info pipeline" -c "go vertex" -c "list"   # pipeline, bound buffers/textures by [[buffer(N)]]
gpudebug -s $S -c "status"                      # 'Replayer: ready' before any fetch
gpudebug -s $S -c "go /commands/cb2/re0" -c "fetch color0 --out $OUT/color0.png" -c "fetch depth --out $OUT/depth.png"
gpudebug -s $S -c "go <draw path>/vertex" -c "fetch buf[3] --out $OUT/ubo.bin"   # a UBO, inline setBytes data too
gpudebug --terminate $S
```

- Draws sit under the backend's debug groups (`main buffer` > `drawables-opaque` / `drawables-translucent` >
  per-layer and, in Debug builds, per-drawable groups from `drawable.cpp` and `*layer_group.cpp`). Use `find`
  rather than walking the tree; `find drawIndexedPrimitives` lists every draw with its arguments.
- `--json` on any command gives machine-readable output.
- Buffer indices are MapLibre's: `idGlobalPaintParamsUBO` = 0, `idGlobalUBOIndex` = 1 (the `uboIndex` pushed
  with `setVertexBytes`), then the shader's UBO ids, then vertex attribute buffers after all UBOs
  (`include/mln/shaders/layer_ubo.hpp`, the shader's `src/mln/shaders/mtl/*.cpp`). Decode a fetched `.bin` with
  the C++ UBO struct (`include/mln/shaders/*_ubo.hpp`): `python3 -c "import struct; ..."`.
- Fetched textures are PNGs: Read them. A depth attachment PNG that is uniform means nothing was drawn into it
  or it was never stored (offscreen depth uses `StoreActionDontCare`).
- Questions worth asking of a trace: is the draw there at all; which pipeline and which buffers and textures are
  bound at that draw; cull mode, winding and depth-stencil state at the draw; the attachment load and store
  actions (`info color0`); the viewport depth range (`api_calls`, the backend applies `DepthMode.range` through
  `setViewport` znear/zfar).

## 4. Shader printf (MSL os_log)

For "which branch does this pixel take" questions. Needs MSL 3.2 and macOS 15 / iOS 18, while the backend
compiles MSL 2.4: a **local, uncommitted** experiment only. In `Context::createProgram`
(`src/mln/mtl/context.cpp`) set `options->setLanguageVersion(MTL::LanguageVersion3_2)` and
`options->setEnableLogging(true)` (check both with `mtl-lookup.py`), add to the shader
`os_log_default.log_info("tile %d z %f", ...)` guarded to one pixel or vertex, and run with
`MTL_LOG_TO_STDERR=1` (`MTL_LOG_BUFFER_SIZE` raises the 1 KB default). Revert before committing.

## 5. Command-buffer errors

A GPU fault or timeout ends a command buffer with an error, not an assertion. In a local experiment, create the
command buffer from an `MTL::CommandBufferDescriptor` with
`setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus)` and log `commandBuffer->error()` and its
encoder infos in a completed handler: it names the encoder that faulted. Command buffers are created in
the platform glue (`MLNMapView+Metal.mm`, `headless_backend.cpp`, `offscreen_texture.cpp`).

## 6. Timing (performance, not correctness)

- `xcrun xctrace record --template 'Metal System Trace' --time-limit 10s --no-prompt --output "$OUT/m.trace"
  --launch -- <binary> <args>`, then `xcrun xctrace export --input "$OUT/m.trace" --toc` and `--xpath` on the
  table you need; `xctrace help` lists the newer `show` / `triage` commands. Encoder and command-buffer labels
  make the timeline readable.
- `metalperftrace collect --last 30s "$OUT/perf"` then `metalperftrace overview --json <trace.atrc>`: always-on
  frame statistics, no relaunch (`man metalperftrace`).
- `gpudebug -s $S -c "profile load"` adds per-encoder, per-draw and per-shader GPU cost to a trace (`profile run`
  needs an M3 / A17 or newer).

## Reporting

Quote the evidence: the screenshot or fetched PNG path, the trace node path and the state line, and the source
line that produced it. "`gpudebug` shows CullMode Back on `/commands/cb2/.../draw0` (TerrainShader), set by
`drawable.cpp:223`" is a finding. A cause without a quoted line is a guess: say so.
