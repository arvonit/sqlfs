#include "core/filesystem.h"
#include "fuse/adapter.h"

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    bool debug = argc == 4 && std::strcmp(argv[3], "--debug") == 0;
    if (argc != 3 && !debug) {
        std::fprintf(stderr, "usage: %s DATABASE MOUNTPOINT [--debug]\n", argv[0]);
        std::fprintf(stderr, "example: %s my.db /Volumes/sqlfs\n", argv[0]);
        return 1;
    }
    const char* database_path = argv[1];
    const char* mountpoint = argv[2];

    Filesystem filesystem;
    auto opened = filesystem.open_database(database_path);
    if (!opened) {
        std::fprintf(stderr,
                     "sqlfs: cannot open %s: %s\n",
                     database_path,
                     std::strerror(opened.error()));
        return 1;
    }

    // default_permissions makes the kernel enforce the permission bits for us
    const char* fuse_argv[] = {argv[0],
                               "-o",
                               "backend=fskit,volname=sqlfs,default_permissions",
                               "-d"};
    fuse_args args = FUSE_ARGS_INIT(debug ? 4 : 3, const_cast<char**>(fuse_argv));

    const fuse_lowlevel_ops& operations = sqlfs_operations();
    fuse_session* session = fuse_session_new(&args, &operations, sizeof(operations), &filesystem);
    if (!session)
        return 1;

    fuse_set_signal_handlers(session);
    int result = 1;
    // With FSKit, a mount failure may only be reported after this returns, in the log output
    if (fuse_session_mount(session, mountpoint) == 0) {
        std::fprintf(stderr,
                     "sqlfs: serving %s at %s (Ctrl+C or umount to stop)\n",
                     database_path,
                     mountpoint);
        result = fuse_session_loop(session);
        fuse_session_unmount(session);
    }
    fuse_remove_signal_handlers(session);
    fuse_session_destroy(session);

    if (!filesystem.sync())
        result = 1;
    return result == 0 ? 0 : 1;
}
