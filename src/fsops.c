#include "fsops.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* Linux-only rename flags may not exist on macOS */
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE  (1 << 1)
#endif

sqlfs_state_t g_state;

/* ── Lock helpers ───────────────────────────────────────────────────────── */

#define LOCK()   pthread_mutex_lock(&g_state.lock)
#define UNLOCK() pthread_mutex_unlock(&g_state.lock)

/* ── Path split ─────────────────────────────────────────────────────────── */

/* Splits "/a/b/c" → parent="/a/b", name="c".
   Splits "/foo"   → parent="/",    name="foo".
   Splits "/"      → parent="/",    name="".        */
static void path_split(const char *path, char *parent_out, char *name_out)
{
    const char *slash = strrchr(path, '/');
    size_t plen = (size_t)(slash - path);

    if (plen == 0) {
        strcpy(parent_out, "/");
    } else {
        memcpy(parent_out, path, plen);
        parent_out[plen] = '\0';
    }
    strcpy(name_out, slash + 1);
}

/* ── Stat fill helper ───────────────────────────────────────────────────── */

static void inode_to_stat(const inode_t *ino, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino    = (ino_t)ino->ino;
    st->st_mode   = ino->mode;
    st->st_nlink  = ino->nlink;
    st->st_uid    = ino->uid;
    st->st_gid    = ino->gid;
    st->st_size   = ino->size;
    st->st_atime  = ino->atime;
    st->st_mtime  = ino->mtime;
    st->st_ctime  = ino->ctime;
    st->st_blksize = BLOCK_SIZE;
    st->st_blocks  = (blkcnt_t)((ino->size + 511) / 512);
}

/* ── FUSE callbacks ─────────────────────────────────────────────────────── */

static void *sqlfs_init(struct fuse_conn_info *conn,
                        struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache  = 0;
    cfg->use_ino       = 1;
    cfg->entry_timeout = 0.0;
    cfg->attr_timeout  = 0.0;
    return &g_state;
}

static void sqlfs_destroy(void *private_data)
{
    (void)private_data;
    db_close(g_state.db);
    pthread_mutex_destroy(&g_state.lock);
}

static int sqlfs_getattr(const char *path, struct stat *st,
                         struct fuse_file_info *fi)
{
    (void)fi;
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    UNLOCK();
    if (r < 0) return r;
    inode_to_stat(&ino, st);
    return 0;
}

static int sqlfs_readdir(const char *path, void *buf,
                         fuse_fill_dir_t filler, off_t offset,
                         struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    LOCK();
    inode_t dir;
    int r = db_get_inode_by_path(g_state.db, path, &dir);
    if (r < 0) { UNLOCK(); return r; }
    if (!S_ISDIR(dir.mode)) { UNLOCK(); return -ENOTDIR; }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    db_readdir(g_state.db, dir.ino, buf, filler);
    UNLOCK();
    return 0;
}

static int sqlfs_open(const char *path, struct fuse_file_info *fi)
{
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    UNLOCK();
    if (r < 0) return r;
    if (S_ISDIR(ino.mode)) return -EISDIR;
    fi->fh = (uint64_t)ino.ino;
    return 0;
}

static int sqlfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    fi->fh = 0;
    return 0;
}

static int sqlfs_read(const char *path, char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    LOCK();
    int r = db_read_blocks(g_state.db, (int64_t)fi->fh, buf, size, offset);
    UNLOCK();
    return r;
}

static int sqlfs_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    LOCK();
    int r = db_write_blocks(g_state.db, (int64_t)fi->fh, buf, size, offset);
    UNLOCK();
    return r;
}

static int sqlfs_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi)
{
    char parent_path[4096], name[256];
    path_split(path, parent_path, name);
    if (!name[0]) return -EINVAL;

    LOCK();
    inode_t parent;
    int r = db_get_inode_by_path(g_state.db, parent_path, &parent);
    if (r < 0) { UNLOCK(); return r; }
    if (!S_ISDIR(parent.mode)) { UNLOCK(); return -ENOTDIR; }

    time_t now = time(NULL);
    inode_t newino;
    memset(&newino, 0, sizeof(newino));
    newino.parent = parent.ino;
    strncpy(newino.name, name, sizeof(newino.name) - 1);
    newino.type  = DT_REG;
    newino.mode  = S_IFREG | (mode & 07777);
    newino.uid   = getuid();
    newino.gid   = getgid();
    newino.size  = 0;
    newino.atime = newino.mtime = newino.ctime = now;
    newino.nlink = 1;

    r = db_create_inode(g_state.db, &newino);
    UNLOCK();
    if (r < 0) return r;
    fi->fh = (uint64_t)newino.ino;
    return 0;
}

static int sqlfs_mkdir(const char *path, mode_t mode)
{
    char parent_path[4096], name[256];
    path_split(path, parent_path, name);
    if (!name[0]) return -EINVAL;

    LOCK();
    inode_t parent;
    int r = db_get_inode_by_path(g_state.db, parent_path, &parent);
    if (r < 0) { UNLOCK(); return r; }
    if (!S_ISDIR(parent.mode)) { UNLOCK(); return -ENOTDIR; }

    time_t now = time(NULL);
    inode_t newino;
    memset(&newino, 0, sizeof(newino));
    newino.parent = parent.ino;
    strncpy(newino.name, name, sizeof(newino.name) - 1);
    newino.type  = DT_DIR;
    newino.mode  = S_IFDIR | (mode & 07777);
    newino.uid   = getuid();
    newino.gid   = getgid();
    newino.size  = 0;
    newino.atime = newino.mtime = newino.ctime = now;
    newino.nlink = 2;  /* "." + future ".." references */

    r = db_create_inode(g_state.db, &newino);
    if (r == 0)
        db_nlink_delta(g_state.db, parent.ino, 1);
    UNLOCK();
    return r;
}

static int sqlfs_unlink(const char *path)
{
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    if (r < 0) { UNLOCK(); return r; }
    if (S_ISDIR(ino.mode)) { UNLOCK(); return -EISDIR; }

    db_nlink_delta(g_state.db, ino.ino, -1);
    /* Re-read to check nlink after delta */
    inode_t updated;
    db_get_inode(g_state.db, ino.ino, &updated);
    if ((int)updated.nlink <= 0) {
        db_truncate_blocks(g_state.db, ino.ino, 0);
        db_delete_inode(g_state.db, ino.ino);
    }
    UNLOCK();
    return 0;
}

static int sqlfs_rmdir(const char *path)
{
    char parent_path[4096], name[256];
    path_split(path, parent_path, name);

    LOCK();
    inode_t dir;
    int r = db_get_inode_by_path(g_state.db, path, &dir);
    if (r < 0) { UNLOCK(); return r; }
    if (!S_ISDIR(dir.mode)) { UNLOCK(); return -ENOTDIR; }

    if (db_count_children(g_state.db, dir.ino) > 0) {
        UNLOCK();
        return -ENOTEMPTY;
    }

    inode_t parent;
    r = db_get_inode_by_path(g_state.db, parent_path, &parent);
    if (r == 0)
        db_nlink_delta(g_state.db, parent.ino, -1);

    db_delete_inode(g_state.db, dir.ino);
    UNLOCK();
    return 0;
}

static int sqlfs_rename(const char *from, const char *to,
                        unsigned int flags)
{
    /* RENAME_EXCHANGE not supported */
    if (flags & RENAME_EXCHANGE) return -EINVAL;

    char from_parent[4096], from_name[256];
    char to_parent[4096],   to_name[256];
    path_split(from, from_parent, from_name);
    path_split(to,   to_parent,   to_name);

    LOCK();
    inode_t fp, tp;
    int r = db_get_inode_by_path(g_state.db, from_parent, &fp);
    if (r < 0) { UNLOCK(); return r; }
    r = db_get_inode_by_path(g_state.db, to_parent, &tp);
    if (r < 0) { UNLOCK(); return r; }

    r = db_rename(g_state.db, fp.ino, from_name, tp.ino, to_name);
    UNLOCK();
    return r;
}

static int sqlfs_truncate(const char *path, off_t size,
                          struct fuse_file_info *fi)
{
    LOCK();
    int64_t ino;
    if (fi && fi->fh) {
        ino = (int64_t)fi->fh;
    } else {
        inode_t i;
        int r = db_get_inode_by_path(g_state.db, path, &i);
        if (r < 0) { UNLOCK(); return r; }
        ino = i.ino;
    }
    int r = db_truncate_blocks(g_state.db, ino, size);
    UNLOCK();
    return r;
}

static int sqlfs_utimens(const char *path, const struct timespec tv[2],
                         struct fuse_file_info *fi)
{
    (void)fi;
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    if (r < 0) { UNLOCK(); return r; }
    ino.atime = tv[0].tv_sec;
    ino.mtime = tv[1].tv_sec;
    ino.ctime = time(NULL);
    r = db_update_inode(g_state.db, &ino);
    UNLOCK();
    return r;
}

static int sqlfs_chmod(const char *path, mode_t mode,
                       struct fuse_file_info *fi)
{
    (void)fi;
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    if (r < 0) { UNLOCK(); return r; }
    ino.mode  = (ino.mode & S_IFMT) | (mode & ~(mode_t)S_IFMT);
    ino.ctime = time(NULL);
    r = db_update_inode(g_state.db, &ino);
    UNLOCK();
    return r;
}

static int sqlfs_chown(const char *path, uid_t uid, gid_t gid,
                       struct fuse_file_info *fi)
{
    (void)fi;
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    if (r < 0) { UNLOCK(); return r; }
    if (uid != (uid_t)-1) ino.uid = uid;
    if (gid != (gid_t)-1) ino.gid = gid;
    ino.ctime = time(NULL);
    r = db_update_inode(g_state.db, &ino);
    UNLOCK();
    return r;
}

static int sqlfs_symlink(const char *target, const char *linkpath)
{
    char parent_path[4096], name[256];
    path_split(linkpath, parent_path, name);
    if (!name[0]) return -EINVAL;

    LOCK();
    inode_t parent;
    int r = db_get_inode_by_path(g_state.db, parent_path, &parent);
    if (r < 0) { UNLOCK(); return r; }
    if (!S_ISDIR(parent.mode)) { UNLOCK(); return -ENOTDIR; }

    time_t now = time(NULL);
    inode_t newino;
    memset(&newino, 0, sizeof(newino));
    newino.parent = parent.ino;
    strncpy(newino.name, name, sizeof(newino.name) - 1);
    newino.type  = DT_LNK;
    newino.mode  = S_IFLNK | 0777;
    newino.uid   = getuid();
    newino.gid   = getgid();
    newino.size  = (off_t)strlen(target);
    newino.atime = newino.mtime = newino.ctime = now;
    newino.nlink = 1;
    strncpy(newino.target, target, sizeof(newino.target) - 1);

    r = db_create_inode(g_state.db, &newino);
    UNLOCK();
    return r;
}

static int sqlfs_readlink(const char *path, char *buf, size_t size)
{
    LOCK();
    inode_t ino;
    int r = db_get_inode_by_path(g_state.db, path, &ino);
    UNLOCK();
    if (r < 0) return r;
    if (!S_ISLNK(ino.mode)) return -EINVAL;
    strncpy(buf, ino.target, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

static int sqlfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    LOCK();
    int64_t used_bytes  = db_sum_block_bytes(g_state.db);
    int64_t total_files = db_count_inodes(g_state.db);
    UNLOCK();

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = BLOCK_SIZE;
    stbuf->f_frsize  = BLOCK_SIZE;

    /* Advertise 1 TiB total */
    const uint64_t total_bytes = 1ULL << 40;
    stbuf->f_blocks = total_bytes / BLOCK_SIZE;
    stbuf->f_bfree  = stbuf->f_blocks - (uint64_t)(used_bytes / BLOCK_SIZE);
    stbuf->f_bavail = stbuf->f_bfree;

    stbuf->f_files  = (fsfilcnt_t)total_files;
    stbuf->f_ffree  = (1 << 20) - (fsfilcnt_t)total_files;
    stbuf->f_namemax = 255;
    return 0;
}

/* ── ops table ──────────────────────────────────────────────────────────── */

static const struct fuse_operations sqlfs_operations = {
    .init     = sqlfs_init,
    .destroy  = sqlfs_destroy,
    .getattr  = sqlfs_getattr,
    .readdir  = sqlfs_readdir,
    .open     = sqlfs_open,
    .release  = sqlfs_release,
    .read     = sqlfs_read,
    .write    = sqlfs_write,
    .create   = sqlfs_create,
    .mkdir    = sqlfs_mkdir,
    .unlink   = sqlfs_unlink,
    .rmdir    = sqlfs_rmdir,
    .rename   = sqlfs_rename,
    .truncate = sqlfs_truncate,
    .utimens  = sqlfs_utimens,
    .chmod    = sqlfs_chmod,
    .chown    = sqlfs_chown,
    .symlink  = sqlfs_symlink,
    .readlink = sqlfs_readlink,
    .statfs   = sqlfs_statfs,
};

const struct fuse_operations *sqlfs_ops(void)
{
    return &sqlfs_operations;
}
