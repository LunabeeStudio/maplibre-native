---
name: vulkan-spec-lookup
description: Looks a Vulkan struct, command, enum, extension or VUID up in the registry this repo compiles against (vk.xml, validusage.json). Use before writing or reviewing any Vulkan call whose members, required version or valid usage you would otherwise recall from memory, and to read the full text of a VUID printed by the validation layer.
---

# Vulkan spec lookup

The registry is ground truth. A name recalled from memory is a guess until this script prints it.

```bash
S=.claude/skills/vulkan-spec-lookup/scripts/vk-lookup.py   # run from the repo root
$S VkAttachmentDescription        # struct: members, optional/len, where introduced
$S vkCmdBindDescriptorSets        # command: parameters, render pass scope, where introduced
$S VK_KHR_swapchain               # extension: dependencies, what it adds
$S VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
$S VUID-vkCmdDraw-renderPass-02684
$S --vuids vkCmdDraw descriptor   # every VUID of a command or struct, filtered by text
$S --search Barrier               # names containing a substring
```

Missing registry: `git submodule update --init --depth 1 vendor/Vulkan-Headers`.

## Reading the result

- **`NOT in VK_VERSION_1_0`**: the backend requests Vulkan 1.0 with only `VK_KHR_swapchain`
  (`src/mln/vulkan/renderer_backend.cpp`, `vk::ApplicationInfo`). Such a name needs an extension enabled at device
  creation plus a runtime support check, or a 1.0 equivalent. Pick the 1.0 equivalent unless the task is to raise
  the baseline.
- **"does not exist in this registry"**: the name is invented or misspelled. Use `--search`.
- **A VUID that is not found**: the validation layer on the device is newer than the vendored headers. Search by
  the command name with `--vuids` instead, the statement text rarely changes.
- The code uses vulkan-hpp: `VkFooBar` is `vk::FooBar`, `VK_FOO_BAR_BIT` is `vk::FooBarFlagBits::eBar`, and
  `vkCmdDraw(cmd, ...)` is `cmd->draw(..., dispatcher)`. Look the C name up, write the hpp form.

## Optional: mcp-Vulkan server

`mcp-Vulkan` is the registry server from the Khronos "AI Assisted Vulkan" guide. When a server named `vulkan-spec`
is connected, its `search-vulkan-docs` and `get-vulkan-topic` tools add prose explanations that the XML lacks. It
is third-party code (`github.com/gpx1000/mcp-Vulkan`), not reviewed by this project: read it before building it.

It is opt-in per person and not declared in the repo, because a declared server that is not installed fails to
connect in every session. To add it for yourself, in this project only:

```bash
git clone https://github.com/gpx1000/mcp-Vulkan.git ~/mcp-Vulkan
(cd ~/mcp-Vulkan/vulkan && npm install && npm run build)
claude mcp add vulkan-spec -- node ~/mcp-Vulkan/vulkan/build/index.js    # run from this repo's root
```

For versions and valid usage, the script wins over the server: it reads the exact headers this repo builds with.
