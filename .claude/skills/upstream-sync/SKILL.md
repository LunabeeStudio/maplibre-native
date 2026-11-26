---
name: upstream-sync
description: Sync the LunabeeStudio fork with upstream maplibre/maplibre-native, then merge upstream feature/terrain-3d into cumbly through a PR, resolving merge conflicts and reporting them in the PR. Use when asked to check upstream news, sync or update the fork, pull upstream, bring main or feature/terrain-3d up to date, merge or rebase terrain into cumbly, or run the upstream-sync script.
---

# Upstream sync for the cumbly fork

Remotes: `origin` = `LunabeeStudio/maplibre-native` (the fork), `upstream` = `maplibre/maplibre-native`.

| Branch | Policy |
| --- | --- |
| `main`, `feature/terrain-3d` | Mirror upstream. The script fast-forwards origin without asking and stops if origin has commits of its own. |
| `cumbly` | Lunabee's branch. It takes upstream `feature/terrain-3d` by **merge** (never rebase, never force-push), on a `sync/terrain-3d-<sha>` branch, through a PR into `cumbly`. |
| `terrain-3d-color-relief` | Ignore. Stale, already squash-merged into `feature/terrain-3d` upstream (#4389). |

The script is `sync.sh` next to this file. It re-runs itself from a temp copy, because it switches branches in the
repo that holds it. This skill exists only on `cumbly` and branches made from it.

## Steps

1. From the repo root, with no tracked changes, run `.claude/skills/upstream-sync/sync.sh`. Print what it reports as
   upstream news: the commits brought into each branch.
2. Act on its exit code:
   - **0**: done. Either nothing new, a branch already pushed (it prints the PR), or a clean merge with its PR opened.
     Go to step 5.
   - **1**: an error, e.g. a dirty tree or origin ahead of upstream. Report it. Do not work around it.
   - **2**: the merge stopped on conflicts, on the `sync/terrain-3d-<sha>` branch. Go to step 3.
3. Resolve each conflicted file (`git diff --name-only --diff-filter=U`):
   - Find each side's intent. `mb=$(git merge-base origin/cumbly origin/feature/terrain-3d)`, then
     `git log --oneline $mb..origin/cumbly -- <file>` and `git log --oneline $mb..origin/feature/terrain-3d -- <file>`.
   - Default: take upstream's version and reapply the Cumbly change on top. Upstream code moves on, and Cumbly
     changes are small and deliberate (see `CLAUDE.md`).
   - Check: `git diff origin/feature/terrain-3d -- <file>` must show only Cumbly's own changes. Check also that no
     conflict marker is left.
   - When a side's intent is unclear, or both sides changed behaviour in ways that do not combine, stop and ask the
     user. Do not guess.
   - Then `git add` the files and run `git commit --no-edit`.
4. Write the conflict report to a file in the scratchpad and run `.claude/skills/upstream-sync/sync.sh --finish --notes <file>`.
   It pushes the branch and opens the PR, with the upstream commit list followed by the notes. The notes use this shape:

   ```markdown
   ## Conflicts

   <N> file(s). <Other files changed on both sides, auto-merged: list, or "No other file was changed on both sides.">

   ### `<path>`
   - Upstream (`<sha>`, #<pr>): <what it changed>.
   - `cumbly` (`<sha>`): <what it changed>.
   - Resolution: <what was kept>. Compared with `feature/terrain-3d`, the file now differs only by <the Cumbly changes>.

   ## Not verified

   - <builds and tests not run after the merge>

   🤖 Generated with [Claude Code](https://claude.com/claude-code)
   ```

   To list the files changed on both sides: `comm -12 <(git diff --name-only $mb origin/cumbly | sort) <(git diff --name-only $mb origin/feature/terrain-3d | sort)`.
5. Report to the user:
   - the number of commits brought into each branch;
   - the conflicts and how each was resolved;
   - the PR link;
   - what was not built. Name the AAR and the XCFramework.

   The repo is left on the sync branch. Say so.

## Rules

- Do not merge the PR. The user reviews it.
- Do not push to `cumbly` directly, and do not force-push any branch.
- If any merged upstream commit touches a Cumbly area (Vulkan custom layers, TrackLayer / SkyLayer, terrain depth range,
  publishing), list it under "Not verified" even when git merged it cleanly.
