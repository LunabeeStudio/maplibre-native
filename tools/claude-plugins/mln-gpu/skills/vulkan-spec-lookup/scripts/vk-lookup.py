#!/usr/bin/env python3
"""Look a Vulkan name or VUID up in the registry this repo compiles against.

Usage:
  vk-lookup.py VkImageMemoryBarrier          struct: members, where it was introduced
  vk-lookup.py vkCmdPipelineBarrier          command: parameters, where it was introduced
  vk-lookup.py VK_KHR_swapchain              extension: dependencies, what it adds
  vk-lookup.py VkImageLayout                 enum: values
  vk-lookup.py VUID-vkCmdDraw-None-02699     one valid-usage statement
  vk-lookup.py --vuids vkCmdDraw [filter]    every VUID of a command or struct, optional text filter
  vk-lookup.py --search barrier              names containing a substring

Sources: vendor/Vulkan-Headers/registry/vk.xml and validusage.json.
"""

import html
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def _fork_or_default(top):
    """`top` if it is the fork, else ~/dev/maplibre-native when that is (e.g. run from the Cumbly repo)."""
    top = Path(top)
    default = Path("~/dev/maplibre-native").expanduser()
    if not (top / "src" / "mln").is_dir() and (default / "src" / "mln").is_dir():
        return default
    return top


def find_repo():
    """The maplibre-native fork: $MLN_REPO, else the git checkout holding the current directory, else ~/dev/maplibre-native."""
    import os
    import subprocess
    if os.environ.get("MLN_REPO"):
        return Path(os.environ["MLN_REPO"]).expanduser().resolve()
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        top = os.getcwd()
    return _fork_or_default(top)


REPO = find_repo()
REGISTRY = REPO / "vendor" / "Vulkan-Headers" / "registry"
# The backend requests this version in src/mln/vulkan/renderer_backend.cpp (vk::ApplicationInfo).
BACKEND_VERSION = "VK_VERSION_1_0"
MAX_VUIDS = 40


def die(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def load_xml():
    if not REGISTRY.is_dir():
        die(f"no Vulkan registry at {REGISTRY}: run from the maplibre-native fork root, or set MLN_REPO")
    path = REGISTRY / "vk.xml"
    if not path.exists():
        die(f"{path} is missing. Run: git submodule update --init --depth 1 vendor/Vulkan-Headers")
    return ET.parse(path).getroot()


def load_vuids():
    path = REGISTRY / "validusage.json"
    if not path.exists():
        die(f"{path} is missing. Run: git submodule update --init --depth 1 vendor/Vulkan-Headers")
    return json.loads(path.read_text())["validation"]


def text_of(element):
    return " ".join("".join(element.itertext()).split())


def clean(vuid_text):
    return " ".join(html.unescape(re.sub(r"<[^>]+>", "", vuid_text)).split())


def core_version(feature_name):
    """vk.xml splits each core version into VK_BASE_/VK_COMPUTE_/VK_GRAPHICS_VERSION_x_y: fold them back."""
    return re.sub(r"^VK_(?:[A-Z]+_)?VERSION_(\d+)_(\d+)$", r"VK_VERSION_\1_\2", feature_name)


def introduced_in(root, name):
    """Core versions and extensions whose <require> blocks list the name."""
    found = []
    for feature in root.iter("feature"):
        if "vulkan" not in feature.get("api", "vulkan").split(","):
            continue
        if any(item.get("name") == name for require in feature.iter("require") for item in require):
            found.append(core_version(feature.get("name")))
    for extension in root.iter("extension"):
        if any(item.get("name") == name for require in extension.iter("require") for item in require):
            found.append(extension.get("name"))
    return list(dict.fromkeys(found))


def print_introduced(root, name):
    where = introduced_in(root, name)
    if not where:
        return
    print("Introduced in: " + ", ".join(where))
    if BACKEND_VERSION not in where:
        print(
            f"  NOT in {BACKEND_VERSION}, which is what this backend requests. Using it means raising the API "
            "version or enabling the extension at device creation, and checking support at runtime."
        )


def show_type(root, name):
    for node in root.iter("type"):
        if node.get("name") != name and (node.find("name") is None or node.find("name").text != name):
            continue
        category = node.get("category", "")
        if node.get("alias"):
            print(f"{name} is an alias of {node.get('alias')}")
            return show_type(root, node.get("alias")) or True
        if category in ("struct", "union"):
            print(f"{category} {name}")
            if node.get("structextends"):
                print(f"  extends (pNext of): {node.get('structextends')}")
            for member in node.findall("member"):
                if "vulkan" not in member.get("api", "vulkan").split(","):
                    continue
                comment = member.find("comment")
                if comment is not None:
                    member.remove(comment)
                notes = [
                    f"{key}={member.get(key)}"
                    for key in ("values", "optional", "len", "limittype")
                    if member.get(key)
                ]
                print(f"  {text_of(member)}" + (f"    [{', '.join(notes)}]" if notes else ""))
            print_introduced(root, name)
            return True
        if category in ("enum", "bitmask", "handle", "basetype", "funcpointer"):
            print(f"{category} {name}: {text_of(node) or '(see enum values)'}")
            if category == "bitmask" and (node.get("requires") or node.get("bitvalues")):
                bits = node.get("requires") or node.get("bitvalues")
                print(f"  bits: {bits}")
                show_enum(root, bits, quiet=True)
            elif category == "enum":
                show_enum(root, name, quiet=True)
            print_introduced(root, name)
            return True
    return False


def show_enum(root, name, quiet=False):
    values = []
    for enums in root.iter("enums"):
        if enums.get("name") == name:
            for e in enums.findall("enum"):
                value = e.get("value") or e.get("alias") or (f"bit {e.get('bitpos')}" if e.get("bitpos") else "")
                values.append((e.get("name"), value, "core"))
    for container in list(root.iter("feature")) + list(root.iter("extension")):
        for enum in container.iter("enum"):
            if enum.get("extends") == name:
                values.append((enum.get("name"), enum.get("alias") or "", core_version(container.get("name"))))
    if not values:
        return False
    if not quiet:
        print(f"enum {name}")
    for value_name, value, origin in values:
        print(f"  {value_name}" + (f" = {value}" if value else "") + (f"    ({origin})" if origin != "core" else ""))
    return True


def show_enum_value(root, name):
    for enums in root.iter("enums"):
        for enum in enums.findall("enum"):
            if enum.get("name") == name:
                print(f"{name}: value of {enums.get('name')} (core)")
                return True
    for container in list(root.iter("feature")) + list(root.iter("extension")):
        for enum in container.iter("enum"):
            if enum.get("name") == name and enum.get("extends"):
                origin = core_version(container.get("name"))
                print(f"{name}: value of {enum.get('extends')}, added by {origin}")
                if origin != BACKEND_VERSION:
                    print(f"  NOT in {BACKEND_VERSION}, which is what this backend requests.")
                return True
    return False


def show_command(root, name):
    for command in root.iter("command"):
        if command.get("alias") and command.get("name") == name:
            print(f"{name} is an alias of {command.get('alias')}")
            return show_command(root, command.get("alias")) or True
        proto = command.find("proto")
        if proto is None or proto.find("name").text != name:
            continue
        print(f"{text_of(proto)}(")
        for param in command.findall("param"):
            if "vulkan" not in param.get("api", "vulkan").split(","):
                continue
            notes = [f"{key}={param.get(key)}" for key in ("optional", "len", "externsync") if param.get(key)]
            print(f"  {text_of(param)}" + (f"    [{', '.join(notes)}]" if notes else ""))
        print(")")
        for key in ("successcodes", "errorcodes", "cmdbufferlevel", "renderpass", "queues"):
            if command.get(key):
                print(f"  {key}: {command.get(key)}")
        print_introduced(root, name)
        return True
    return False


def show_extension(root, name):
    for extension in root.iter("extension"):
        if extension.get("name") != name:
            continue
        print(f"extension {name} ({extension.get('type', '?')}, supported: {extension.get('supported')})")
        for key in ("depends", "promotedto", "deprecatedby", "obsoletedby", "platform"):
            if extension.get(key):
                print(f"  {key}: {extension.get(key)}")
        for require in extension.iter("require"):
            for item in require:
                if item.tag in ("type", "command") or (item.tag == "enum" and item.get("extends")):
                    print(f"  adds {item.tag} {item.get('name')}")
        return True
    return False


def show_vuid(vuids, vuid):
    hits = [
        (owner, entry)
        for owner, groups in vuids.items()
        for entries in groups.values()
        for entry in entries
        if vuid in entry["vuid"]
    ]
    for owner, entry in hits[:MAX_VUIDS]:
        print(f"{entry['vuid']}  ({owner})\n  {clean(entry['text'])}\n")
    if len(hits) > MAX_VUIDS:
        print(f"... {len(hits) - MAX_VUIDS} more, narrow the query")
    return bool(hits)


def list_vuids(vuids, owner, needle):
    groups = vuids.get(owner)
    if groups is None:
        die(f"No valid-usage section named {owner}")
    shown = 0
    total = 0
    for condition, entries in groups.items():
        for entry in entries:
            total += 1
            text = clean(entry["text"])
            if needle and needle.lower() not in (entry["vuid"] + text).lower():
                continue
            if shown < MAX_VUIDS:
                suffix = "" if condition == "core" else f"  [only with {condition}]"
                print(f"{entry['vuid']}{suffix}\n  {text}\n")
            shown += 1
    print(f"{shown} of {total} VUIDs matched" + (f", first {MAX_VUIDS} shown: add a filter" if shown > MAX_VUIDS else ""))


def search(root, needle):
    names = set()
    for node in root.iter("type"):
        names.add(node.get("name") or (node.find("name").text if node.find("name") is not None else None))
    for command in root.iter("command"):
        proto = command.find("proto")
        names.add(command.get("name") or (proto.find("name").text if proto is not None else None))
    for extension in root.iter("extension"):
        names.add(extension.get("name"))
    hits = sorted(n for n in names if n and needle.lower() in n.lower())
    print("\n".join(hits[:80]) or "no match")
    if len(hits) > 80:
        print(f"... {len(hits) - 80} more")


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return
    if argv[0] == "--vuids":
        if len(argv) < 2:
            die("usage: vk-lookup.py --vuids <command-or-struct> [filter]")
        return list_vuids(load_vuids(), argv[1], argv[2] if len(argv) > 2 else "")
    if argv[0] == "--search":
        if len(argv) < 2:
            die("usage: vk-lookup.py --search <substring>")
        return search(load_xml(), argv[1])
    name = argv[0]
    if name.startswith("VUID-"):
        if not show_vuid(load_vuids(), name):
            die(f"{name} not found in validusage.json (the registry may be older than the validation layer)")
        return
    root = load_xml()
    found = (
        show_command(root, name)
        or show_extension(root, name)
        or show_type(root, name)
        or show_enum(root, name)
        or show_enum_value(root, name)
    )
    if not found:
        print(f"{name} does not exist in this registry. Close names:")
        search(root, re.sub(r"^(Vk|vk|VK_)", "", name)[:12])
        sys.exit(1)
    count = sum(len(entries) for entries in load_vuids().get(name, {}).values())
    if count:
        print(f"{count} VUIDs: vk-lookup.py --vuids {name} [filter]")


if __name__ == "__main__":
    main(sys.argv[1:])
