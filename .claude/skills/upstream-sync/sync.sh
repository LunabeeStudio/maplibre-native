#!/usr/bin/env bash
# upstream-sync skill: keep LunabeeStudio/maplibre-native in step with upstream maplibre/maplibre-native.
#
#   sync.sh                     fast-forward origin main and feature/terrain-3d to upstream, then merge
#                               feature/terrain-3d into cumbly on a sync/terrain-3d-<sha> branch and open a PR
#   sync.sh --finish [--notes FILE]
#                               after resolving merge conflicts and committing the merge on the sync branch:
#                               push it and open the PR, appending FILE (markdown, e.g. a conflict report) to its body
#
# Exit codes: 0 done or nothing to do, 1 error, 2 merge stopped on conflicts (resolve, commit, then --finish).
set -euo pipefail

# Bash reads a script while it runs, and this one switches branches in the repo that holds it.
# Run from a temp copy so a checkout never rewrites the file under it.
if [ -z "${MLN_SYNC_COPY:-}" ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  MLN_SYNC_COPY=$copy exec bash "$copy" "$@"
fi
trap 'rm -f "$MLN_SYNC_COPY"' EXIT

REPO=${MLN_REPO:-$HOME/dev/maplibre-native}
FORK=LunabeeStudio/maplibre-native
FF_BRANCHES=(main feature/terrain-3d)
BASE=feature/terrain-3d
TARGET=cumbly

die() { echo "sync.sh: $*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }

cd "$REPO"

require_clean() {
  git diff --quiet && git diff --cached --quiet || die "tracked changes in $REPO; commit or stash them first"
}

# Commits of $1..$2 as a fenced list (fenced so "#1234" does not link to the fork's issues).
commit_list() {
  echo '```'
  git log --first-parent --format='%h %s' "$1..$2"
  echo '```'
}

open_pr() { # branch, notes file (optional)
  local br=$1 notes=${2:-} sha body
  sha=$(git rev-parse --short=12 "origin/$BASE")
  body=$(mktemp)
  {
    echo "Merges upstream \`$BASE\` (\`$sha\`) into \`$TARGET\`."
    echo
    echo "Upstream commits brought in (first parent):"
    echo
    commit_list "origin/$TARGET" "origin/$BASE"
    if [ -n "$notes" ]; then echo; cat "$notes"; fi
  } >"$body"
  git push -u origin "$br"
  gh pr create --repo "$FORK" --base "$TARGET" --head "$br" \
    --title "Merge upstream $BASE ($sha) into $TARGET" --body-file "$body"
  rm -f "$body"
}

if [ "${1:-}" = "--finish" ]; then
  notes=""
  [ "${2:-}" = "--notes" ] && notes=${3:?--notes needs a file}
  br=$(git branch --show-current)
  case "$br" in sync/terrain-3d-*) ;; *) die "--finish must run on a sync/terrain-3d-* branch (on '$br')" ;; esac
  git rev-parse -q --verify MERGE_HEAD >/dev/null && die "merge still in progress; resolve and commit it first"
  git merge-base --is-ancestor "origin/$BASE" HEAD || die "HEAD does not contain origin/$BASE; merge not committed?"
  require_clean
  open_pr "$br" "$notes"
  exit 0
fi

[ $# -eq 0 ] || die "unknown arguments: $*"
require_clean
git rev-parse -q --verify MERGE_HEAD >/dev/null && die "a merge is in progress"
[ -d "$(git rev-parse --git-path rebase-merge)" ] || [ -d "$(git rev-parse --git-path rebase-apply)" ] \
  && die "a rebase is in progress"

say "fetch"
git fetch --prune upstream
git fetch --prune origin

current=$(git branch --show-current)

for b in "${FF_BRANCHES[@]}"; do
  ahead=$(git rev-list --count "upstream/$b..origin/$b")
  behind=$(git rev-list --count "origin/$b..upstream/$b")
  say "$b: origin is $behind behind, $ahead ahead of upstream"
  [ "$ahead" -eq 0 ] || die "origin/$b has $ahead commits not in upstream/$b; refusing to fast-forward"
  if [ "$behind" -gt 0 ]; then
    git log --first-parent --format='  %h %s' "origin/$b..upstream/$b"
    git push origin "refs/remotes/upstream/$b:refs/heads/$b"
  fi
  # Fast-forward the local branch too, when it exists and has nothing of its own.
  if git show-ref -q --verify "refs/heads/$b"; then
    if [ "$current" = "$b" ]; then
      git merge --ff-only "upstream/$b"
    elif git merge-base --is-ancestor "$b" "upstream/$b"; then
      # update-ref, not 'branch -f': that would re-point the branch's tracking to upstream.
      git update-ref "refs/heads/$b" "upstream/$b" "$(git rev-parse "$b")"
    else
      echo "  warning: local $b has commits not in upstream; left as is"
    fi
  fi
done

if git show-ref -q --verify "refs/heads/$TARGET" \
  && [ "$(git rev-list --count "origin/$TARGET..$TARGET")" -gt 0 ]; then
  echo "  warning: local $TARGET has unpushed commits; the sync branch starts from origin/$TARGET without them"
fi

new=$(git rev-list --count "origin/$TARGET..origin/$BASE")
say "$TARGET: $new commits of $BASE not merged yet"
[ "$new" -gt 0 ] || exit 0

sha=$(git rev-parse --short=12 "origin/$BASE")
br="sync/terrain-3d-$sha"
if git ls-remote -q --exit-code --heads origin "$br" >/dev/null; then
  echo "  $br already pushed:"
  gh pr list --repo "$FORK" --head "$br" --state all --json url,state --jq '.[] | "  \(.state) \(.url)"'
  exit 0
fi
git show-ref -q --verify "refs/heads/$br" && die "local branch $br exists; delete it or finish it with --finish"

git log --first-parent --format='  %h %s' "origin/$TARGET..origin/$BASE"
git switch --no-track -c "$br" "origin/$TARGET"

if git merge --no-ff --no-edit -m "Merge upstream $BASE ($sha) into $TARGET" "origin/$BASE"; then
  open_pr "$br"
else
  say "merge stopped on conflicts (on $br):"
  git diff --name-only --diff-filter=U | sed 's/^/  /'
  echo
  echo "Resolve, 'git commit --no-edit', then: .claude/skills/upstream-sync/sync.sh --finish [--notes conflicts.md]"
  exit 2
fi
