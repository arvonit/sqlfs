#include "db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Schema ─────────────────────────────────────────────────────────────── */

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA foreign_keys = ON;"
    "PRAGMA synchronous = NORMAL;"

    "CREATE TABLE IF NOT EXISTS inodes ("
    "  ino     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent  INTEGER NOT NULL,"
    "  name    TEXT    NOT NULL,"
    "  type    INTEGER NOT NULL,"
    "  mode    INTEGER NOT NULL,"
    "  uid     INTEGER NOT NULL DEFAULT 0,"
    "  gid     INTEGER NOT NULL DEFAULT 0,"
    "  size    INTEGER NOT NULL DEFAULT 0,"
    "  atime   INTEGER NOT NULL DEFAULT 0,"
    "  mtime   INTEGER NOT NULL DEFAULT 0,"
    "  ctime   INTEGER NOT NULL DEFAULT 0,"
    "  nlink   INTEGER NOT NULL DEFAULT 1,"
    "  target  TEXT    DEFAULT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS blocks ("
    "  ino     INTEGER NOT NULL,"
    "  blk_num INTEGER NOT NULL,"
    "  data    BLOB    NOT NULL,"
    "  PRIMARY KEY (ino, blk_num)"
    ");"

    "CREATE UNIQUE INDEX IF NOT EXISTS idx_inodes_parent_name"
    "  ON inodes (parent, name);";

/* ── Prepared statement cache ───────────────────────────────────────────── */

typedef struct {
    sqlite3_stmt *get_inode;        /* SELECT * FROM inodes WHERE ino=? */
    sqlite3_stmt *lookup;           /* SELECT * FROM inodes WHERE parent=? AND name=? */
    sqlite3_stmt *insert_inode;     /* INSERT INTO inodes ... */
    sqlite3_stmt *update_inode;     /* UPDATE inodes SET ... WHERE ino=? */
    sqlite3_stmt *delete_inode;     /* DELETE FROM inodes WHERE ino=? */
    sqlite3_stmt *readdir;          /* SELECT ino,name,type FROM inodes WHERE parent=? */
    sqlite3_stmt *count_children;   /* SELECT COUNT(*) FROM inodes WHERE parent=? */
    sqlite3_stmt *nlink_delta;      /* UPDATE inodes SET nlink=nlink+? WHERE ino=? */
    sqlite3_stmt *get_block;        /* SELECT data FROM blocks WHERE ino=? AND blk_num=? */
    sqlite3_stmt *put_block;        /* INSERT OR REPLACE INTO blocks VALUES(?,?,?) */
    sqlite3_stmt *del_blocks_from;  /* DELETE FROM blocks WHERE ino=? AND blk_num>=? */
    sqlite3_stmt *trim_block;       /* SELECT data FROM blocks WHERE ino=? AND blk_num=? */
    sqlite3_stmt *sum_blocks;       /* SELECT COALESCE(SUM(length(data)),0) FROM blocks */
    sqlite3_stmt *count_inodes;     /* SELECT COUNT(*) FROM inodes */
} stmt_cache_t;

static stmt_cache_t g_stmts;

#define PREP(db, field, sql) \
    do { \
        if (sqlite3_prepare_v2((db), (sql), -1, &g_stmts.field, NULL) != SQLITE_OK) { \
            fprintf(stderr, "db: prepare failed [" #field "]: %s\n", \
                    sqlite3_errmsg(db)); \
            return -EIO; \
        } \
    } while (0)

static int prepare_statements(sqlite3 *db)
{
    PREP(db, get_inode,
         "SELECT ino,parent,name,type,mode,uid,gid,size,"
         "atime,mtime,ctime,nlink,COALESCE(target,'') "
         "FROM inodes WHERE ino=?");

    PREP(db, lookup,
         "SELECT ino,parent,name,type,mode,uid,gid,size,"
         "atime,mtime,ctime,nlink,COALESCE(target,'') "
         "FROM inodes WHERE parent=? AND name=?");

    PREP(db, insert_inode,
         "INSERT INTO inodes (parent,name,type,mode,uid,gid,size,"
         "atime,mtime,ctime,nlink,target) "
         "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");

    PREP(db, update_inode,
         "UPDATE inodes SET parent=?,name=?,type=?,mode=?,uid=?,gid=?,size=?,"
         "atime=?,mtime=?,ctime=?,nlink=?,target=? WHERE ino=?");

    PREP(db, delete_inode,
         "DELETE FROM inodes WHERE ino=?");

    PREP(db, readdir,
         "SELECT ino,name,type FROM inodes WHERE parent=?");

    PREP(db, count_children,
         "SELECT COUNT(*) FROM inodes WHERE parent=?");

    PREP(db, nlink_delta,
         "UPDATE inodes SET nlink=nlink+? WHERE ino=?");

    PREP(db, get_block,
         "SELECT data FROM blocks WHERE ino=? AND blk_num=?");

    PREP(db, put_block,
         "INSERT OR REPLACE INTO blocks (ino,blk_num,data) VALUES (?,?,?)");

    PREP(db, del_blocks_from,
         "DELETE FROM blocks WHERE ino=? AND blk_num>=?");

    /* reuse get_block for trim_block read step */
    PREP(db, trim_block,
         "SELECT data FROM blocks WHERE ino=? AND blk_num=?");

    PREP(db, sum_blocks,
         "SELECT COALESCE(SUM(length(data)),0) FROM blocks");

    PREP(db, count_inodes,
         "SELECT COUNT(*) FROM inodes");

    return 0;
}

/* ── Row → inode_t helper ───────────────────────────────────────────────── */

static void row_to_inode(sqlite3_stmt *s, inode_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ino    = sqlite3_column_int64(s, 0);
    out->parent = sqlite3_column_int64(s, 1);

    const char *n = (const char *)sqlite3_column_text(s, 2);
    if (n) strncpy(out->name, n, sizeof(out->name) - 1);

    out->type   = sqlite3_column_int(s, 3);
    out->mode   = (mode_t)sqlite3_column_int(s, 4);
    out->uid    = (uid_t)sqlite3_column_int(s, 5);
    out->gid    = (gid_t)sqlite3_column_int(s, 6);
    out->size   = (off_t)sqlite3_column_int64(s, 7);
    out->atime  = (time_t)sqlite3_column_int64(s, 8);
    out->mtime  = (time_t)sqlite3_column_int64(s, 9);
    out->ctime  = (time_t)sqlite3_column_int64(s, 10);
    out->nlink  = (nlink_t)sqlite3_column_int(s, 11);

    const char *t = (const char *)sqlite3_column_text(s, 12);
    if (t) strncpy(out->target, t, sizeof(out->target) - 1);
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

int db_open(const char *path, sqlite3 **db)
{
    int rc = sqlite3_open(path, db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open: cannot open '%s': %s\n",
                path, sqlite3_errmsg(*db));
        return -EIO;
    }

    /* Apply schema */
    char *errmsg = NULL;
    rc = sqlite3_exec(*db, SCHEMA_SQL, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open: schema error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -EIO;
    }

    /* Seed root inode if the database is empty */
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(*db,
        "SELECT COUNT(*) FROM inodes", -1, &s, NULL);
    sqlite3_step(s);
    int count = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);

    if (count == 0) {
        time_t now = time(NULL);
        rc = sqlite3_exec(*db,
            "INSERT INTO inodes "
            "(ino,parent,name,type,mode,uid,gid,size,atime,mtime,ctime,nlink)"
            " SELECT 1,0,'',4,16877,"
            "        (SELECT uid FROM (SELECT ?1 AS uid)),"
            "        (SELECT gid FROM (SELECT ?2 AS gid)),"
            "        0,?3,?3,?3,2",
            NULL, NULL, &errmsg);

        /* Simple parametrized insert instead */
        (void)errmsg;
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(*db,
            "INSERT INTO inodes "
            "(ino,parent,name,type,mode,uid,gid,size,atime,mtime,ctime,nlink)"
            " VALUES (1,0,'',4,16877,?,?,0,?,?,?,2)",
            -1, &ins, NULL);
        sqlite3_bind_int64(ins, 1, (sqlite3_int64)getuid());
        sqlite3_bind_int64(ins, 2, (sqlite3_int64)getgid());
        sqlite3_bind_int64(ins, 3, (sqlite3_int64)now);
        sqlite3_bind_int64(ins, 4, (sqlite3_int64)now);
        sqlite3_bind_int64(ins, 5, (sqlite3_int64)now);
        rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        if (rc != SQLITE_DONE) {
            fprintf(stderr, "db_open: root insert failed: %s\n",
                    sqlite3_errmsg(*db));
            return -EIO;
        }
    }

    return prepare_statements(*db);
}

void db_close(sqlite3 *db)
{
    if (!db) return;
    sqlite3_finalize(g_stmts.get_inode);
    sqlite3_finalize(g_stmts.lookup);
    sqlite3_finalize(g_stmts.insert_inode);
    sqlite3_finalize(g_stmts.update_inode);
    sqlite3_finalize(g_stmts.delete_inode);
    sqlite3_finalize(g_stmts.readdir);
    sqlite3_finalize(g_stmts.count_children);
    sqlite3_finalize(g_stmts.nlink_delta);
    sqlite3_finalize(g_stmts.get_block);
    sqlite3_finalize(g_stmts.put_block);
    sqlite3_finalize(g_stmts.del_blocks_from);
    sqlite3_finalize(g_stmts.trim_block);
    sqlite3_finalize(g_stmts.sum_blocks);
    sqlite3_finalize(g_stmts.count_inodes);
    sqlite3_close(db);
}

/* ── Inode operations ───────────────────────────────────────────────────── */

int db_get_inode(sqlite3 *db, int64_t ino, inode_t *out)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.get_inode;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int64(s, 1, ino);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) return -ENOENT;
    row_to_inode(s, out);
    return 0;
}

int db_lookup(sqlite3 *db, int64_t parent_ino, const char *name, inode_t *out)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.lookup;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int64(s, 1, parent_ino);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) return -ENOENT;
    row_to_inode(s, out);
    return 0;
}

int db_get_inode_by_path(sqlite3 *db, const char *path, inode_t *out)
{
    /* Root shortcut */
    if (strcmp(path, "/") == 0)
        return db_get_inode(db, 1, out);

    /* Walk components */
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int64_t cur_ino = 1;
    inode_t cur;
    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "/", &saveptr);
    while (tok) {
        int r = db_lookup(db, cur_ino, tok, &cur);
        if (r < 0) return r;
        cur_ino = cur.ino;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return db_get_inode(db, cur_ino, out);
}

int db_create_inode(sqlite3 *db, inode_t *in_out)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.insert_inode;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    sqlite3_bind_int64(s, 1,  in_out->parent);
    sqlite3_bind_text(s,  2,  in_out->name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s,   3,  in_out->type);
    sqlite3_bind_int(s,   4,  (int)in_out->mode);
    sqlite3_bind_int64(s, 5,  (sqlite3_int64)in_out->uid);
    sqlite3_bind_int64(s, 6,  (sqlite3_int64)in_out->gid);
    sqlite3_bind_int64(s, 7,  (sqlite3_int64)in_out->size);
    sqlite3_bind_int64(s, 8,  (sqlite3_int64)in_out->atime);
    sqlite3_bind_int64(s, 9,  (sqlite3_int64)in_out->mtime);
    sqlite3_bind_int64(s, 10, (sqlite3_int64)in_out->ctime);
    sqlite3_bind_int(s,   11, (int)in_out->nlink);

    if (in_out->target[0])
        sqlite3_bind_text(s, 12, in_out->target, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 12);

    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) return -EIO;

    in_out->ino = sqlite3_last_insert_rowid(
        sqlite3_db_handle(g_stmts.insert_inode));
    return 0;
}

int db_update_inode(sqlite3 *db, const inode_t *inode)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.update_inode;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    sqlite3_bind_int64(s, 1,  inode->parent);
    sqlite3_bind_text(s,  2,  inode->name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s,   3,  inode->type);
    sqlite3_bind_int(s,   4,  (int)inode->mode);
    sqlite3_bind_int64(s, 5,  (sqlite3_int64)inode->uid);
    sqlite3_bind_int64(s, 6,  (sqlite3_int64)inode->gid);
    sqlite3_bind_int64(s, 7,  (sqlite3_int64)inode->size);
    sqlite3_bind_int64(s, 8,  (sqlite3_int64)inode->atime);
    sqlite3_bind_int64(s, 9,  (sqlite3_int64)inode->mtime);
    sqlite3_bind_int64(s, 10, (sqlite3_int64)inode->ctime);
    sqlite3_bind_int(s,   11, (int)inode->nlink);

    if (inode->target[0])
        sqlite3_bind_text(s, 12, inode->target, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 12);

    sqlite3_bind_int64(s, 13, inode->ino);

    int rc = sqlite3_step(s);
    return (rc == SQLITE_DONE) ? 0 : -EIO;
}

int db_delete_inode(sqlite3 *db, int64_t ino)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.delete_inode;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int64(s, 1, ino);
    int rc = sqlite3_step(s);
    return (rc == SQLITE_DONE) ? 0 : -EIO;
}

int db_readdir(sqlite3 *db, int64_t ino, void *buf, fuse_fill_dir_t filler)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.readdir;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int64(s, 1, ino);

    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(s, 1);
        if (name && name[0])
            filler(buf, name, NULL, 0, 0);
    }
    return 0;
}

int db_rename(sqlite3 *db, int64_t old_parent, const char *old_name,
              int64_t new_parent, const char *new_name)
{
    /* Atomic rename with optional overwrite */
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -EIO;

    /* Check if destination exists */
    inode_t dest;
    int dest_exists = (db_lookup(db, new_parent, new_name, &dest) == 0);
    if (dest_exists) {
        /* If dest is a non-empty directory, refuse */
        if (S_ISDIR(dest.mode)) {
            sqlite3_stmt *cc = g_stmts.count_children;
            sqlite3_reset(cc);
            sqlite3_clear_bindings(cc);
            sqlite3_bind_int64(cc, 1, dest.ino);
            sqlite3_step(cc);
            int cnt = sqlite3_column_int(cc, 0);
            if (cnt > 0) {
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                return -ENOTEMPTY;
            }
        }
        /* Delete destination */
        db_delete_inode(db, dest.ino);
        /* Also delete any blocks for a regular file */
        sqlite3_stmt *dbl = g_stmts.del_blocks_from;
        sqlite3_reset(dbl);
        sqlite3_clear_bindings(dbl);
        sqlite3_bind_int64(dbl, 1, dest.ino);
        sqlite3_bind_int64(dbl, 2, 0);
        sqlite3_step(dbl);
    }

    /* Perform the rename */
    sqlite3_stmt *upd = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE inodes SET parent=?, name=? WHERE parent=? AND name=?",
        -1, &upd, NULL);
    sqlite3_bind_int64(upd, 1, new_parent);
    sqlite3_bind_text(upd,  2, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(upd, 3, old_parent);
    sqlite3_bind_text(upd,  4, old_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(upd);
    sqlite3_finalize(upd);

    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -EIO;
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

int db_nlink_delta(sqlite3 *db, int64_t ino, int delta)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.nlink_delta;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int(s,   1, delta);
    sqlite3_bind_int64(s, 2, ino);
    int rc = sqlite3_step(s);
    return (rc == SQLITE_DONE) ? 0 : -EIO;
}

int db_count_children(sqlite3 *db, int64_t ino)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.count_children;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    sqlite3_bind_int64(s, 1, ino);
    if (sqlite3_step(s) != SQLITE_ROW) return 0;
    return sqlite3_column_int(s, 0);
}

/* ── Block operations ───────────────────────────────────────────────────── */

int db_read_blocks(sqlite3 *db, int64_t ino,
                   char *buf, size_t size, off_t offset)
{
    (void)db;
    if (size == 0) return 0;

    int64_t first_blk = offset / BLOCK_SIZE;
    int64_t last_blk  = ((int64_t)offset + (int64_t)size - 1) / BLOCK_SIZE;
    int bytes_read = 0;

    for (int64_t blk = first_blk; blk <= last_blk; blk++) {
        int blk_start = (blk == first_blk) ? (int)(offset % BLOCK_SIZE) : 0;
        int dst_off   = (int)(blk * BLOCK_SIZE + blk_start - offset);
        int want      = BLOCK_SIZE - blk_start;
        int remaining = (int)size - dst_off;
        if (want > remaining) want = remaining;
        if (want <= 0) break;

        sqlite3_stmt *s = g_stmts.get_block;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, ino);
        sqlite3_bind_int64(s, 2, blk);

        if (sqlite3_step(s) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(s, 0);
            int blob_len     = sqlite3_column_bytes(s, 0);
            int copy_len = blob_len - blk_start;
            if (copy_len > want) copy_len = want;
            if (copy_len > 0) {
                memcpy(buf + dst_off, (const char *)blob + blk_start, copy_len);
                if (copy_len < want)
                    memset(buf + dst_off + copy_len, 0, want - copy_len);
            } else {
                memset(buf + dst_off, 0, want);
            }
        } else {
            /* Sparse hole */
            memset(buf + dst_off, 0, want);
        }
        bytes_read += want;
    }
    return bytes_read;
}

int db_write_blocks(sqlite3 *db, int64_t ino,
                    const char *buf, size_t size, off_t offset)
{
    if (size == 0) return 0;

    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    int64_t first_blk = offset / BLOCK_SIZE;
    int64_t last_blk  = ((int64_t)offset + (int64_t)size - 1) / BLOCK_SIZE;
    int bytes_written = 0;

    for (int64_t blk = first_blk; blk <= last_blk; blk++) {
        int blk_off  = (blk == first_blk) ? (int)(offset % BLOCK_SIZE) : 0;
        int src_off  = (int)(blk * BLOCK_SIZE + blk_off - offset);
        int write_len = BLOCK_SIZE - blk_off;
        int remaining = (int)size - src_off;
        if (write_len > remaining) write_len = remaining;
        if (write_len <= 0) break;

        char staging[BLOCK_SIZE];

        if (blk_off > 0 || write_len < BLOCK_SIZE) {
            /* Partial block: read-modify-write */
            memset(staging, 0, BLOCK_SIZE);
            sqlite3_stmt *gs = g_stmts.get_block;
            sqlite3_reset(gs);
            sqlite3_clear_bindings(gs);
            sqlite3_bind_int64(gs, 1, ino);
            sqlite3_bind_int64(gs, 2, blk);
            if (sqlite3_step(gs) == SQLITE_ROW) {
                const void *blob = sqlite3_column_blob(gs, 0);
                int blob_len     = sqlite3_column_bytes(gs, 0);
                if (blob_len > 0)
                    memcpy(staging, blob, blob_len < BLOCK_SIZE ? blob_len : BLOCK_SIZE);
            }
        } else {
            /* Full block */
            memcpy(staging, buf + src_off, BLOCK_SIZE);
        }

        if (blk_off > 0 || write_len < BLOCK_SIZE)
            memcpy(staging + blk_off, buf + src_off, write_len);

        sqlite3_stmt *ps = g_stmts.put_block;
        sqlite3_reset(ps);
        sqlite3_clear_bindings(ps);
        sqlite3_bind_int64(ps, 1, ino);
        sqlite3_bind_int64(ps, 2, blk);
        sqlite3_bind_blob(ps,  3, staging, BLOCK_SIZE, SQLITE_TRANSIENT);
        sqlite3_step(ps);

        bytes_written += write_len;
    }

    /* Update inode size if we extended the file */
    sqlite3_stmt *us = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE inodes SET size=MAX(size,?), mtime=?, ctime=? WHERE ino=?",
        -1, &us, NULL);
    time_t now = time(NULL);
    sqlite3_bind_int64(us, 1, (sqlite3_int64)(offset + (off_t)size));
    sqlite3_bind_int64(us, 2, (sqlite3_int64)now);
    sqlite3_bind_int64(us, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(us, 4, ino);
    sqlite3_step(us);
    sqlite3_finalize(us);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return bytes_written;
}

int db_truncate_blocks(sqlite3 *db, int64_t ino, off_t new_size)
{
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

    int64_t keep_blks = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* Delete blocks beyond the new size */
    sqlite3_stmt *del = g_stmts.del_blocks_from;
    sqlite3_reset(del);
    sqlite3_clear_bindings(del);
    sqlite3_bind_int64(del, 1, ino);
    sqlite3_bind_int64(del, 2, keep_blks);
    sqlite3_step(del);

    /* Trim the last partial block if size is not block-aligned */
    if (new_size > 0 && (new_size % BLOCK_SIZE) != 0) {
        int64_t last_blk = (new_size - 1) / BLOCK_SIZE;
        int keep_bytes   = (int)(new_size % BLOCK_SIZE);

        sqlite3_stmt *gs = g_stmts.trim_block;
        sqlite3_reset(gs);
        sqlite3_clear_bindings(gs);
        sqlite3_bind_int64(gs, 1, ino);
        sqlite3_bind_int64(gs, 2, last_blk);

        if (sqlite3_step(gs) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(gs, 0);
            int blob_len     = sqlite3_column_bytes(gs, 0);
            if (blob_len > keep_bytes) {
                /* Write trimmed version */
                sqlite3_stmt *ps = g_stmts.put_block;
                sqlite3_reset(ps);
                sqlite3_clear_bindings(ps);
                sqlite3_bind_int64(ps, 1, ino);
                sqlite3_bind_int64(ps, 2, last_blk);
                sqlite3_bind_blob(ps, 3, blob, keep_bytes, SQLITE_TRANSIENT);
                sqlite3_step(ps);
            }
        }
    }

    /* Update size in inode */
    sqlite3_stmt *us = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE inodes SET size=?, mtime=?, ctime=? WHERE ino=?",
        -1, &us, NULL);
    time_t now = time(NULL);
    sqlite3_bind_int64(us, 1, (sqlite3_int64)new_size);
    sqlite3_bind_int64(us, 2, (sqlite3_int64)now);
    sqlite3_bind_int64(us, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(us, 4, ino);
    sqlite3_step(us);
    sqlite3_finalize(us);

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    return 0;
}

/* ── statfs helpers ─────────────────────────────────────────────────────── */

int64_t db_sum_block_bytes(sqlite3 *db)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.sum_blocks;
    sqlite3_reset(s);
    if (sqlite3_step(s) != SQLITE_ROW) return 0;
    return sqlite3_column_int64(s, 0);
}

int64_t db_count_inodes(sqlite3 *db)
{
    (void)db;
    sqlite3_stmt *s = g_stmts.count_inodes;
    sqlite3_reset(s);
    if (sqlite3_step(s) != SQLITE_ROW) return 0;
    return sqlite3_column_int64(s, 0);
}
