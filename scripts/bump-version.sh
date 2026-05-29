#!/usr/bin/env bash
# Bump the STASHA_VERSION macro in src/main.c, commit the change, and tag
# the commit so the release workflow has a matching git tag to ship from.
#
# Usage:
#   scripts/bump-version.sh <new-version>           # e.g. 0.1.10_000
#   scripts/bump-version.sh <new-version> --no-tag  # bump+commit only
#
# The new version must be a string the release workflow accepts: the tag
# created is "v<new-version>".

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <new-version> [--no-tag]" >&2
    exit 2
fi

new_version="$1"
tag_after_commit=1
if [[ "${2:-}" == "--no-tag" ]]; then
    tag_after_commit=0
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
main_c="$repo_root/src/main.c"

if [[ ! -f "$main_c" ]]; then
    echo "error: $main_c not found" >&2
    exit 1
fi

current=$(grep -E '^#define STASHA_VERSION ' "$main_c" | head -n1 | sed -E 's/.*"([^"]+)".*/\1/')
if [[ -z "$current" ]]; then
    echo "error: could not find STASHA_VERSION in $main_c" >&2
    exit 1
fi

if [[ "$current" == "$new_version" ]]; then
    echo "STASHA_VERSION already at $current — nothing to do"
    exit 0
fi

if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    echo "error: working tree has uncommitted changes — refusing to bump" >&2
    git -C "$repo_root" status --short >&2
    exit 1
fi

echo "bumping STASHA_VERSION: $current -> $new_version"

# Portable in-place edit (works on macOS BSD sed + GNU sed).
tmpfile=$(mktemp)
sed -E "s/^(#define STASHA_VERSION )\"[^\"]+\"/\1\"$new_version\"/" "$main_c" > "$tmpfile"
mv "$tmpfile" "$main_c"

if ! grep -qE "^#define STASHA_VERSION \"$new_version\"" "$main_c"; then
    echo "error: post-edit sanity check failed" >&2
    exit 1
fi

git -C "$repo_root" add src/main.c
git -C "$repo_root" commit -m "chore: bump STASHA_VERSION to $new_version"

if [[ $tag_after_commit -eq 1 ]]; then
    tag="v$new_version"
    git -C "$repo_root" tag -a "$tag" -m "Release $tag"
    echo
    echo "Bumped to $new_version and created tag $tag."
    echo "Push with: git push origin main $tag"
else
    echo
    echo "Bumped to $new_version (no tag created)."
fi
