#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class FileKind { File, Folder, Symlink };

struct FileAttributes {
    uint64_t id = 0;
    FileKind kind = FileKind::File;
    uint32_t permissions = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t name_count = 0;
    uint64_t size = 0;
    int64_t created_ns = 0;
    int64_t modified_ns = 0;
    int64_t changed_ns = 0;
    int64_t accessed_ns = 0;
};

// Only the fields that are set get changed
struct AttributeChanges {
    std::optional<uint32_t> permissions;
    std::optional<uint32_t> uid;
    std::optional<uint32_t> gid;
    std::optional<uint64_t> size;
    std::optional<int64_t> created_ns;
    std::optional<int64_t> modified_ns;
    std::optional<int64_t> accessed_ns;
};

struct FolderEntry {
    std::string name;
    uint64_t id = 0;
    FileKind kind = FileKind::File;
};

// Space on the disk that holds the database, which is the space sqlfs can grow into
struct StorageUsage {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t available_bytes = 0; // free space usable without root
    uint64_t file_count = 0;
    uint64_t free_file_slots = 0;
};

// The user creating a new file becomes its owner
struct Owner {
    uint32_t uid = 0;
    uint32_t gid = 0;
};
