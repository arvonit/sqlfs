#pragma once

#include "db.h"

typedef struct {
    sqlite3         *db;
    pthread_mutex_t  lock;
} sqlfs_state_t;

extern sqlfs_state_t g_state;

/* Returns the populated fuse_operations struct */
const struct fuse_operations *sqlfs_ops(void);
