PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;

-- Every file, folder, or symlink. file_id 1 is always the root folder (FUSE_ROOT_ID)
CREATE TABLE IF NOT EXISTS files (
  file_id        INTEGER PRIMARY KEY,
  kind           TEXT NOT NULL CHECK (kind IN ('file', 'folder', 'symlink')),
  permissions    INTEGER NOT NULL,          -- e.g., 0644
  owner_uid      INTEGER NOT NULL,
  group_gid      INTEGER NOT NULL,
  name_count     INTEGER NOT NULL,          -- names pointing here (POSIX nlink)
  size_bytes     INTEGER NOT NULL DEFAULT 0,
  contents       BLOB,                      -- file bytes; NULL for folders and symlinks
  symlink_target TEXT,
  created_ns     INTEGER NOT NULL,
  modified_ns    INTEGER NOT NULL,          -- contents changed
  changed_ns     INTEGER NOT NULL,          -- metadata changed
  accessed_ns    INTEGER NOT NULL
) STRICT;

-- "Folder X contains the name Y, which is file Z"
CREATE TABLE IF NOT EXISTS folder_contents (
  folder_id INTEGER NOT NULL REFERENCES files(file_id),
  name      TEXT NOT NULL,
  file_id   INTEGER NOT NULL REFERENCES files(file_id),
  PRIMARY KEY (folder_id, name)
) STRICT;

-- Finds a file's names quickly (used by rename and hard-link bookkeeping)
CREATE INDEX IF NOT EXISTS folder_contents_by_file ON folder_contents (file_id);
