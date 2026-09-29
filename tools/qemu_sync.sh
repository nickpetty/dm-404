#!/bin/sh
# Brings third_party/qemu to the pinned release with our patches and the
# files under core/qemu/ laid over it. Safe to run repeatedly.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tag=v11.1.2
q=$root/third_party/qemu
if [ ! -d "$q/.git" ]; then
    git clone -q --depth 1 --branch $tag https://gitlab.com/qemu-project/qemu.git "$q"
fi
for p in "$root"/core/qemu/patches/*.patch; do
    if git -C "$q" apply --reverse --check "$p" 2>/dev/null; then
        continue        # already applied
    fi
    git -C "$q" apply "$p"
    echo "applied $(basename "$p")"
done
# Our own files: copy only what changed so ninja rebuilds only that.
(cd "$root/core/qemu" && find hw include -type f) | while read -r f; do
    if ! cmp -s "$root/core/qemu/$f" "$q/$f"; then
        mkdir -p "$(dirname "$q/$f")"
        cp "$root/core/qemu/$f" "$q/$f"
        echo "synced $f"
    fi
done
