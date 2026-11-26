// Translates libfuse's C callbacks into Filesystem calls. Every callback catches all exceptions,
// because a C++ exception must never unwind through libfuse's C code
#include "fuse/adapter.h"

#include "core/filesystem.h"

#include <cerrno>
#include <ctime>
#include <string>
#include <sys/mount.h>
#include <vector>

// How long the kernel may cache names and attributes. Kept short because FSKit does not
// support the notify calls that would let us invalidate its cache early
constexpr double CACHE_SECONDS = 1.0;

static Filesystem& filesystem(fuse_req_t req) {
    return *static_cast<Filesystem*>(fuse_req_userdata(req));
}

static Owner caller(fuse_req_t req) {
    const fuse_ctx* context = fuse_req_ctx(req);
    return Owner{context->uid, context->gid};
}

static timespec to_timespec(int64_t ns) {
    return timespec{(time_t)(ns / 1'000'000'000), (long)(ns % 1'000'000'000)};
}

static int64_t to_ns(const timespec& time) {
    return time.tv_sec * 1'000'000'000LL + time.tv_nsec;
}

static int64_t now_ns() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return to_ns(now);
}

static mode_t kind_bits(FileKind kind) {
    switch (kind) {
    case FileKind::Folder:
        return S_IFDIR;
    case FileKind::Symlink:
        return S_IFLNK;
    case FileKind::File:
        return S_IFREG;
    }
    return S_IFREG;
}

static fuse_darwin_attr to_darwin_attr(const FileAttributes& attributes) {
    fuse_darwin_attr attr{};
    attr.ino = attributes.id;
    attr.mode = kind_bits(attributes.kind) | attributes.permissions;
    attr.nlink = attributes.name_count;
    attr.uid = attributes.uid;
    attr.gid = attributes.gid;
    attr.size = attributes.size;
    attr.blksize = 4096;
    attr.blocks = (attributes.size + 511) / 512;
    attr.btimespec = to_timespec(attributes.created_ns);
    attr.mtimespec = to_timespec(attributes.modified_ns);
    attr.ctimespec = to_timespec(attributes.changed_ns);
    attr.atimespec = to_timespec(attributes.accessed_ns);
    return attr;
}

static fuse_darwin_entry_param to_entry(const FileAttributes& attributes) {
    fuse_darwin_entry_param entry{};
    entry.ino = attributes.id;
    entry.attr = to_darwin_attr(attributes);
    entry.attr_timeout = CACHE_SECONDS;
    entry.entry_timeout = CACHE_SECONDS;
    return entry;
}

static void reply_entry(fuse_req_t req, const FSResult<FileAttributes>& result) {
    if (!result) {
        fuse_reply_err(req, result.error());
        return;
    }
    fuse_darwin_entry_param entry = to_entry(*result);
    fuse_reply_entry(req, &entry);
}

static void reply_status(fuse_req_t req, const FSResult<void>& result) {
    fuse_reply_err(req, result ? 0 : result.error());
}

// The kernel asks for attribute data in two steps: first with size 0 to learn how big it is,
// then again with a buffer of that size
static void reply_xattr_data(fuse_req_t req, const std::string& data, size_t size) {
    if (size == 0)
        fuse_reply_xattr(req, data.size());
    else if (data.size() > size)
        fuse_reply_err(req, ERANGE);
    else
        fuse_reply_buf(req, data.data(), data.size());
}

static void on_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    try {
        reply_entry(req, filesystem(req).lookup(parent, name));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_forget(fuse_req_t req, fuse_ino_t id, uint64_t count) {
    try {
        filesystem(req).forget(id, count);
    } catch (...) {
    }
    fuse_reply_none(req);
}

static void on_forget_multi(fuse_req_t req, size_t count, fuse_forget_data* forgets) {
    try {
        for (size_t i = 0; i < count; i++) {
            filesystem(req).forget(forgets[i].ino, forgets[i].nlookup);
        }
    } catch (...) {
    }
    fuse_reply_none(req);
}

static void on_getattr(fuse_req_t req, fuse_ino_t id, fuse_file_info*) {
    try {
        auto attributes = filesystem(req).get_attributes(id);
        if (!attributes) {
            fuse_reply_err(req, attributes.error());
            return;
        }
        fuse_darwin_attr attr = to_darwin_attr(*attributes);
        fuse_reply_attr(req, &attr, CACHE_SECONDS);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_setattr(fuse_req_t req, fuse_ino_t id, fuse_darwin_attr* attr, int to_set,
                       fuse_file_info*) {
    try {
        AttributeChanges changes;
        if (to_set & FUSE_SET_ATTR_MODE)
            changes.permissions = attr->mode;
        if (to_set & FUSE_SET_ATTR_UID)
            changes.uid = attr->uid;
        if (to_set & FUSE_SET_ATTR_GID)
            changes.gid = attr->gid;
        if (to_set & FUSE_SET_ATTR_SIZE)
            changes.size = attr->size;
        if (to_set & FUSE_SET_ATTR_ATIME)
            changes.accessed_ns = to_ns(attr->atimespec);
        if (to_set & FUSE_SET_ATTR_ATIME_NOW)
            changes.accessed_ns = now_ns();
        if (to_set & FUSE_SET_ATTR_MTIME)
            changes.modified_ns = to_ns(attr->mtimespec);
        if (to_set & FUSE_SET_ATTR_MTIME_NOW)
            changes.modified_ns = now_ns();
        if (to_set & FUSE_SET_ATTR_BTIME)
            changes.created_ns = to_ns(attr->btimespec);

        auto attributes = filesystem(req).set_attributes(id, changes);
        if (!attributes) {
            fuse_reply_err(req, attributes.error());
            return;
        }
        fuse_darwin_attr updated = to_darwin_attr(*attributes);
        fuse_reply_attr(req, &updated, CACHE_SECONDS);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_readlink(fuse_req_t req, fuse_ino_t id) {
    try {
        auto target = filesystem(req).read_symlink(id);
        if (!target)
            fuse_reply_err(req, target.error());
        else
            fuse_reply_readlink(req, target->c_str());
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
    try {
        reply_entry(req, filesystem(req).make_folder(parent, name, mode, caller(req)));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
    try {
        reply_status(req, filesystem(req).remove_file(parent, name));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
    try {
        reply_status(req, filesystem(req).remove_folder(parent, name));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_symlink(fuse_req_t req, const char* target, fuse_ino_t parent, const char* name) {
    try {
        reply_entry(req, filesystem(req).make_symlink(parent, name, target, caller(req)));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_rename(fuse_req_t req, fuse_ino_t parent, const char* name, fuse_ino_t new_parent,
                      const char* new_name, unsigned int flags) {
    try {
        // Swapping two names atomically (RENAME_EXCHANGE) is not supported
        if (flags & RENAME_EXCHANGE) {
            fuse_reply_err(req, EINVAL);
            return;
        }
        bool replace_existing = !(flags & RENAME_NOREPLACE);
        reply_status(req,
                     filesystem(req).rename(parent, name, new_parent, new_name, replace_existing));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_link(fuse_req_t req, fuse_ino_t id, fuse_ino_t new_parent, const char* new_name) {
    try {
        reply_entry(req, filesystem(req).make_hard_link(id, new_parent, new_name));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_open(fuse_req_t req, fuse_ino_t id, fuse_file_info* info) {
    try {
        auto opened = filesystem(req).open(id);
        if (!opened)
            fuse_reply_err(req, opened.error());
        else
            fuse_reply_open(req, info);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_create(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode,
                      fuse_file_info* info) {
    try {
        Filesystem& fs = filesystem(req);
        auto created = fs.create_file(parent, name, mode, caller(req));
        if (!created) {
            fuse_reply_err(req, created.error());
            return;
        }
        auto opened = fs.open(created->id);
        if (!opened) {
            fuse_reply_err(req, opened.error());
            return;
        }
        fuse_darwin_entry_param entry = to_entry(*created);
        fuse_reply_create(req, &entry, info);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_read(fuse_req_t req, fuse_ino_t id, size_t size, off_t offset, fuse_file_info*) {
    try {
        auto data = filesystem(req).read(id, offset, size);
        if (!data)
            fuse_reply_err(req, data.error());
        else
            fuse_reply_buf(req, data->data(), data->size());
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_write(fuse_req_t req, fuse_ino_t id, const char* buffer, size_t size, off_t offset,
                     fuse_file_info*) {
    try {
        auto written = filesystem(req).write(id, offset, std::string_view(buffer, size));
        if (!written)
            fuse_reply_err(req, written.error());
        else
            fuse_reply_write(req, *written);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_flush(fuse_req_t req, fuse_ino_t, fuse_file_info*) {
    // Every write is already committed to SQLite, so there is nothing to flush
    fuse_reply_err(req, 0);
}

static void on_release(fuse_req_t req, fuse_ino_t id, fuse_file_info*) {
    try {
        filesystem(req).release(id);
    } catch (...) {
    }
    fuse_reply_err(req, 0);
}

static void on_fsync(fuse_req_t req, fuse_ino_t, int, fuse_file_info*) {
    try {
        reply_status(req, filesystem(req).sync());
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_opendir(fuse_req_t req, fuse_ino_t id, fuse_file_info* info) {
    try {
        auto folder = filesystem(req).get_attributes(id);
        if (!folder)
            fuse_reply_err(req, folder.error());
        else if (folder->kind != FileKind::Folder)
            fuse_reply_err(req, ENOTDIR);
        else
            fuse_reply_open(req, info);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

// The kernel may call readdir several times with a growing offset until we return nothing.
// Our offset is simply the position in the sorted list of entries
static void on_readdir(fuse_req_t req, fuse_ino_t id, size_t size, off_t offset, fuse_file_info*) {
    try {
        auto children = filesystem(req).list_folder(id);
        if (!children) {
            fuse_reply_err(req, children.error());
            return;
        }
        // The kernel ignores the ID we give for "..", so the folder's own ID is fine there
        std::vector<FolderEntry> entries = {{".", id, FileKind::Folder},
                                            {"..", id, FileKind::Folder}};
        entries.insert(entries.end(), children->begin(), children->end());

        std::string buffer(size, '\0');
        size_t used = 0;
        for (size_t i = offset; i < entries.size(); i++) {
            fuse_darwin_attr attr{};
            attr.ino = entries[i].id;
            attr.mode = kind_bits(entries[i].kind);
            size_t needed = fuse_add_direntry(req,
                                              buffer.data() + used,
                                              size - used,
                                              entries[i].name.c_str(),
                                              &attr,
                                              i + 1);
            if (needed > size - used)
                break;
            used += needed;
        }
        fuse_reply_buf(req, buffer.data(), used);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_releasedir(fuse_req_t req, fuse_ino_t, fuse_file_info*) {
    fuse_reply_err(req, 0);
}

static void on_fsyncdir(fuse_req_t req, fuse_ino_t, int, fuse_file_info*) {
    try {
        reply_status(req, filesystem(req).sync());
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

// position is only used by resource forks (com.apple.ResourceFork), which sqlfs doesn't support
static void on_setxattr(fuse_req_t req, fuse_ino_t id, const char* name, const char* value,
                        size_t size, int flags, uint32_t position) {
    try {
        if (position != 0) {
            fuse_reply_err(req, EINVAL);
            return;
        }
        reply_status(req,
                     filesystem(req).set_xattr(id, name, std::string_view(value, size), flags));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_getxattr(fuse_req_t req, fuse_ino_t id, const char* name, size_t size,
                        uint32_t position) {
    try {
        if (position != 0) {
            fuse_reply_err(req, EINVAL);
            return;
        }
        auto value = filesystem(req).get_xattr(id, name);
        if (!value)
            fuse_reply_err(req, value.error());
        else
            reply_xattr_data(req, *value, size);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

// The list is every name followed by a '\0', all in one buffer
static void on_listxattr(fuse_req_t req, fuse_ino_t id, size_t size) {
    try {
        auto names = filesystem(req).list_xattrs(id);
        if (!names) {
            fuse_reply_err(req, names.error());
            return;
        }
        std::string list;
        for (const std::string& name : *names) {
            list += name;
            list += '\0';
        }
        reply_xattr_data(req, list, size);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_removexattr(fuse_req_t req, fuse_ino_t id, const char* name) {
    try {
        reply_status(req, filesystem(req).remove_xattr(id, name));
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static void on_statfs(fuse_req_t req, fuse_ino_t) {
    try {
        auto usage = filesystem(req).get_usage();
        if (!usage) {
            fuse_reply_err(req, usage.error());
            return;
        }
        // macOS uses BSD's struct statfs here instead of POSIX's struct statvfs
        constexpr uint64_t BLOCK_SIZE = 4096;
        struct statfs stats{};
        stats.f_bsize = BLOCK_SIZE;
        stats.f_iosize = BLOCK_SIZE;
        stats.f_blocks = usage->total_bytes / BLOCK_SIZE;
        stats.f_bfree = usage->free_bytes / BLOCK_SIZE;
        stats.f_bavail = usage->available_bytes / BLOCK_SIZE;
        stats.f_files = usage->file_count + usage->free_file_slots;
        stats.f_ffree = usage->free_file_slots;
        fuse_reply_statfs(req, &stats);
    } catch (...) {
        fuse_reply_err(req, EIO);
    }
}

static fuse_lowlevel_ops make_operations() {
    fuse_lowlevel_ops ops{};
    ops.lookup = on_lookup;
    ops.forget = on_forget;
    ops.forget_multi = on_forget_multi;
    ops.getattr = on_getattr;
    ops.setattr = on_setattr;
    ops.readlink = on_readlink;
    ops.mkdir = on_mkdir;
    ops.unlink = on_unlink;
    ops.rmdir = on_rmdir;
    ops.symlink = on_symlink;
    ops.rename = on_rename;
    ops.link = on_link;
    ops.open = on_open;
    ops.create = on_create;
    ops.read = on_read;
    ops.write = on_write;
    ops.flush = on_flush;
    ops.release = on_release;
    ops.fsync = on_fsync;
    ops.opendir = on_opendir;
    ops.readdir = on_readdir;
    ops.releasedir = on_releasedir;
    ops.fsyncdir = on_fsyncdir;
    ops.setxattr = on_setxattr;
    ops.getxattr = on_getxattr;
    ops.listxattr = on_listxattr;
    ops.removexattr = on_removexattr;
    ops.statfs = on_statfs;
    return ops;
}

const fuse_lowlevel_ops& sqlfs_operations() {
    static const fuse_lowlevel_ops operations = make_operations();
    return operations;
}
