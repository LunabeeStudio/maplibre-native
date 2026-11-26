#!/usr/bin/env python3
"""Compile every MapLibre Metal shader offline, the way the runtime assembles and compiles it.

Usage:
  msl-check.py                         all shaders, iOS and macOS targets
  msl-check.py Terrain Fill            only shaders whose name contains one of the words
  msl-check.py --keep DIR Terrain      also write the assembled .metal files into DIR
  msl-check.py -D HAS_UNIFORM_u_color=1 Fill    add a define (the runtime adds HAS_UNIFORM_u_* per style)

The runtime (src/mln/mtl/context.cpp, Context::createProgram) compiles
  shaders::prelude (include/mln/shaders/mtl/common.hpp) + ShaderSource::prelude + ShaderSource::source
with fast math and the MTL::LanguageVersion set there. A shader that fails here fails at run time as an
Event::Shader "compile failed" log followed by assert(false). Passing here does not prove the shader is right:
defines chosen at run time select other code paths, and the binding indices are only checked by the API
validation layer when the shader is used.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path



def _fork_or_default(top):
    """`top` if it is the fork, else ~/dev/maplibre-native when that is (e.g. run from the Cumbly repo)."""
    top = Path(top)
    default = Path("~/dev/maplibre-native").expanduser()
    if not (top / "src" / "mln").is_dir() and (default / "src" / "mln").is_dir():
        return default
    return top


def find_repo():
    if os.environ.get("MLN_REPO"):
        return Path(os.environ["MLN_REPO"]).expanduser().resolve()
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        top = os.getcwd()
    return _fork_or_default(top)


REPO = find_repo()
SHADERS = REPO / "include" / "mln" / "shaders" / "mtl"
CONTEXT = REPO / "src" / "mln" / "mtl" / "context.cpp"
# ProgramParameters::getDefines (src/mln/shaders/program_parameters.cpp): every program gets the pixel ratio.
RUNTIME_DEFINES = ["-DDEVICE_PIXEL_RATIO=2.0"]
RAW = r'R"(?P<delim>\w*)\((?P<body>.*?)\)(?P=delim)"'


def die(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def language_version():
    match = re.search(r"LanguageVersion(\d+)_(\d+)", CONTEXT.read_text()) if CONTEXT.exists() else None
    return f"{match.group(1)}.{match.group(2)}" if match else "2.4"


def deployment_targets():
    ios, macos = "16.0", "14.3"
    bazel = REPO / "platform" / "ios" / "BUILD.bazel"
    if bazel.exists():
        found = re.findall(r'minimum_os_version\s*=\s*"([\d.]+)"', bazel.read_text())
        if found:
            ios = min(found, key=lambda v: tuple(int(p) for p in v.split(".")))
    cmake = REPO / "CMakeLists.txt"
    if cmake.exists():
        found = re.search(r'set\(CMAKE_OSX_DEPLOYMENT_TARGET "([\d.]+)"\)', cmake.read_text())
        if found:
            macos = found.group(1)
    return ios, macos


def raw_strings(text):
    """constexpr auto name = R"(...)"; at namespace scope."""
    return {m.group(1): m.group("body") for m in re.finditer(rf"constexpr\s+auto\s+(\w+)\s*=\s*{RAW}\s*;", text, re.S)}


def shader_sources():
    common = (SHADERS / "common.hpp").read_text()
    common_prelude = raw_strings(common).get("prelude")
    if common_prelude is None:
        die(f"no 'prelude' raw string in {SHADERS / 'common.hpp'}")
    shaders = []
    named = {}
    for path in sorted(SHADERS.glob("*.hpp")):
        named.update(raw_strings(path.read_text()))
    for path in sorted(SHADERS.glob("*.hpp")):
        text = path.read_text()
        starts = list(re.finditer(r"struct\s+ShaderSource<\s*BuiltIn::(\w+)\s*,\s*gfx::Backend::Type::Metal\s*>\s*\{", text))
        for index, block in enumerate(starts):
            # The body runs to the next specialisation: MSL inside the raw strings has its own "};".
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            name, body = block.group(1), text[block.end() : end]

            def member(field):
                literal = re.search(rf"\b{field}\s*=\s*{RAW}", body, re.S)
                if literal:
                    return literal.group("body")
                ref = re.search(rf"\b{field}\s*=\s*(\w+)\s*;", body)
                if ref:
                    return named.get(ref.group(1), common_prelude if ref.group(1) == "prelude" else None)
                return None

            source = member("source")
            if source is None:
                print(f"skip {name} ({path.name}): source not found as a raw string", file=sys.stderr)
                continue
            prelude = member("prelude") or ""
            line = text[: block.start()].count("\n") + 1
            shaders.append((name, f"{path.name}:{line}", common_prelude + prelude + source))
    return shaders


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        print(__doc__)
        return
    keep, defines, filters = None, [], []
    while args:
        arg = args.pop(0)
        if arg == "--keep":
            keep = Path(args.pop(0)).expanduser()
        elif arg == "-D":
            defines.append("-D" + args.pop(0))
        elif arg.startswith("-D"):
            defines.append(arg)
        else:
            filters.append(arg.lower())
    if not SHADERS.is_dir():
        die(f"no {SHADERS}: run from the maplibre-native fork root, or set MLN_REPO")
    version = language_version()
    ios, macos = deployment_targets()
    targets = [
        ("ios", "iphoneos", f"ios-metal{version}", f"air64-apple-ios{ios}"),
        ("macos", "macosx", f"macos-metal{version}", f"air64-apple-macos{macos}"),
    ]
    print(f"MSL {version} (from {CONTEXT.relative_to(REPO)}), targets iOS {ios} / macOS {macos}, fast math on")
    work = Path(keep) if keep else Path(tempfile.mkdtemp(prefix="msl-check-"))
    work.mkdir(parents=True, exist_ok=True)
    failures = 0
    selected = [s for s in shader_sources() if not filters or any(f in s[0].lower() for f in filters)]
    if not selected:
        die("no shader matches " + " ".join(filters))
    for name, origin, source in selected:
        metal = work / f"{name}.metal"
        metal.write_text(source)
        # The runtime defines HAS_UNIFORM_u_<prop> for each property that is constant in the style, so a
        # shader has 2^n variants. Compile the two extremes: every property data-driven, every one constant.
        uniforms = sorted(set(re.findall(r"\bHAS_UNIFORM_(u_\w+)", source)))
        variants = [("attributes", [])]
        if uniforms:
            variants.append(("uniforms", [f"-DHAS_UNIFORM_{u}" for u in uniforms]))
        for variant, variant_defines in variants:
            for label, sdk, std, target in targets:
                cmd = ["xcrun", "-sdk", sdk, "metal", f"-std={std}", "-target", target, "-ffast-math",
                       *RUNTIME_DEFINES, *variant_defines, *defines, "-c", str(metal), "-o", os.devnull]
                result = subprocess.run(cmd, capture_output=True, text=True)
                diagnostics = [l for l in result.stderr.splitlines() if l.strip()]
                status = "ok" if result.returncode == 0 else "FAILED"
                warnings = sum(": warning:" in l for l in diagnostics)
                print(f"{status:6} {label:5} {variant:10} {name}  ({origin}){f'  {warnings} warnings' if warnings else ''}")
                if result.returncode != 0:
                    failures += 1
                    errors = [l for l in diagnostics if ": error:" in l]
                    for l in (errors or diagnostics)[:12]:
                        print("    " + l.replace(str(work) + "/", ""))
    print(f"\n{len(selected)} shaders, {failures} failed compiles. Assembled sources: {work}")
    print("Line numbers count from the top of the assembled file: common.hpp prelude, then the shader's prelude, then its source.")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
