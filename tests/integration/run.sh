#!/usr/bin/env bash
# Mounts sqlfs through FSKit and checks real commands against it.
# Usage: bash tests/integration/run.sh   (after building into build/)
set -euo pipefail

cd "$(dirname "$0")/../.."
SQLFS=build/sqlfs
MOUNT=/Volumes/sqlfs-test
WORK=tmp/integration
DB=$WORK/test.db
LOG=$WORK/sqlfs.log

passed=0
failed=0

check() {
    local description=$1
    shift
    if "$@"; then
        echo "  ok    $description"
        passed=$((passed + 1))
    else
        echo "  FAIL  $description"
        failed=$((failed + 1))
    fi
}

mount_sqlfs() {
    "$SQLFS" "$DB" "$MOUNT" >>"$LOG" 2>&1 &
    for _ in $(seq 1 20); do
        mount | grep -q " on $MOUNT " && return 0
        sleep 0.5
    done
    echo "sqlfs did not mount. Log:"
    cat "$LOG"
    exit 1
}

unmount_sqlfs() {
    umount "$MOUNT"
    for _ in $(seq 1 20); do
        mount | grep -q " on $MOUNT " || return 0
        sleep 0.5
    done
    echo "sqlfs did not unmount"
    exit 1
}

rm -rf "$WORK"
mkdir -p "$WORK"
mount | grep -q " on $MOUNT " && umount "$MOUNT"

echo "Mounting $DB at $MOUNT"
mount_sqlfs

echo "Folders"
mkdir -p "$MOUNT/a/b/c"
check "mkdir -p creates nested folders" test -d "$MOUNT/a/b/c"
check "ls lists a new folder" bash -c "ls '$MOUNT' | grep -qx a"
check "rmdir refuses a non-empty folder" bash -c "! rmdir '$MOUNT/a' 2>/dev/null"

echo "Files"
echo "hello sqlfs" >"$MOUNT/a/hello.txt"
check "write then read a small file" bash -c "[ \"\$(cat '$MOUNT/a/hello.txt')\" = 'hello sqlfs' ]"
echo "more" >>"$MOUNT/a/hello.txt"
check "append to a file" bash -c "[ \"\$(tail -1 '$MOUNT/a/hello.txt')\" = 'more' ]"
head -c 1048576 /dev/urandom >"$WORK/random.bin"
cp "$WORK/random.bin" "$MOUNT/a/random.bin"
check "copy a 1 MB file with matching checksum" \
    bash -c "[ \"\$(shasum < '$WORK/random.bin')\" = \"\$(shasum < '$MOUNT/a/random.bin')\" ]"
: >"$MOUNT/a/hello.txt"
check "truncate a file to zero" test ! -s "$MOUNT/a/hello.txt"
chmod 600 "$MOUNT/a/hello.txt"
check "chmod changes permissions" bash -c "[ \"\$(stat -f %Lp '$MOUNT/a/hello.txt')\" = 600 ]"

echo "Rename"
mv "$MOUNT/a/hello.txt" "$MOUNT/a/b/renamed.txt"
check "mv moves a file between folders" test -f "$MOUNT/a/b/renamed.txt"
echo "old" >"$MOUNT/old.txt"
echo "new" >"$MOUNT/new.txt"
mv -f "$MOUNT/new.txt" "$MOUNT/old.txt"
check "mv replaces an existing file" bash -c "[ \"\$(cat '$MOUNT/old.txt')\" = new ]"
check "mv refuses to move a folder into itself" \
    bash -c "! mv '$MOUNT/a' '$MOUNT/a/b/c/' 2>/dev/null"

echo "Links"
ln "$MOUNT/old.txt" "$MOUNT/hard.txt"
check "hard link shares contents" bash -c "[ \"\$(cat '$MOUNT/hard.txt')\" = new ]"
check "hard link raises the link count" bash -c "[ \"\$(stat -f %l '$MOUNT/old.txt')\" = 2 ]"
ln -s old.txt "$MOUNT/soft.txt"
check "symlink resolves" bash -c "[ \"\$(cat '$MOUNT/soft.txt')\" = new ]"
check "readlink returns the target" bash -c "[ \"\$(readlink '$MOUNT/soft.txt')\" = old.txt ]"

echo "Deleting an open file"
echo "still readable" >"$MOUNT/open.txt"
exec 3<"$MOUNT/open.txt"
rm "$MOUNT/open.txt"
check "deleted file is gone from the folder" test ! -e "$MOUNT/open.txt"
check "deleted file is still readable while open" bash -c "[ \"\$(cat <&3)\" = 'still readable' ]"
exec 3<&-

echo "Extended attributes"
echo "tagged" >"$MOUNT/tagged.txt"
xattr -w user.color blue "$MOUNT/tagged.txt"
check "xattr -w then -p reads it back" \
    bash -c "[ \"\$(xattr -p user.color '$MOUNT/tagged.txt')\" = blue ]"
check "xattr -l lists it" bash -c "xattr -l '$MOUNT/tagged.txt' | grep -qx 'user.color: blue'"
xattr -w user.temp gone "$MOUNT/tagged.txt"
xattr -d user.temp "$MOUNT/tagged.txt"
check "xattr -d removes it" bash -c "! xattr -p user.temp '$MOUNT/tagged.txt' 2>/dev/null"
cp -p "$MOUNT/tagged.txt" "$MOUNT/tagged-copy.txt"
check "cp -p keeps attributes" \
    bash -c "[ \"\$(xattr -p user.color '$MOUNT/tagged-copy.txt')\" = blue ]"
xattr -w user.folder yes "$MOUNT/a"
check "folders can have attributes" bash -c "[ \"\$(xattr -p user.folder '$MOUNT/a')\" = yes ]"
check "no AppleDouble ._ files appear" bash -c "! ls -a '$MOUNT' | grep -q '^\._'"

echo "Disk usage"
check "df reports a nonzero size" \
    bash -c "[ \"\$(df -k '$MOUNT' | awk 'NR == 2 { print \$2 }')\" -gt 0 ]"

echo "Persistence"
unmount_sqlfs
mount_sqlfs
check "files survive a remount" \
    bash -c "[ \"\$(shasum < '$WORK/random.bin')\" = \"\$(shasum < '$MOUNT/a/random.bin')\" ]"
check "folders survive a remount" test -d "$MOUNT/a/b/c"
check "symlinks survive a remount" bash -c "[ \"\$(readlink '$MOUNT/soft.txt')\" = old.txt ]"
check "attributes survive a remount" \
    bash -c "[ \"\$(xattr -p user.color '$MOUNT/tagged.txt')\" = blue ]"

unmount_sqlfs
echo
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
