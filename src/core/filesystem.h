#pragma once

#include "core/file_attributes.h"
#include "core/fs_result.h"
#include "db/database.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

constexpr uint64_t ROOT_FILE_ID = 1;

// All filesystem logic, with no FUSE types. Each public method matches one FUSE operation.
//
// Lifetimes: the kernel remembers file IDs it got from us (a "lookup") and tells us when it
// forgets them. A file whose last name was removed is only deleted once the kernel has
// forgotten it and no program has it open, which is how POSIX lets you read a deleted file
// that you still have open.
class Filesystem {
public:
    FSResult<void> open_database(const std::string& path);

    // Methods that hand a file to the kernel (lookup and the create/make methods) count as a
    // lookup, so they must be balanced by forget()
    FSResult<FileAttributes> lookup(uint64_t folder_id, const std::string& name);
    void forget(uint64_t id, uint64_t count);

    FSResult<FileAttributes> get_attributes(uint64_t id);
    FSResult<FileAttributes> set_attributes(uint64_t id, const AttributeChanges& changes);
    FSResult<std::vector<FolderEntry>> list_folder(uint64_t id);
    FSResult<std::string> read(uint64_t id, uint64_t offset, uint64_t size);
    FSResult<size_t> write(uint64_t id, uint64_t offset, std::string_view data);
    FSResult<std::string> read_symlink(uint64_t id);

    FSResult<FileAttributes> create_file(uint64_t folder_id, const std::string& name,
                                         uint32_t permissions, Owner owner);
    FSResult<FileAttributes> make_folder(uint64_t folder_id, const std::string& name,
                                         uint32_t permissions, Owner owner);
    FSResult<FileAttributes> make_symlink(uint64_t folder_id, const std::string& name,
                                          const std::string& target, Owner owner);
    FSResult<FileAttributes> make_hard_link(uint64_t id, uint64_t folder_id,
                                            const std::string& name);

    FSResult<void> remove_file(uint64_t folder_id, const std::string& name);
    FSResult<void> remove_folder(uint64_t folder_id, const std::string& name);
    FSResult<void> rename(uint64_t old_folder_id, const std::string& old_name,
                          uint64_t new_folder_id, const std::string& new_name,
                          bool replace_existing);

    FSResult<void> open(uint64_t id);
    void release(uint64_t id);
    FSResult<void> sync();

    // Extended attributes. flags may contain XATTR_CREATE or XATTR_REPLACE
    FSResult<void> set_xattr(uint64_t id, const std::string& name, std::string_view value,
                             int flags);
    FSResult<std::string> get_xattr(uint64_t id, const std::string& name);
    FSResult<std::vector<std::string>> list_xattrs(uint64_t id);
    FSResult<void> remove_xattr(uint64_t id, const std::string& name);

    FSResult<StorageUsage> get_usage();

private:
    struct OpenCounts {
        uint64_t lookups = 0;
        uint64_t open_handles = 0;
    };

    FSResult<FileAttributes> add_file(uint64_t folder_id, const std::string& name, FileKind kind,
                                      uint32_t permissions, Owner owner,
                                      const std::string& symlink_target);
    void delete_if_unused(uint64_t id);

    Database database;
    std::string database_path;
    std::map<uint64_t, OpenCounts> open_counts;
};
