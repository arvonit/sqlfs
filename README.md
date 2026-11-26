# sqlfs

A user-mode filesystem that stores every file, folder, and symlink in a single SQLite database. It
mounts on macOS through [macFUSE](https://macfuse.github.io)'s **FSKit backend**, so it runs
entirely in user space with no kernel extension.

```console
$ sqlfs notes.db /Volumes/sqlfs &
$ echo "hello" > /Volumes/sqlfs/hello.txt
$ sqlite3 notes.db "SELECT name, file_id FROM folder_contents"
hello.txt|2
```

Written in C++26 against libfuse's **low-level (inode-based) API**.

## Building and running

Requirements: macOS 15.4 or later, [macFUSE](https://macfuse.github.io) 5.1 or later with its file
system extension turned on (**System Settings → General → Login Items & Extensions → File System
Extensions → macFUSE**), CMake 3.25 or later, and Xcode's command line tools. SQLite comes with
macOS, and the build downloads Catch2.

```fish
cmake -S . -B build
cmake --build build
./build/sqlfs my.db /Volumes/sqlfs          # add --debug to log every FUSE request
umount /Volumes/sqlfs                        # or press Ctrl+C
```

Tests:

```fish
ctest --test-dir build                       # unit tests, no mounting needed
bash tests/integration/run.sh                # mounts through FSKit and runs real commands
```

Debug builds (the default) run with AddressSanitizer and UndefinedBehaviorSanitizer.

## Architecture

```
  ls, cp, Finder ...
        │  system calls
  macOS (FSKit)  ──►  macFUSE  ──►  libfuse low-level API
                                         │  C callbacks with inode numbers
                     ┌───────────────────▼───────────────────┐
  src/fuse/          │ adapter: C callbacks ↔ C++ calls       │
                     ├────────────────────────────────────────┤
  src/core/          │ Filesystem: all filesystem rules       │  ← unit tested here
                     ├────────────────────────────────────────┤
  src/db/            │ Database / Statement / Transaction     │
                     └───────────────────┬────────────────────┘
                                         ▼
                                    my.db (SQLite)
```

- **`src/db`**: thin RAII wrappers over the SQLite C API. A `Transaction` rolls back in its
  destructor unless `commit()` ran, so returning early on any error undoes partial changes.
- **`src/core`**: the `Filesystem` class. It knows nothing about FUSE, so the unit tests call
  it directly. Errors are `std::expected<T, int>` values holding a POSIX errno code.
- **`src/fuse`**: converts between libfuse's C callbacks and `Filesystem`, and makes sure no C++
  exception ever unwinds into libfuse's C code.

## Schema

```sql
-- Every file, folder, or symlink. file_id 1 is the root folder
CREATE TABLE files (
  file_id INTEGER PRIMARY KEY,
  kind TEXT CHECK (kind IN ('file', 'folder', 'symlink')),
  permissions, owner_uid, group_gid,
  name_count,          -- how many names point here (POSIX nlink)
  size_bytes, contents BLOB, symlink_target,
  created_ns, modified_ns, changed_ns, accessed_ns
) STRICT;

-- "Folder X contains the name Y, which is file Z"
CREATE TABLE folder_contents (
  folder_id, name, file_id,
  PRIMARY KEY (folder_id, name)
) STRICT;
```

Full version: [src/db/schema.sql](src/db/schema.sql). Keeping names separate from files is what
real Unix filesystems do (directory entries vs. inodes), and it makes hard links work: two rows in
`folder_contents` simply point at the same `file_id`.

## Design decisions

**Low-level API instead of paths.** libfuse's high-level API hands you a path like `/a/b/c.txt`
on every call, which you'd have to resolve one folder at a time. The low-level API hands you inode
numbers, which map directly onto `file_id`. In return, sqlfs has to track how many references the
kernel holds to each inode.

**Deleting files that are still open.** POSIX lets a program keep reading a file after it's
deleted. sqlfs keeps in-memory counts of kernel lookups (released by `forget`) and open handles
(released by `release`). Removing the last name only drops `name_count` to 0. The row is deleted
once nothing references it. If sqlfs crashes first, the next mount sweeps up rows with
`name_count = 0`.

**Every operation is one transaction.** `rename` touches up to five rows (move the name, replace
a target, and update both folders' link counts and timestamps). Running each operation in a single
SQLite transaction means a crash never leaves a half-finished rename.

**Durability.** The database runs in WAL mode with `synchronous = NORMAL`, so ordinary writes are
fast and the database can never be corrupted. When a program calls `fsync`, sqlfs runs a full WAL
checkpoint so the data is really on disk, which is the same contract as a normal filesystem.

**One BLOB per file.** File contents live in a single `contents` column. That's simple and makes
the database easy to inspect, but it's the main tradeoff (see below).

## Known limitations

- **Writes that grow a file rewrite the whole file**, so writing a large file in small pieces is
  slow (quadratic in the file size). Files are limited to SQLite's ~1 GB BLOB maximum. Splitting
  contents into fixed-size chunks would fix both.
- **Single-threaded**: requests are handled one at a time.
- No extended attributes (xattrs) or `statfs` yet. `RENAME_EXCHANGE` (atomic swap) isn't supported.
- FSKit doesn't support FUSE's cache-invalidation notifications, so the kernel caches attributes
  for only 1 second.
- Inode numbers that programs see through `stat` come from FSKit and don't match `file_id`.
