# tools/cli

`./mln`, the dev CLI of the `cumbly` branch: build and publish the Android AAR, build and run the GLFW app on macOS.
Same shape as Cumbly's `./cumbly` (`tools/cli` there), scaled down.

## Running it

- `./mln` at the repo root is a committed symlink to `tools/cli/mln`, a bash launcher that runs
  `uv run --project tools/cli mln`. No install step: `uv` (`brew install uv`) builds `tools/cli/.venv` from `uv.lock`
  on first use. Every checkout runs its own code.
- Bare `./mln` in a terminal opens the dashboard (`tui/`); `MLN_NO_TUI=1`, or no TTY, prints the help.
- `uv run --project tools/cli pytest`, `uv run --project tools/cli ruff check`,
  `uv run --project tools/cli ruff format --check`.

```bash
./mln android build [--flavor vulkan|opengl] [--abis arm64-v8a] [--release]   # prints the AAR path
./mln android publish local|artifactory [--flavor …] [--abis …] [-y]           # prints the version to pin
./mln android version                                                         # the version a local publish gets
./mln macos build [--backend vulkan|metal]                                    # prints the mbgl-glfw path
./mln macos run [--backend …] [--driver kosmickrisp|moltenvk] [--style URL] [--no-build] [-- app args]
./mln macos stop [--backend …]                                                # the app the dashboard started
./mln doctor [--fix]                                                          # scripts/vulkan-doctor.sh
```

Dashboard keys: `b` build, `r` run (macOS, detached, log followed), `s` stop, `l` logs, `p` publish to Maven Local,
`P` publish to Artifactory (asks first), `D` doctor, `q` quit. One row per Android flavor, per Vulkan driver, and Metal.

## Boundaries

- It orchestrates processes only. `platform/android/scripts/publish-cumbly.sh` stays the source of truth for the
  published version, the credentials and the Artifactory checks; the CLI runs it and reads the `maplibre = "…"` line
  it prints, and gets the next version from its `--print-version`. CI never runs this CLI.
- Any change that moves how something builds or runs (a renamed Gradle task, a new flavor, a CMake preset, a new Vulkan
  driver) updates this directory in the same change.

## Layout and rules

Same as Cumbly's CLI:

- `<package>/core.py` pure (no subprocess, file write, env read); `system.py` the I/O; `commands.py` Typer only.
  Tests cover `core` and `tui/model.py`.
- Errors are `MlnError(message)`, written for the user; commands run through `output.guarded`, which prints
  `mln: <line>` on stderr and exits 1.
- Progress goes to stderr through `on_line`; data an agent parses (a path, a version) goes to stdout, bare.
- An outward step (Artifactory) asks first: `--yes` in the CLI, a Confirm modal in the dashboard; no terminal refuses.
- Vulkan on macOS: `macos.core.run_env` gives the app the LunarG SDK's loader (`$VULKAN_SDK`, else the newest
  `~/VulkanSDK/<version>/macOS`) with exactly one driver in `VK_DRIVER_FILES`, KosmicKrisp by default. Detached runs
  keep their pid, label and log under `tools/cli/.run/`.
