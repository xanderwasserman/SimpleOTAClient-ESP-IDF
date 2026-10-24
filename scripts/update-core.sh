#!/usr/bin/env bash
# Copy the shared core into simpleota/src/core, and its public header into
# simpleota/include, at a given ref of the core repo.
#
#   scripts/update-core.sh [ref]    ref: a tag, branch or commit (default: main)
#
# Commit the result together with simpleota/src/core/CORE_VERSION, which CI checks
# the copy against. SOTA_CORE_REPO overrides the repo, e.g. a local clone.

set -euo pipefail

ref=${1:-main}
repo=${SOTA_CORE_REPO:-https://github.com/wassie-dev/simpleOTAClient-core.git}
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

git clone -q "$repo" "$tmp/core"
git -C "$tmp/core" checkout -q --detach "$ref"
"$tmp/core/scripts/sync-core.sh" "$root/simpleota/src/core" "$root/simpleota/include"
