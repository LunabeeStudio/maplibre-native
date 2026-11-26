---
name: metal-spec-lookup
description: Looks a Metal API name (ObjC or metal-cpp), a Metal Shading Language function or type, an MSL spec rule or a per-GPU-family limit up in the SDK, toolchain and Apple specs this machine builds with, and flags anything newer than the iOS/macOS deployment targets. Use before writing or reviewing any Metal call, MSL builtin or attribute you would otherwise recall from memory.
---

# Metal spec lookup

The SDK headers, the vendored metal-cpp and Apple's specification are ground truth. A Metal name recalled from
memory is a guess until this script prints it: Metal 4 (`MTL4*`, iOS/macOS 26) and Metal 3 APIs look alike, and
metal-cpp renames everything.

```bash
S=${CLAUDE_SKILL_DIR}/scripts/mtl-lookup.py   # fork: MLN_REPO, else the git top level, else ~/dev/maplibre-native
$S MTL::RenderCommandEncoder::setVertexBytes   # metal-cpp method -> C++ signature, ObjC selector, declaration, availability
$S MTL::RenderPassDescriptor                   # metal-cpp class -> ObjC class with every member and its availability
$S MTLRenderPassDescriptor.tileWidth           # ObjC property or selector (setTriangleFillMode: works too)
$S MTL::PixelFormatDepth32Float_Stencil8       # enum value (MTLPixelFormat... works too); a typedef NS_ENUM lists its values
$S MTLCreateSystemDefaultDevice                # C function or constant
$S CAMetalLayer.maximumDrawableCount           # QuartzCore and MetalKit (MTKView) are searched too
$S --msl dfdx                                  # MSL stdlib declaration and the #if guards around it
$S --spec "[[invariant]]"                      # MSL specification text (attributes, address spaces, rules)
$S --limits "texture argument"                 # Metal feature set tables, columns are GPU families
$S --search Residency                          # names containing a substring, ObjC + metal-cpp + MSL
```

The first `--spec` or `--limits` call downloads the PDF from developer.apple.com into `~/.cache/metal-spec` and
extracts its text with `uv run --with pypdf` (about 10 s, once). Later calls read the cache. `grep -n -i` on the
`.txt` file there works too when the query needs regex or more context.

## Reading the result

- **`WARNING: needs ios X, deployment target is 16.0`**: the fork ships iOS 16.0 (`platform/ios/BUILD.bazel`) and
  macOS 14.3 (`CMakeLists.txt`); the script reads both. A newer API needs a run-time check (metal-cpp has no
  `@available`: check `respondsToSelector` on the object, or `device->supportsFamily(...)`) and a fallback, or an
  older API. Everything `MTL4*` falls here.
- **`UNAVAILABLE on ios`**: the API does not exist on iOS (for example `MTLPixelFormatDepth24Unorm_Stencil8`).
  The backend must compile for both.
- **"does not exist in vendor/metal-cpp"**: the C++ name is invented or misspelled. Use `--search`. metal-cpp maps
  `MTLFooBar` to `MTL::FooBar`, `MTLPixelFormatX` to `MTL::PixelFormatX`, a selector `setFoo:atIndex:` to
  `setFoo(..., index)`, and a getter `isFoo` to `isFoo()` or `foo()`. Look the C++ form up, then read the ObjC
  declaration it prints: the doc comment there is Apple's.
- **availability inherited from the class**: the member has no annotation of its own; the class's applies.
- **MSL version**: the backend compiles shaders at run time with `MTL::LanguageVersion2_4`
  (`src/mln/mtl/context.cpp`, `Context::createProgram`). A stdlib declaration inside
  `#if __METAL_VERSION__ >= 300` (or a feature guard such as `__HAVE_LOGGING__`) is unusable there until that
  version is raised, which also needs the matching OS minimum (MSL 3.0: iOS 16, MSL 3.2: iOS 18).
  `msl-check.py` in the `metal-validate` skill compiles the shaders offline with the same version.
- **`--limits`**: the `[columns]` line names the GPU family of each number. Footnote digits are glued to the
  label (`... function 2 31 31 ...`: the `2` is a footnote). The iOS simulator does not report the device's
  family: check limits against the lowest family the app supports, Apple3 (A9 iPads still run iPadOS 16).

## Other sources

- Man pages are part of macOS 27 and are authoritative for the tools: `man MetalValidation | col -b`,
  `man gpucapture`, `man gpudebug`, `man metalperftrace`, `man MetalPerformanceHUD`.
- Any Apple documentation page as JSON (the HTML is rendered by JavaScript):
  `curl -s https://developer.apple.com/tutorials/data/documentation/metal/mtlcapturemanager.json`, path lower
  case as in the page URL. Prose only: availability and signatures still come from the headers.
- Apple's own Metal agent skills (validation, gpucapture, gpudebug, rendering-issue symptom router, metal-cpp
  lifetimes) ship in `github.com/apple/game-porting-toolkit` (`game-porting-skills`). They are generic, written
  for game ports; the `metal-*` skills here carry the MapLibre specifics. Install them only if the user asks:
  `/plugin marketplace add apple/game-porting-toolkit`.
