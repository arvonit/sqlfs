#!/usr/bin/env bash
# Smoke test for sqlfs — mount, exercise all core operations, unmount.
set -euo pipefail

if [[ "$(uname)" == "Darwin" ]]; then
    MOUNT=/Volumes/sqlfs_test
else
    MOUNT=/tmp/sqlfs_test_mount
fi
DB=/tmp/sqlfs_test.db
BIN="${1:-./build/sqlfs}"

if [[ ! -x "$BIN" ]]; then
    echo "error: binary not found at $BIN" >&2
    echo "Build first: cmake -B build && cmake --build build" >&2
    exit 1
fi

cleanup() {
    if mountpoint -q "$MOUNT" 2>/dev/null || mount | grep -q "$MOUNT"; then
        if [[ "$(uname)" == "Darwin" ]]; then
            diskutil unmount force "$MOUNT" 2>/dev/null || true
        else
            fusermount3 -u "$MOUNT" 2>/dev/null || true
        fi
    fi
    if [[ "$(uname)" != "Darwin" ]]; then
        rm -rf "$MOUNT"
    fi
    rm -f "$DB"
}
trap cleanup EXIT

if [[ "$(uname)" != "Darwin" ]]; then
    mkdir -p "$MOUNT"
fi
rm -f "$DB"

echo "==> Mounting sqlfs..."
"$BIN" -o db="$DB" "$MOUNT" &
FUSE_PID=$!
sleep 1

if ! mountpoint -q "$MOUNT" 2>/dev/null && ! mount | grep -q "$MOUNT"; then
    echo "FAIL: filesystem did not mount" >&2
    exit 1
fi

# ── Tests ────────────────────────────────────────────────────────────────

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "  ok: $*"; }

echo "==> Testing basic file write/read..."
echo "hello sqlfs" > "$MOUNT/hello.txt"
[[ "$(cat "$MOUNT/hello.txt")" == "hello sqlfs" ]] || fail "read after write"
pass "write and read file"

echo "==> Testing mkdir and nested file..."
mkdir "$MOUNT/subdir"
[[ -d "$MOUNT/subdir" ]] || fail "mkdir"
echo "nested content" > "$MOUNT/subdir/nested.txt"
[[ "$(cat "$MOUNT/subdir/nested.txt")" == "nested content" ]] || fail "nested file read"
pass "mkdir and nested file"

echo "==> Testing symlink..."
ln -s hello.txt "$MOUNT/link.txt"
[[ "$(cat "$MOUNT/link.txt")" == "hello sqlfs" ]] || fail "symlink read"
[[ "$(readlink "$MOUNT/link.txt")" == "hello.txt" ]] || fail "readlink"
pass "symlink"

echo "==> Testing rename..."
mv "$MOUNT/hello.txt" "$MOUNT/renamed.txt"
[[ -f "$MOUNT/renamed.txt" ]] || fail "renamed file missing"
[[ ! -f "$MOUNT/hello.txt" ]] || fail "original still exists after rename"
pass "rename"

echo "==> Testing chmod..."
chmod 600 "$MOUNT/renamed.txt"
PERMS=$(stat -c "%a" "$MOUNT/renamed.txt" 2>/dev/null \
     || stat -f "%Lp" "$MOUNT/renamed.txt")
[[ "$PERMS" == "600" ]] || fail "chmod: expected 600 got $PERMS"
pass "chmod"

echo "==> Testing truncate..."
echo "this will be truncated" > "$MOUNT/trunc.txt"
truncate -s 4 "$MOUNT/trunc.txt"
[[ "$(wc -c < "$MOUNT/trunc.txt" | tr -d ' ')" == "4" ]] || fail "truncate size"
pass "truncate"

echo "==> Testing large file (multi-block)..."
dd if=/dev/urandom bs=4096 count=16 2>/dev/null > /tmp/sqlfs_random.bin
cp /tmp/sqlfs_random.bin "$MOUNT/large.bin"
cmp /tmp/sqlfs_random.bin "$MOUNT/large.bin" || fail "large file mismatch"
rm /tmp/sqlfs_random.bin
pass "large file (64 KiB, 16 blocks)"

echo "==> Testing unlink..."
rm "$MOUNT/renamed.txt"
[[ ! -f "$MOUNT/renamed.txt" ]] || fail "unlink"
rm "$MOUNT/link.txt"
rm "$MOUNT/trunc.txt"
rm "$MOUNT/large.bin"
pass "unlink"

echo "==> Testing rmdir..."
rm "$MOUNT/subdir/nested.txt"
rmdir "$MOUNT/subdir"
[[ ! -d "$MOUNT/subdir" ]] || fail "rmdir"
pass "rmdir"

echo "==> Testing statfs..."
df -h "$MOUNT" | grep -q "/" || fail "statfs"
pass "statfs"

# ── Unmount ──────────────────────────────────────────────────────────────

echo "==> Unmounting..."
if [[ "$(uname)" == "Darwin" ]]; then
    diskutil unmount "$MOUNT"
else
    fusermount3 -u "$MOUNT"
fi

echo ""
echo "All smoke tests passed."
