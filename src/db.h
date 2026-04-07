#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <sqlite3.h>

/* Must come before any fuse include */
#ifdef __APPLE__
#  define FUSE_USE_VERSION 31
#  include <fuse.h>
#else
#  define FUSE_USE_VERSION 31
#  include <fuse3/fuse.h>
#endif

#define BLOCK_SIZE 4096

typedef struct {
    int64_t  ino;
    int64_t  parent;
    char     name[256];
    int      type;          /* DT_REG=8, DT_DIR=4, DT_LNK=10 */
    mode_t   mode;
    uid_t    uid;
    gid_t    gid;
    off_t    size;
    time_t   atime;
    time_t   mtime;
    time_t   ctime;
    nlink_t  nlink;
    char     target[4096];  /* symlink target, or "" */
} inode_t;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
int  db_open(const char *path, sqlite3 **db);
void db_close(sqlite3 *db);

/* ── Inode operations (return 0 or -errno) ──────────────────────────────── */
int db_get_inode(sqlite3 *db, int64_t ino, inode_t *out);
int db_get_inode_by_path(sqlite3 *db, const char *path, inode_t *out);
int db_lookup(sqlite3 *db, int64_t parent_ino, const char *name, inode_t *out);
int db_create_inode(sqlite3 *db, inode_t *in_out); /* sets in_out->ino */
int db_update_inode(sqlite3 *db, const inode_t *inode);
int db_delete_inode(sqlite3 *db, int64_t ino);
int db_readdir(sqlite3 *db, int64_t ino, void *buf, fuse_fill_dir_t filler);
int db_rename(sqlite3 *db, int64_t old_parent, const char *old_name,
              int64_t new_parent, const char *new_name);
int db_nlink_delta(sqlite3 *db, int64_t ino, int delta);
int db_count_children(sqlite3 *db, int64_t ino);

/* ── Block operations (return bytes transferred or -errno) ──────────────── */
int db_read_blocks(sqlite3 *db, int64_t ino,
                   char *buf, size_t size, off_t offset);
int db_write_blocks(sqlite3 *db, int64_t ino,
                    const char *buf, size_t size, off_t offset);
int db_truncate_blocks(sqlite3 *db, int64_t ino, off_t new_size);

/* ── statfs helpers ─────────────────────────────────────────────────────── */
int64_t db_sum_block_bytes(sqlite3 *db);
int64_t db_count_inodes(sqlite3 *db);
