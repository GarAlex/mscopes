#!/usr/bin/env bash
#
# Publish a snapshot of the current commit to the public GitHub repository —
# without the development history.
#
# The public repo has its own linear history: one commit per publish, whose
# tree is HEAD's tree minus tools/publish-exclude.txt. That history lives on
# the local branch `public` (created as an orphan the first time) and is
# pushed to the GitHub remote's `main`. The full history stays on `main` /
# the NAS remote and is never pushed to GitHub.
#
#   tools/publish.sh                 # snapshot HEAD → branch public → push github main
#   tools/publish.sh --dry-run       # build the snapshot, list it, push nothing
#   tools/publish.sh -m "message"    # custom commit message for the snapshot
#
#   REMOTE=github BRANCH=main        # environment overrides
#
# Setup, once:  git remote add github git@github.com:<user>/mscopes.git
#
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
REMOTE="${REMOTE:-github}"
BRANCH="${BRANCH:-main}"
LOCAL_BRANCH="${LOCAL_BRANCH:-public}"
EXCLUDE="$HERE/publish-exclude.txt"

dry=0; msg=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) dry=1 ;;
        -m) msg="$2"; shift ;;
        *) echo "usage: tools/publish.sh [--dry-run] [-m message]" >&2; exit 2 ;;
    esac
    shift
done

cd "$REPO"
head="$(git rev-parse HEAD)"
short="$(git rev-parse --short HEAD)"
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "note: working tree has uncommitted changes; the snapshot is taken from HEAD ($short), not the working tree." >&2
fi
if [[ $dry -eq 0 ]] && ! git remote get-url "$REMOTE" > /dev/null 2>&1; then
    echo "error: no remote '$REMOTE'. Add it:  git remote add $REMOTE git@github.com:<user>/mscopes.git" >&2
    exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/mscopes-publish.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
export_dir="$tmp/tree"; mkdir -p "$export_dir"

# 1. Export HEAD's tree (tracked files only — nothing untracked or ignored can leak).
git archive --format=tar HEAD | tar -x -C "$export_dir"

# 2. Drop the local-only paths.
while IFS= read -r line; do
    line="${line%%#*}"; line="${line#"${line%%[![:space:]]*}"}"; line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    if [[ -e "$export_dir/$line" ]]; then
        rm -rf "${export_dir:?}/$line"
        echo "excluded: $line"
    else
        echo "warning: exclude entry not in tree: $line" >&2
    fi
done < "$EXCLUDE"

# 3. Sanity checks: no dev log / local presets, no big files.
if ls -d "$export_dir"/presets/classic "$export_dir"/presets/extracted > /dev/null 2>&1; then
    echo "error: local-only preset folders ended up in the snapshot" >&2; exit 1
fi
big="$(find "$export_dir" -type f -size +10M | sed "s|$export_dir/||")"
if [[ -n "$big" ]]; then
    echo "error: files over 10 MB in the snapshot:" >&2; echo "$big" >&2; exit 1
fi

count="$(find "$export_dir" -type f | wc -l | tr -d ' ')"
size="$(du -sh "$export_dir" | cut -f1)"
echo "snapshot of $short: $count files, $size"

if [[ $dry -eq 1 ]]; then
    ( cd "$export_dir" && find . -type f | sed 's|^\./||' | sort )
    exit 0
fi

# 4. Commit that tree onto the local public branch (temporary index; the
#    working tree and main are untouched).
export GIT_INDEX_FILE="$tmp/index"
git read-tree --empty
git --work-tree="$export_dir" add -A
tree="$(git write-tree)"
unset GIT_INDEX_FILE
parent="$(git rev-parse -q --verify "refs/heads/$LOCAL_BRANCH" || true)"
if [[ -n "$parent" && "$(git rev-parse "$parent^{tree}")" == "$tree" ]]; then
    echo "nothing changed since the last snapshot on $LOCAL_BRANCH; not committing."
    commit="$parent"
else
    [[ -z "$msg" ]] && msg="Snapshot $(date +%Y-%m-%d) ($short)"
    if [[ -n "$parent" ]]; then
        commit="$(git commit-tree "$tree" -p "$parent" -m "$msg")"
    else
        commit="$(git commit-tree "$tree" -m "$msg")"
    fi
    git update-ref "refs/heads/$LOCAL_BRANCH" "$commit"
    echo "committed $(git rev-parse --short "$commit") on $LOCAL_BRANCH: $msg"
fi

# 5. Push the public branch as the remote's main. Fast-forward only: the public
#    history is append-only once it exists.
git push "$REMOTE" "refs/heads/$LOCAL_BRANCH:refs/heads/$BRANCH"
echo "published $(git rev-parse --short "$commit") → $REMOTE/$BRANCH"
