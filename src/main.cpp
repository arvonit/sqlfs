#include "core/filesystem.h"
#include "fuse/adapter.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mount.h>
#include <thread>

// With FSKit, fuse_session_loop waits for messages in a way that signals don't interrupt, so
// libfuse's own Ctrl+C handling never stops it. Instead, this thread waits for Ctrl+C and
// unmounts the volume, which makes the loop return
static void unmount_on_signal(sigset_t signals, std::string mountpoint) {
    while (true) {
        int signal = 0;
        sigwait(&signals, &signal);
        if (unmount(mountpoint.c_str(), 0) == 0)
            return;
        if (errno == EBUSY)
            std::fprintf(stderr,
                         "\nsqlfs: %s is busy. Close shells and Finder windows using it and "
                         "press Ctrl+C again, or force it with: umount -f %s\n",
                         mountpoint.c_str(),
                         mountpoint.c_str());
        else
            std::fprintf(stderr,
                         "\nsqlfs: cannot unmount %s: %s\n",
                         mountpoint.c_str(),
                         strerror(errno));
    }
}

int main(int argc, char* argv[]) {
    // Block the stop signals before any threads start, so only unmount_on_signal receives them
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

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

    int result = 1;
    // With FSKit, a mount failure may only be reported after this returns, in the log output
    if (fuse_session_mount(session, mountpoint) == 0) {
        std::fprintf(stderr,
                     "sqlfs: serving %s at %s (Ctrl+C or umount to stop)\n",
                     database_path,
                     mountpoint);
        std::thread(unmount_on_signal, stop_signals, std::string(mountpoint)).detach();
        result = fuse_session_loop(session);
        fuse_session_unmount(session);
    }
    fuse_session_destroy(session);

    if (!filesystem.sync())
        result = 1;
    return result == 0 ? 0 : 1;
}
