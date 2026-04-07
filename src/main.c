#include "fsops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Option parsing ─────────────────────────────────────────────────────── */

struct sqlfs_opts {
    char *db_path;
    int   show_help;
};

#define SQLFS_OPT(templ, field) \
    { templ, offsetof(struct sqlfs_opts, field), 1 }

static const struct fuse_opt sqlfs_opt_spec[] = {
    SQLFS_OPT("db=%s",  db_path),
    SQLFS_OPT("-h",     show_help),
    SQLFS_OPT("--help", show_help),
    FUSE_OPT_END
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -o db=<database-file> <mountpoint> [FUSE options]\n"
        "\n"
        "Options:\n"
        "  -o db=PATH    Path to SQLite database file (created if absent)\n"
        "  -f            Run in foreground\n"
        "  -d            Debug mode (implies -f)\n"
        "  -s            Single-threaded mode\n"
        "  -h, --help    Show this help\n",
        prog);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct sqlfs_opts opts;
    memset(&opts, 0, sizeof(opts));

    if (fuse_opt_parse(&args, &opts, sqlfs_opt_spec, NULL) != 0) {
        usage(argv[0]);
        return 1;
    }

    if (opts.show_help) {
        usage(argv[0]);
        fuse_opt_free_args(&args);
        return 0;
    }

    if (!opts.db_path) {
        fprintf(stderr, "error: -o db=<path> is required\n\n");
        usage(argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }

    /* Initialise global state */
    memset(&g_state, 0, sizeof(g_state));
    if (db_open(opts.db_path, &g_state.db) != 0) {
        fprintf(stderr, "error: failed to open database '%s'\n", opts.db_path);
        fuse_opt_free_args(&args);
        return 1;
    }
    pthread_mutex_init(&g_state.lock, NULL);

    int ret = fuse_main(args.argc, args.argv, sqlfs_ops(), NULL);

    fuse_opt_free_args(&args);
    return ret;
}
