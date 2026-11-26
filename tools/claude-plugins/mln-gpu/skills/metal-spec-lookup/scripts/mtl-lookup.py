#!/usr/bin/env python3
"""Look a Metal API name, MSL name or spec term up in the sources this machine and repo build with.

Usage:
  mtl-lookup.py MTLRenderPassDescriptor                  class or protocol: declaration, availability, members
  mtl-lookup.py MTL::RenderPassDescriptor                same, from the metal-cpp name
  mtl-lookup.py MTL::RenderCommandEncoder::setVertexBytes   metal-cpp method: C++ signature, selector, ObjC declaration
  mtl-lookup.py MTLRenderPassDescriptor.tileWidth        ObjC member (property or selector)
  mtl-lookup.py MTLPixelFormatDepth32Float_Stencil8      enum value or constant (MTL::PixelFormatDepth32Float_Stencil8 too)
  mtl-lookup.py --msl dfdx                               MSL standard library declaration and its version guard
  mtl-lookup.py --spec "[[invariant]]"                   Metal Shading Language specification text
  mtl-lookup.py --limits "buffer argument"               Metal feature set tables (per GPU family limits)
  mtl-lookup.py --search Residency                       names containing a substring (ObjC, metal-cpp, MSL)

Sources: the iOS SDK Metal, MetalKit and QuartzCore headers of the selected Xcode (xcrun), the vendored metal-cpp
(vendor/metal-cpp) of the maplibre-native fork, the Metal toolchain's MSL headers, and Apple's MSL specification and
feature set tables PDFs, downloaded once into ~/.cache/metal-spec.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

MAX_MATCHES = 12
CACHE = Path(os.environ.get("METAL_SPEC_CACHE", "~/.cache/metal-spec")).expanduser()
PDFS = {
    "spec": "https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf",
    "limits": "https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf",
}
# Fallbacks when the build files cannot be read: platform/ios/BUILD.bazel and CMakeLists.txt.
DEFAULT_TARGETS = {"ios": "16.0", "macos": "14.3"}


def die(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""



def _fork_or_default(top):
    """`top` if it is the fork, else ~/dev/maplibre-native when that is (e.g. run from the Cumbly repo)."""
    top = Path(top)
    default = Path("~/dev/maplibre-native").expanduser()
    if not (top / "src" / "mln").is_dir() and (default / "src" / "mln").is_dir():
        return default
    return top


def find_repo():
    """The maplibre-native fork: $MLN_REPO, else the git checkout holding the current directory, else ~/dev/maplibre-native."""
    if os.environ.get("MLN_REPO"):
        return Path(os.environ["MLN_REPO"]).expanduser().resolve()
    top = run(["git", "rev-parse", "--show-toplevel"])
    return _fork_or_default(top or os.getcwd())


REPO = find_repo()
METAL_CPP = REPO / "vendor" / "metal-cpp"


def deployment_targets():
    targets = dict(DEFAULT_TARGETS)
    bazel = REPO / "platform" / "ios" / "BUILD.bazel"
    if bazel.exists():
        found = re.findall(r'minimum_os_version\s*=\s*"([\d.]+)"', bazel.read_text())
        if found:
            targets["ios"] = min(found, key=version_key)
    cmake = REPO / "CMakeLists.txt"
    if cmake.exists():
        found = re.search(r'set\(CMAKE_OSX_DEPLOYMENT_TARGET "([\d.]+)"\)', cmake.read_text())
        if found:
            targets["macos"] = found.group(1)
    return targets


def version_key(version):
    return tuple(int(part) for part in version.split(".") if part.isdigit())


# ---------------------------------------------------------------- SDK headers


def sdk_header_dirs():
    sdk = run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"]) or run(["xcrun", "--show-sdk-path"])
    if not sdk:
        die("xcrun found no SDK: install Xcode and run 'sudo xcode-select -s /Applications/Xcode.app'")
    frameworks = Path(sdk) / "System" / "Library" / "Frameworks"
    dirs = [frameworks / name / "Headers" for name in ("Metal.framework", "MetalKit.framework")]
    return sdk, [d for d in dirs if d.is_dir()] + [frameworks / "QuartzCore.framework" / "Headers"]


def sdk_headers():
    _, dirs = sdk_header_dirs()
    files = []
    for d in dirs:
        if d.name == "Headers" and d.parent.name == "QuartzCore.framework":
            files += [f for f in d.glob("CAMetal*.h")]
        else:
            files += sorted(d.glob("*.h"))
    return files


def read_lines(path):
    return path.read_text(errors="replace").splitlines()


def availability(text):
    """API_AVAILABLE / API_UNAVAILABLE / API_DEPRECATED fragments of a declaration."""
    return re.findall(r"API_(?:AVAILABLE|UNAVAILABLE|DEPRECATED(?:_WITH_REPLACEMENT)?)\((?:[^()]|\([^()]*\))*\)", text)


def check_targets(annotations, targets):
    """Warnings against the deployment targets, or [] when the declaration is usable on both."""
    text = " ".join(annotations)
    if not annotations:
        return []
    warnings = []
    for platform, minimum in targets.items():
        if re.search(rf"API_UNAVAILABLE\([^)]*\b{platform}\b", text):
            warnings.append(f"UNAVAILABLE on {platform}")
            continue
        introduced = re.search(rf"API_AVAILABLE\((?:[^()]|\([^()]*\))*\b{platform}\(([\d.]+)\)", text)
        if introduced and version_key(introduced.group(1)) > version_key(minimum):
            warnings.append(
                f"needs {platform} {introduced.group(1)}, deployment target is {minimum}: guard it at run time "
                "(metal-cpp: respondsToSelector / supportsFamily) or pick an older API"
            )
        deprecated = re.search(rf"API_DEPRECATED[^;]*\b{platform}\([\d.]+,\s*([\d.]+)\)", text)
        if deprecated:
            warnings.append(f"deprecated on {platform} since {deprecated.group(1)}")
    return warnings


def statement(lines, start):
    """The declaration starting at lines[start], up to its ';' or '{' (several lines for long selectors)."""
    parts = []
    for i in range(start, min(start + 12, len(lines))):
        parts.append(lines[i].strip())
        if lines[i].rstrip().endswith((";", "{")) or (i > start and lines[i].strip() == ""):
            break
    return " ".join(parts)


def doc_comment(lines, start):
    """The /*! ... */ or /** ... */ block right above a declaration, abstract first, at most 6 lines."""
    end = start - 1
    while end >= 0 and lines[end].strip() == "":
        end -= 1
    if end < 0 or not lines[end].strip().endswith("*/"):
        return []
    begin = end
    while begin > 0 and not re.match(r"\s*/\*", lines[begin]):
        begin -= 1
    body = [re.sub(r"^\s*(/\*[*!]?|\*/|\*)\s?", "", line).rstrip() for line in lines[begin : end + 1]]
    body = [re.sub(r"\*/$", "", line).strip() for line in body if line.strip() not in ("", "*/")]
    return body[:6]


def strip_comments(lines):
    """The same lines with comments blanked, so declarations quoted in doc comments are not matched."""
    code, in_block = [], False
    for line in lines:
        out, i = "", 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                else:
                    in_block, i = False, end + 2
            elif line.startswith("/*", i):
                in_block, i = True, i + 2
            elif line.startswith("//", i):
                break
            else:
                out += line[i]
                i += 1
        code.append(out)
    return code


class Block:
    """An @interface or @protocol block of an ObjC header."""

    def __init__(self, path, lines, start, end, kind, name, code):
        self.path, self.lines, self.start, self.end, self.kind, self.name = path, lines, start, end, kind, name
        self.code = code

    def header(self):
        # MTL_EXPORT API_AVAILABLE(...) is often on the line above @interface.
        above = self.code[self.start - 1] if self.start > 0 else ""
        return (above.strip() + " " + statement(self.code, self.start)).strip()


def objc_blocks(files):
    blocks = []
    for path in files:
        lines = read_lines(path)
        code = strip_comments(lines)
        i = 0
        while i < len(lines):
            match = re.match(r"\s*@(interface|protocol)\s+(\w+)", code[i])
            if match and not re.match(r"\s*@protocol\s+\w+\s*[;,]", code[i]):
                end = i + 1
                while end < len(lines) and not code[end].startswith("@end"):
                    end += 1
                blocks.append(Block(path, lines, i, end, match.group(1), match.group(2), code))
                i = end
            i += 1
    return blocks


def skip_parens(text, i):
    depth = 0
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def selector_of(declaration):
    """'- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index' -> selector."""
    text = re.sub(r"^\s*[-+]\s*", "", declaration)
    if text.startswith("("):
        text = text[skip_parens(text, 0) :]
    parts, i = [], 0
    while True:
        match = re.compile(r"\s*(\w+)\s*(:?)").match(text, i)
        if not match or match.group(1).startswith(("API_", "NS_", "MTL_", "__")):
            break
        if not match.group(2):
            return match.group(1) if not parts else "".join(parts)
        parts.append(match.group(1) + ":")
        i = match.end()
        while i < len(text) and text[i] == " ":
            i += 1
        if i < len(text) and text[i] == "(":
            i = skip_parens(text, i)
        param = re.compile(r"\s*\w+").match(text, i)
        i = param.end() if param else i
    return "".join(parts)


def members(block):
    """(kind, name or selector, line index) for each property and method of a block."""
    out = []
    i = block.start + 1
    while i < block.end:
        line = block.code[i]
        if re.match(r"\s*@property\b", line):
            decl = statement(block.code, i)
            names = re.findall(r"(\w+)\s*(?:API_|NS_|MTL_|__|;|\[)", re.sub(r"\(\s*[^)]*\)", "", decl, count=1))
            getter = re.search(r"getter\s*=\s*(\w+)", decl)
            name = names[0] if names else "?"
            out.append(("property", name, i, getter.group(1) if getter else None))
        elif re.match(r"\s*[-+]\s*\(", line):
            out.append(("method", selector_of(statement(block.code, i)), i, None))
        i += 1
    return out


def print_member(block, kind, name, index, targets, class_annotations):
    decl = statement(block.code, index)
    print(f"{kind} {block.name}.{name}    ({block.path.name}:{index + 1})")
    for line in doc_comment(block.lines, index):
        print(f"  // {line}")
    print(f"  {decl}")
    annotations = availability(decl) or class_annotations
    if not availability(decl) and class_annotations:
        print("  availability: inherited from the class")
    for warning in check_targets(annotations, targets):
        print(f"  WARNING: {warning}")


def show_block(block, targets, list_members=True):
    header = block.header()
    print(f"@{block.kind} {block.name}    ({block.path.name}:{block.start + 1})")
    for line in doc_comment(block.lines, block.start - (1 if "API_" in block.lines[block.start - 1] else 0)):
        print(f"  // {line}")
    print(f"  {header}")
    for warning in check_targets(availability(header), targets):
        print(f"  WARNING: {warning}")
    if list_members:
        for kind, name, index, getter in members(block):
            decl = statement(block.code, index)
            avail = " ".join(availability(decl))
            label = f"{name} (getter={getter})" if getter else name
            print(f"    {kind:8} {label}  {avail}".rstrip())


def find_blocks(name):
    return [b for b in objc_blocks(sdk_headers()) if b.name == name]


def show_objc_member(class_name, member, targets):
    """member is a property name, a getter, or a full or partial selector."""
    found = False
    for block in find_blocks(class_name):
        class_annotations = availability(block.header())
        for kind, name, index, getter in members(block):
            setter = "set" + name[:1].upper() + name[1:] + ":" if kind == "property" else None
            first_part = name.split(":")[0]
            if member in (name, getter, setter) or (":" not in member and member == first_part) or (
                member.endswith(":") and name.startswith(member)
            ):
                print_member(block, kind, name, index, targets, class_annotations)
                print()
                found = True
    return found


def enclosing_enum_availability(code, index):
    """An enum value without its own annotation has the availability of its typedef NS_ENUM / NS_OPTIONS."""
    start = index
    while start > 0 and not re.match(r"\s*typedef\s+NS_(?:ENUM|OPTIONS|CLOSED_ENUM)", code[start]):
        if code[start].strip().startswith("}"):
            return []
        start -= 1
    end = index
    while end < len(code) and not code[end].strip().startswith("}"):
        end += 1
    if start <= 0 or end >= len(code):
        return []
    annotations = availability(code[start] + " " + code[end] + " " + (code[start - 1] if start else ""))
    typedef = re.search(r"NS_\w+\(\s*\w+\s*,\s*(\w+)\)", code[start])
    label = typedef.group(1) if typedef else "the enum"
    if annotations:
        print(f"  availability: inherited from {label}: {' '.join(annotations)}")
    else:
        print(f"  availability: none on the value nor on {label}: available since Metal itself (iOS 8, macOS 10.11)")
    return annotations


def show_symbol(name, targets):
    """Enum values, constants and C functions: any declaration line naming it."""
    hits = 0
    for path in sdk_headers():
        lines = read_lines(path)
        code = strip_comments(lines)
        for i, line in enumerate(code):
            if re.search(rf"\b{re.escape(name)}\b", line) and not line.strip().startswith(("@property", "-", "+")):
                if not re.match(rf"\s*(?:typedef\s+NS_\w+\(\w+,\s*)?{re.escape(name)}\b|.*\b{re.escape(name)}\s*[(=,;)]", line):
                    continue
                decl = statement(code, i) if "(" in line and "=" not in line else line.strip()
                print(f"{path.name}:{i + 1}: {decl}")
                for comment in doc_comment(lines, i)[:3]:
                    print(f"  // {comment}")
                annotations = availability(decl)
                if not annotations and not line.lstrip().startswith("typedef"):
                    annotations = enclosing_enum_availability(code, i)
                for warning in check_targets(annotations, targets):
                    print(f"  WARNING: {warning}")
                hits += 1
                if line.lstrip().startswith("typedef NS_"):
                    values = []
                    for follow in code[i + 1 : i + 400]:
                        if follow.strip().startswith("}"):
                            break
                        value = re.match(r"\s*(MTL\w+|CA\w+)\b(.*)", follow)
                        if value:
                            values.append(f"    {value.group(1)}{' ' + ' '.join(availability(value.group(2))) if 'API_' in value.group(2) else ''}")
                    print("\n".join(values[:80]))
                if hits >= MAX_MATCHES:
                    return hits
    return hits


# ---------------------------------------------------------------- metal-cpp


def metal_cpp_files():
    if not METAL_CPP.is_dir():
        return []
    return sorted(p for p in METAL_CPP.rglob("*.hpp") if "SingleHeader" not in p.parts)


def bridge_selectors():
    selectors = {}
    for path in METAL_CPP.rglob("*HeaderBridge.hpp"):
        text = path.read_text(errors="replace")
        for key, value in re.findall(r'_\w+_PRIVATE_DEF_SEL\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)', text):
            selectors[key] = value
    return selectors


def cpp_to_objc_class(namespace, name):
    prefix = {"MTL": "MTL", "MTL4": "MTL4", "CA": "CA", "MTK": "MTK", "NS": "NS"}.get(namespace, namespace)
    return prefix + name


def show_cpp(qualified, targets):
    """MTL::Class, MTL::Class::method or MTL::EnumValue from metal-cpp, mapped to the ObjC declaration."""
    parts = qualified.split("::")
    if not metal_cpp_files():
        die(f"no metal-cpp at {METAL_CPP}: run from the maplibre-native fork root, or set MLN_REPO")
    namespace = parts[0]
    if len(parts) == 2:
        name = parts[1]
        for path in metal_cpp_files():
            lines = read_lines(path)
            for i, line in enumerate(lines):
                if re.match(rf"\s*class\s+{re.escape(name)}\s*:", line):
                    print(f"metal-cpp: {line.strip()}    (vendor/metal-cpp/{path.relative_to(METAL_CPP)}:{i + 1})")
                    blocks = find_blocks(cpp_to_objc_class(namespace, name))
                    for block in blocks:
                        show_block(block, targets)
                    if not blocks:
                        print(f"  no @interface/@protocol {cpp_to_objc_class(namespace, name)} in the SDK headers")
                    return True
        # an enum value: MTL::PixelFormatDepth32Float_Stencil8 -> MTLPixelFormatDepth32Float_Stencil8
        for path in metal_cpp_files():
            for i, line in enumerate(read_lines(path)):
                if re.match(rf"\s*{re.escape(name)}\s*=", line):
                    print(f"metal-cpp: {line.strip()}    (vendor/metal-cpp/{path.relative_to(METAL_CPP)}:{i + 1})")
                    show_symbol(cpp_to_objc_class(namespace, name), targets)
                    return True
        return False
    if len(parts) == 3:
        cls, method = parts[1], parts[2]
        selectors = bridge_selectors()
        found = False
        for path in metal_cpp_files():
            text = path.read_text(errors="replace")
            pattern = re.compile(
                rf"_\w+_INLINE\s+([^\n]*?)\b{re.escape(namespace)}::{re.escape(cls)}::{re.escape(method)}\(([^)]*)\)([^{{]*)\{{(.*?)\n\}}",
                re.S,
            )
            for match in pattern.finditer(text):
                found = True
                line_no = text[: match.start()].count("\n") + 1
                print(f"metal-cpp: {match.group(1).strip()} {namespace}::{cls}::{method}({' '.join(match.group(2).split())}){match.group(3).rstrip()}")
                print(f"  (vendor/metal-cpp/{path.relative_to(METAL_CPP)}:{line_no})")
                for key in re.findall(r"_\w+_PRIVATE_SEL\((\w+)\)", match.group(4)):
                    selector = selectors.get(key, key.replace("_", ":"))
                    print(f"  selector: {selector}")
                    if not show_objc_member(cpp_to_objc_class(namespace, cls), selector, targets):
                        print(f"  no {selector} in @interface/@protocol {cpp_to_objc_class(namespace, cls)} (may be declared on a superclass or protocol)")
                    print()
        if not found:
            print(f"no inline definition of {qualified} in vendor/metal-cpp. Known methods of {namespace}::{cls}:")
            for path in metal_cpp_files():
                names = sorted(set(re.findall(rf"{re.escape(namespace)}::{re.escape(cls)}::(\w+)\(", path.read_text(errors="replace"))))
                if names:
                    print("  " + ", ".join(names))
        return True
    return False


# ---------------------------------------------------------------- MSL headers


def msl_header_dir():
    metal = run(["xcrun", "--find", "metal"])
    if not metal:
        die("xcrun cannot find 'metal': install the Metal toolchain (Xcode > Settings > Components, or "
            "'xcodebuild -downloadComponent MetalToolchain')")
    root = Path(metal).resolve().parent.parent
    for candidate in sorted(root.glob("metal/*/lib/clang/*/include/metal")) + sorted(root.glob("lib/clang/*/include/metal")):
        if (candidate / "metal_stdlib").exists():
            return candidate
    die(f"no metal_stdlib under {root}")


def version_guards(lines, index):
    """The #if conditions enclosing a line (MSL version and feature guards)."""
    stack = []
    for line in lines[: index + 1]:
        stripped = line.strip()
        if stripped.startswith("#if"):
            stack.append(stripped)
        elif stripped.startswith("#endif") and stack:
            stack.pop()
        elif stripped.startswith(("#else", "#elif")) and stack:
            stack[-1] = stack[-1] + "  [" + stripped + "]"
    return [g for g in stack if not re.search(r"_H\b|_INCLUDED|__METAL_[A-Z]+_H", g)]


def show_msl(name):
    directory = msl_header_dir()
    print(f"MSL headers: {directory}")
    hits = 0
    pattern = re.compile(rf"(\b{re.escape(name)}\s*\(|\b(?:struct|class|enum class|using|typedef)\s+{re.escape(name)}\b|\b{re.escape(name)}\s*=)")
    for path in sorted(p for p in directory.rglob("*") if p.is_file()):
        lines = read_lines(path)
        for i, line in enumerate(lines):
            if pattern.search(line) and not line.strip().startswith(("//", "*", "#define")):
                guards = version_guards(lines, i)
                print(f"{path.relative_to(directory)}:{i + 1}: {line.strip()[:160]}")
                if guards:
                    print("  inside: " + " && ".join(guards[-3:]))
                hits += 1
                if hits >= MAX_MATCHES:
                    print(f"(first {MAX_MATCHES} matches; refine with --search or the spec)")
                    return
    if not hits:
        print(f"{name} is not declared in the MSL headers. Attributes ([[...]]), address spaces and language rules "
              "are only in the specification: mtl-lookup.py --spec")


# ---------------------------------------------------------------- PDFs


def pdf_text(kind):
    CACHE.mkdir(parents=True, exist_ok=True)
    url = PDFS[kind]
    pdf = CACHE / Path(url).name
    txt = pdf.with_suffix(".txt")
    if txt.exists():
        return txt
    if not pdf.exists():
        print(f"downloading {url} into {CACHE} (once)", file=sys.stderr)
        if subprocess.run(["curl", "-fsSL", "-o", str(pdf), url]).returncode != 0:
            die(f"download failed: {url}")
    print(f"extracting text from {pdf.name} (once, ~10 s)", file=sys.stderr)
    script = (
        "import logging, sys, pypdf\n"
        "logging.disable(logging.CRITICAL)\n"
        "reader = pypdf.PdfReader(sys.argv[1])\n"
        "with open(sys.argv[2], 'w') as out:\n"
        "    for i, page in enumerate(reader.pages):\n"
        "        out.write(f'\\f[page {i + 1}]\\n' + (page.extract_text() or '') + '\\n')\n"
    )
    result = subprocess.run(
        ["uv", "run", "-q", "--with", "pypdf", "python", "-I", "-c", script, str(pdf), str(txt)],
        capture_output=True, text=True,
    )
    if result.returncode != 0 or not txt.exists():
        die("text extraction needs uv (brew install uv): " + result.stderr[-400:])
    return txt


def show_pdf(kind, query, context=6):
    txt = pdf_text(kind)
    lines = txt.read_text(errors="replace").splitlines()
    title = next((l.strip() for l in lines[:12] if l.strip().startswith("Version")), "")
    print(f"{Path(PDFS[kind]).name} {title}  (cache: {txt})")
    needle = query.lower()
    page, shown, last, columns = "?", 0, -100, ""
    for i, line in enumerate(lines):
        if line.startswith("[page ") and line.endswith("]"):
            page = line[6:-1]
            continue
        if line.startswith("GPU family") and "Apple" in line:
            columns = line  # page-continuation headers repeat "GPU family 1" without the names
        if needle in line.lower() and i - last > context:
            print(f"\n--- page {page}, line {i + 1}")
            if kind == "limits" and columns:
                print(f"[columns] {columns}")
            print("\n".join(l for l in lines[max(0, i - 2) : i + context] if not l.startswith("[page ")))
            shown += 1
            last = i
            if shown >= MAX_MATCHES:
                print(f"\n(first {MAX_MATCHES} matches; refine the query, or grep -n -i '{query}' {txt})")
                break
    if not shown:
        print(f"'{query}' not found. The extracted text drops some symbols and joins words: try a shorter query.")


# ---------------------------------------------------------------- search


def search(substring):
    needle = substring.lower()
    names = set()
    for block in objc_blocks(sdk_headers()):
        if needle in block.name.lower():
            names.add(f"objc  @{block.kind} {block.name}")
        for kind, name, _, _ in members(block):
            if needle in name.lower():
                names.add(f"objc  {kind} {block.name}.{name}")
    for path in sdk_headers():
        for token in set(re.findall(r"\b((?:MTL|CA)\w+)\b", path.read_text(errors="replace"))):
            if needle in token.lower():
                names.add(f"objc  {token}")
    for path in metal_cpp_files():
        for cls in set(re.findall(r"\bclass\s+(\w+)\s*:", path.read_text(errors="replace"))):
            if needle in cls.lower():
                namespace = {"Metal": "MTL", "Foundation": "NS", "QuartzCore": "CA", "MetalFX": "MTLFX"}.get(path.parent.name, path.parent.name)
                names.add(f"cpp   {namespace}::{cls}")
    directory = msl_header_dir()
    for path in directory.rglob("*"):
        if path.is_file():
            for token in set(re.findall(r"\b([a-z_][a-z0-9_]{2,})\s*\(", path.read_text(errors="replace"))):
                if needle in token.lower() and not token.startswith("__"):
                    names.add(f"msl   {token}")
    for line in sorted(names)[:200]:
        print(line)
    if len(names) > 200:
        print(f"... {len(names) - 200} more, narrow the substring")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    targets = deployment_targets()
    if args[0] == "--msl" and len(args) > 1:
        return show_msl(args[1])
    if args[0] == "--spec" and len(args) > 1:
        return show_pdf("spec", " ".join(args[1:]))
    if args[0] == "--limits" and len(args) > 1:
        return show_pdf("limits", " ".join(args[1:]), context=10)
    if args[0] == "--search" and len(args) > 1:
        return search(args[1])
    name = args[0]
    print(f"deployment targets: iOS {targets['ios']}, macOS {targets['macos']}")
    if "::" in name:
        if not show_cpp(name, targets):
            die(f"{name} does not exist in vendor/metal-cpp. Use --search.")
        return
    if "." in name:
        cls, member = name.split(".", 1)
        if not show_objc_member(cls, member, targets):
            die(f"no member {member} in @interface/@protocol {cls}. List the class: mtl-lookup.py {cls}")
        return
    blocks = find_blocks(name)
    if blocks:
        for block in blocks:
            show_block(block, targets)
        return
    if not show_symbol(name, targets):
        die(f"{name} does not exist in the SDK Metal headers. Use --search, or --msl for a shading-language name.")


if __name__ == "__main__":
    main()
