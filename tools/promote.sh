#!/usr/bin/env bash
# Promote a firmware commit to the stable channel of the site.
#
#   tools/promote.sh [<firmware commit, branch or tag>]   default: nightly
#
# Moves the firmware's main branch to that commit, then this repo's
# `stable` branch to the "Firmware <sha>" commit on main that pinned it (the
# one the Site workflow built and smoke-tested), and starts the workflow so
# the root of the site is rebuilt now rather than at the hour. Both moves
# must be fast-forwards. Needs push access to both repos, and gh.
set -euo pipefail
cd "$(dirname "$0")/.."
ref=${1:-nightly}
git fetch -q origin
git -C firmware fetch -q origin
fw=$(git -C firmware rev-parse -q --verify "origin/$ref^{commit}" 2>/dev/null ||
     git -C firmware rev-parse --verify "$ref^{commit}")

bump=
for c in $(git log --reverse --format=%H origin/main -- firmware); do
    [ "$(git rev-parse "$c:firmware")" = "$fw" ] && { bump=$c; break; }
done
[ -n "$bump" ] || { echo "no commit on main pins firmware ${fw:0:8}: the Site workflow has not built it" >&2; exit 1; }

ff() {  # ff <repo dir> <branch> <new>: the branch's current tip must be an ancestor of <new>
    local cur; cur=$(git -C "$1" rev-parse -q --verify "origin/$2" 2>/dev/null || true)
    [ -z "$cur" ] || git -C "$1" merge-base --is-ancestor "$cur" "$3" ||
        { echo "$1: $2 ${cur:0:8} is not an ancestor of ${3:0:8}; refusing" >&2; exit 1; }
}
ff firmware main "$fw"
ff . stable "$bump"
echo "firmware main -> ${fw:0:8} $(git -C firmware log -1 --format=%s "$fw")"
echo "stable        -> ${bump:0:8} $(git log -1 --format=%s "$bump")"
git -C firmware push -q origin "$fw:refs/heads/main"
git push -q origin "$bump:refs/heads/stable"
[ "$(git branch --show-current)" = stable ] || git branch -f stable "$bump"
gh workflow run site.yml && echo "Site workflow started"
