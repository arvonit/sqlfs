#include "core/filesystem.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <unistd.h>

constexpr size_t MAX_NAME_LENGTH = 255;
constexpr uint32_t PERMISSION_BITS = 07777;

static int64_t now_ns() {
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

static const char* kind_to_text(FileKind kind) {
    switch (kind) {
    case FileKind::File:
        return "file";
    case FileKind::Folder:
        return "folder";
    case FileKind::Symlink:
        return "symlink";
    }
    return "file";
}

static FileKind kind_from_text(const std::string& text) {
    if (text == "folder")
        return FileKind::Folder;
    if (text == "symlink")
        return FileKind::Symlink;
    return FileKind::File;
}

static void bind_optional(Statement& statement, int index, std::optional<int64_t> value) {
    if (value)
        statement.bind(index, *value);
    else
        statement.bind_null(index);
}

static FSResult<FileAttributes> load_attributes(Database& database, uint64_t id) {
    Statement select(database,
                     "SELECT kind, permissions, owner_uid, group_gid, name_count, size_bytes, "
                     "created_ns, modified_ns, changed_ns, accessed_ns "
                     "FROM files WHERE file_id = ?");
    select.bind(1, (int64_t)id);
    auto found = select.step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(ENOENT);

    FileAttributes attributes;
    attributes.id = id;
    attributes.kind = kind_from_text(select.column_text(0));
    attributes.permissions = (uint32_t)select.column_int(1);
    attributes.uid = (uint32_t)select.column_int(2);
    attributes.gid = (uint32_t)select.column_int(3);
    attributes.name_count = (uint32_t)select.column_int(4);
    attributes.size = (uint64_t)select.column_int(5);
    attributes.created_ns = select.column_int(6);
    attributes.modified_ns = select.column_int(7);
    attributes.changed_ns = select.column_int(8);
    attributes.accessed_ns = select.column_int(9);
    return attributes;
}

static FSResult<FileAttributes> require_folder(Database& database, uint64_t id) {
    auto folder = load_attributes(database, id);
    if (!folder)
        return folder;
    if (folder->kind != FileKind::Folder)
        return std::unexpected(ENOTDIR);
    return folder;
}

static FSResult<uint64_t> find_child(Database& database, uint64_t folder_id,
                                     const std::string& name) {
    Statement select(database,
                     "SELECT file_id FROM folder_contents WHERE folder_id = ? AND name = ?");
    select.bind(1, (int64_t)folder_id);
    select.bind(2, name);
    auto found = select.step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(ENOENT);
    return (uint64_t)select.column_int(0);
}

static FSResult<bool> is_empty_folder(Database& database, uint64_t id) {
    Statement select(database, "SELECT 1 FROM folder_contents WHERE folder_id = ? LIMIT 1");
    select.bind(1, (int64_t)id);
    auto has_child = select.step();
    if (!has_child)
        return std::unexpected(has_child.error());
    return !*has_child;
}

// Walks up from folder_id toward the root, checking whether it passes through ancestor_id.
// Moving a folder inside itself would cut it off from the tree, so rename must refuse that
static FSResult<bool> is_inside(Database& database, uint64_t folder_id, uint64_t ancestor_id) {
    uint64_t current = folder_id;
    while (current != ROOT_FILE_ID) {
        if (current == ancestor_id)
            return true;
        Statement select(database, "SELECT folder_id FROM folder_contents WHERE file_id = ?");
        select.bind(1, (int64_t)current);
        auto found = select.step();
        if (!found)
            return std::unexpected(found.error());
        if (!*found)
            return false;
        current = (uint64_t)select.column_int(0);
    }
    return false;
}

// Records that a folder's contents changed. name_count_change is +1 or -1 when a subfolder is
// added or removed, because a folder's name_count is 2 + its number of subfolders
static FSResult<void> touch_folder(Database& database, uint64_t folder_id, int name_count_change) {
    Statement update(database,
                     "UPDATE files SET name_count = name_count + ?, modified_ns = ?, "
                     "changed_ns = ? WHERE file_id = ?");
    int64_t now = now_ns();
    update.bind(1, (int64_t)name_count_change);
    update.bind(2, now);
    update.bind(3, now);
    update.bind(4, (int64_t)folder_id);
    return update.run();
}

// Removes one name. The file itself stays until delete_if_unused() finds it unused
static FSResult<void> remove_name(Database& database, uint64_t folder_id, const std::string& name,
                                  const FileAttributes& file) {
    Statement remove(database, "DELETE FROM folder_contents WHERE folder_id = ? AND name = ?");
    remove.bind(1, (int64_t)folder_id);
    remove.bind(2, name);
    auto removed = remove.run();
    if (!removed)
        return removed;

    bool is_folder = file.kind == FileKind::Folder;
    // A removed folder has no names left at all, including its own "." entry
    Statement update(database,
                     is_folder ? "UPDATE files SET name_count = 0, changed_ns = ? "
                                 "WHERE file_id = ?"
                               : "UPDATE files SET name_count = name_count - 1, changed_ns = ? "
                                 "WHERE file_id = ?");
    update.bind(1, now_ns());
    update.bind(2, (int64_t)file.id);
    auto updated = update.run();
    if (!updated)
        return updated;

    return touch_folder(database, folder_id, is_folder ? -1 : 0);
}

static FSResult<std::string> load_contents(Database& database, uint64_t id) {
    Statement select(database, "SELECT kind, contents FROM files WHERE file_id = ?");
    select.bind(1, (int64_t)id);
    auto found = select.step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(ENOENT);

    FileKind kind = kind_from_text(select.column_text(0));
    if (kind == FileKind::Folder)
        return std::unexpected(EISDIR);
    if (kind == FileKind::Symlink)
        return std::unexpected(EINVAL);
    return select.column_blob(1);
}

static FSResult<void> save_contents(Database& database, uint64_t id, const std::string& contents) {
    Statement update(database,
                     "UPDATE files SET contents = ?, size_bytes = ?, modified_ns = ?, "
                     "changed_ns = ? WHERE file_id = ?");
    int64_t now = now_ns();
    update.bind_blob(1, contents);
    update.bind(2, (int64_t)contents.size());
    update.bind(3, now);
    update.bind(4, now);
    update.bind(5, (int64_t)id);
    return update.run();
}

FSResult<void> Filesystem::open_database(const std::string& path) {
    auto opened = database.open(path);
    if (!opened)
        return opened;

    Statement create_root(database,
                          "INSERT OR IGNORE INTO files (file_id, kind, permissions, owner_uid, "
                          "group_gid, name_count, created_ns, modified_ns, changed_ns, "
                          "accessed_ns) VALUES (?, 'folder', ?, ?, ?, 2, ?, ?, ?, ?)");
    int64_t now = now_ns();
    create_root.bind(1, (int64_t)ROOT_FILE_ID);
    create_root.bind(2, (int64_t)0755);
    create_root.bind(3, (int64_t)getuid());
    create_root.bind(4, (int64_t)getgid());
    for (int index = 5; index <= 8; index++)
        create_root.bind(index, now);
    auto created = create_root.run();
    if (!created)
        return created;

    // Files with no names that were still open when sqlfs last stopped (or crashed)
    return database.execute("DELETE FROM files WHERE name_count = 0");
}

FSResult<FileAttributes> Filesystem::lookup(uint64_t folder_id, const std::string& name) {
    auto child_id = find_child(database, folder_id, name);
    if (!child_id)
        return std::unexpected(child_id.error());
    auto attributes = load_attributes(database, *child_id);
    if (attributes)
        open_counts[*child_id].lookups++;
    return attributes;
}

void Filesystem::forget(uint64_t id, uint64_t count) {
    OpenCounts& counts = open_counts[id];
    counts.lookups -= std::min(count, counts.lookups);
    delete_if_unused(id);
}

FSResult<FileAttributes> Filesystem::get_attributes(uint64_t id) {
    return load_attributes(database, id);
}

FSResult<FileAttributes> Filesystem::set_attributes(uint64_t id, const AttributeChanges& changes) {
    Transaction transaction(database);

    if (changes.size) {
        auto contents = load_contents(database, id);
        if (!contents)
            return std::unexpected(contents.error());
        contents->resize(*changes.size, '\0');
        auto saved = save_contents(database, id, *contents);
        if (!saved)
            return std::unexpected(saved.error());
    }

    // coalesce(?, column) keeps the current value when the parameter is NULL (not changing)
    Statement update(database,
                     "UPDATE files SET permissions = coalesce(?, permissions), "
                     "owner_uid = coalesce(?, owner_uid), group_gid = coalesce(?, group_gid), "
                     "created_ns = coalesce(?, created_ns), "
                     "modified_ns = coalesce(?, modified_ns), "
                     "accessed_ns = coalesce(?, accessed_ns), changed_ns = ? "
                     "WHERE file_id = ?");
    std::optional<int64_t> permissions;
    if (changes.permissions)
        permissions = *changes.permissions & PERMISSION_BITS;
    bind_optional(update, 1, permissions);
    bind_optional(update, 2, changes.uid);
    bind_optional(update, 3, changes.gid);
    bind_optional(update, 4, changes.created_ns);
    bind_optional(update, 5, changes.modified_ns);
    bind_optional(update, 6, changes.accessed_ns);
    update.bind(7, now_ns());
    update.bind(8, (int64_t)id);
    auto updated = update.run();
    if (!updated)
        return std::unexpected(updated.error());

    auto committed = transaction.commit();
    if (!committed)
        return std::unexpected(committed.error());
    return load_attributes(database, id);
}

FSResult<std::vector<FolderEntry>> Filesystem::list_folder(uint64_t id) {
    auto folder = require_folder(database, id);
    if (!folder)
        return std::unexpected(folder.error());

    Statement select(database,
                     "SELECT entry.name, entry.file_id, file.kind "
                     "FROM folder_contents AS entry JOIN files AS file USING (file_id) "
                     "WHERE entry.folder_id = ? ORDER BY entry.name");
    select.bind(1, (int64_t)id);

    std::vector<FolderEntry> entries;
    while (true) {
        auto has_row = select.step();
        if (!has_row)
            return std::unexpected(has_row.error());
        if (!*has_row)
            break;
        FolderEntry entry;
        entry.name = select.column_text(0);
        entry.id = (uint64_t)select.column_int(1);
        entry.kind = kind_from_text(select.column_text(2));
        entries.push_back(entry);
    }
    return entries;
}

FSResult<std::string> Filesystem::read(uint64_t id, uint64_t offset, uint64_t size) {
    // substr() works on BLOBs and counts from 1, so only the requested bytes leave SQLite
    Statement select(database, "SELECT kind, substr(contents, ?, ?) FROM files WHERE file_id = ?");
    select.bind(1, (int64_t)offset + 1);
    select.bind(2, (int64_t)size);
    select.bind(3, (int64_t)id);
    auto found = select.step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(ENOENT);
    if (kind_from_text(select.column_text(0)) == FileKind::Folder) {
        return std::unexpected(EISDIR);
    }
    return select.column_blob(1);
}

FSResult<size_t> Filesystem::write(uint64_t id, uint64_t offset, std::string_view data) {
    Transaction transaction(database);
    auto contents = load_contents(database, id);
    if (!contents)
        return std::unexpected(contents.error());

    // Writing past the end leaves a gap, which reads back as zeros
    if (offset + data.size() > contents->size())
        contents->resize(offset + data.size(), '\0');
    contents->replace(offset, data.size(), data);

    auto saved = save_contents(database, id, *contents);
    if (!saved)
        return std::unexpected(saved.error());
    auto committed = transaction.commit();
    if (!committed)
        return std::unexpected(committed.error());
    return data.size();
}

FSResult<std::string> Filesystem::read_symlink(uint64_t id) {
    Statement select(database, "SELECT kind, symlink_target FROM files WHERE file_id = ?");
    select.bind(1, (int64_t)id);
    auto found = select.step();
    if (!found)
        return std::unexpected(found.error());
    if (!*found)
        return std::unexpected(ENOENT);
    if (kind_from_text(select.column_text(0)) != FileKind::Symlink) {
        return std::unexpected(EINVAL);
    }
    return select.column_text(1);
}

FSResult<FileAttributes> Filesystem::create_file(uint64_t folder_id, const std::string& name,
                                                 uint32_t permissions, Owner owner) {
    return add_file(folder_id, name, FileKind::File, permissions, owner, "");
}

FSResult<FileAttributes> Filesystem::make_folder(uint64_t folder_id, const std::string& name,
                                                 uint32_t permissions, Owner owner) {
    return add_file(folder_id, name, FileKind::Folder, permissions, owner, "");
}

FSResult<FileAttributes> Filesystem::make_symlink(uint64_t folder_id, const std::string& name,
                                                  const std::string& target, Owner owner) {
    return add_file(folder_id, name, FileKind::Symlink, 0755, owner, target);
}

FSResult<FileAttributes> Filesystem::make_hard_link(uint64_t id, uint64_t folder_id,
                                                    const std::string& name) {
    if (name.size() > MAX_NAME_LENGTH)
        return std::unexpected(ENAMETOOLONG);
    auto file = load_attributes(database, id);
    if (!file)
        return file;
    if (file->kind == FileKind::Folder)
        return std::unexpected(EPERM);
    auto folder = require_folder(database, folder_id);
    if (!folder)
        return folder;
    if (find_child(database, folder_id, name))
        return std::unexpected(EEXIST);

    Transaction transaction(database);
    Statement insert(database,
                     "INSERT INTO folder_contents (folder_id, name, file_id) VALUES (?, ?, ?)");
    insert.bind(1, (int64_t)folder_id);
    insert.bind(2, name);
    insert.bind(3, (int64_t)id);
    auto inserted = insert.run();
    if (!inserted)
        return std::unexpected(inserted.error());

    Statement update(database,
                     "UPDATE files SET name_count = name_count + 1, changed_ns = ? "
                     "WHERE file_id = ?");
    update.bind(1, now_ns());
    update.bind(2, (int64_t)id);
    auto updated = update.run();
    if (!updated)
        return std::unexpected(updated.error());

    auto touched = touch_folder(database, folder_id, 0);
    if (!touched)
        return std::unexpected(touched.error());
    auto committed = transaction.commit();
    if (!committed)
        return std::unexpected(committed.error());

    open_counts[id].lookups++;
    return load_attributes(database, id);
}

FSResult<void> Filesystem::remove_file(uint64_t folder_id, const std::string& name) {
    auto child_id = find_child(database, folder_id, name);
    if (!child_id)
        return std::unexpected(child_id.error());
    auto file = load_attributes(database, *child_id);
    if (!file)
        return std::unexpected(file.error());
    if (file->kind == FileKind::Folder)
        return std::unexpected(EPERM);

    Transaction transaction(database);
    auto removed = remove_name(database, folder_id, name, *file);
    if (!removed)
        return removed;
    auto committed = transaction.commit();
    if (!committed)
        return committed;

    delete_if_unused(file->id);
    return {};
}

FSResult<void> Filesystem::remove_folder(uint64_t folder_id, const std::string& name) {
    auto child_id = find_child(database, folder_id, name);
    if (!child_id)
        return std::unexpected(child_id.error());
    auto folder = require_folder(database, *child_id);
    if (!folder)
        return std::unexpected(folder.error());
    auto empty = is_empty_folder(database, folder->id);
    if (!empty)
        return std::unexpected(empty.error());
    if (!*empty)
        return std::unexpected(ENOTEMPTY);

    Transaction transaction(database);
    auto removed = remove_name(database, folder_id, name, *folder);
    if (!removed)
        return removed;
    auto committed = transaction.commit();
    if (!committed)
        return committed;

    delete_if_unused(folder->id);
    return {};
}

FSResult<void> Filesystem::rename(uint64_t old_folder_id, const std::string& old_name,
                                  uint64_t new_folder_id, const std::string& new_name,
                                  bool replace_existing) {
    if (new_name.size() > MAX_NAME_LENGTH)
        return std::unexpected(ENAMETOOLONG);
    auto source_id = find_child(database, old_folder_id, old_name);
    if (!source_id)
        return std::unexpected(source_id.error());
    auto source = load_attributes(database, *source_id);
    if (!source)
        return std::unexpected(source.error());
    auto new_folder = require_folder(database, new_folder_id);
    if (!new_folder)
        return std::unexpected(new_folder.error());

    bool moving_folder = source->kind == FileKind::Folder;
    if (moving_folder) {
        auto inside = is_inside(database, new_folder_id, source->id);
        if (!inside)
            return std::unexpected(inside.error());
        if (*inside)
            return std::unexpected(EINVAL);
    }

    Transaction transaction(database);

    std::optional<FileAttributes> replaced;
    auto target_id = find_child(database, new_folder_id, new_name);
    if (target_id) {
        if (!replace_existing)
            return std::unexpected(EEXIST);
        // Both names already point at the same file, so POSIX says there is nothing to do
        if (*target_id == source->id)
            return {};

        auto target = load_attributes(database, *target_id);
        if (!target)
            return std::unexpected(target.error());
        bool target_is_folder = target->kind == FileKind::Folder;
        if (moving_folder && !target_is_folder)
            return std::unexpected(ENOTDIR);
        if (!moving_folder && target_is_folder)
            return std::unexpected(EISDIR);
        if (target_is_folder) {
            auto empty = is_empty_folder(database, target->id);
            if (!empty)
                return std::unexpected(empty.error());
            if (!*empty)
                return std::unexpected(ENOTEMPTY);
        }
        auto removed = remove_name(database, new_folder_id, new_name, *target);
        if (!removed)
            return removed;
        replaced = *target;
    } else if (target_id.error() != ENOENT) {
        return std::unexpected(target_id.error());
    }

    Statement move(database,
                   "UPDATE folder_contents SET folder_id = ?, name = ? "
                   "WHERE folder_id = ? AND name = ?");
    move.bind(1, (int64_t)new_folder_id);
    move.bind(2, new_name);
    move.bind(3, (int64_t)old_folder_id);
    move.bind(4, old_name);
    auto moved = move.run();
    if (!moved)
        return moved;

    Statement update(database, "UPDATE files SET changed_ns = ? WHERE file_id = ?");
    update.bind(1, now_ns());
    update.bind(2, (int64_t)source->id);
    auto updated = update.run();
    if (!updated)
        return updated;

    int subfolder_change = (moving_folder && old_folder_id != new_folder_id) ? 1 : 0;
    auto touched_old = touch_folder(database, old_folder_id, -subfolder_change);
    if (!touched_old)
        return touched_old;
    auto touched_new = touch_folder(database, new_folder_id, subfolder_change);
    if (!touched_new)
        return touched_new;

    auto committed = transaction.commit();
    if (!committed)
        return committed;

    if (replaced)
        delete_if_unused(replaced->id);
    return {};
}

FSResult<void> Filesystem::open(uint64_t id) {
    auto file = load_attributes(database, id);
    if (!file)
        return std::unexpected(file.error());
    if (file->kind == FileKind::Folder)
        return std::unexpected(EISDIR);
    open_counts[id].open_handles++;
    return {};
}

void Filesystem::release(uint64_t id) {
    OpenCounts& counts = open_counts[id];
    if (counts.open_handles > 0)
        counts.open_handles--;
    delete_if_unused(id);
}

FSResult<void> Filesystem::sync() {
    // With synchronous = NORMAL, commits may sit unsynced in the WAL file. A full checkpoint
    // flushes them into the main database file and syncs it to disk
    return database.execute("PRAGMA wal_checkpoint(FULL)");
}

FSResult<FileAttributes> Filesystem::add_file(uint64_t folder_id, const std::string& name,
                                              FileKind kind, uint32_t permissions, Owner owner,
                                              const std::string& symlink_target) {
    if (name.size() > MAX_NAME_LENGTH)
        return std::unexpected(ENAMETOOLONG);
    auto folder = require_folder(database, folder_id);
    if (!folder)
        return folder;
    if (find_child(database, folder_id, name))
        return std::unexpected(EEXIST);

    Transaction transaction(database);
    Statement insert(database,
                     "INSERT INTO files (kind, permissions, owner_uid, group_gid, name_count, "
                     "size_bytes, contents, symlink_target, created_ns, modified_ns, "
                     "changed_ns, accessed_ns) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    int64_t now = now_ns();
    insert.bind(1, kind_to_text(kind));
    insert.bind(2, (int64_t)(permissions & PERMISSION_BITS));
    insert.bind(3, (int64_t)owner.uid);
    insert.bind(4, (int64_t)owner.gid);
    insert.bind(5, (int64_t)(kind == FileKind::Folder ? 2 : 1));
    insert.bind(6, (int64_t)symlink_target.size());
    if (kind == FileKind::File)
        insert.bind_blob(7, "");
    else
        insert.bind_null(7);
    if (kind == FileKind::Symlink)
        insert.bind(8, symlink_target);
    else
        insert.bind_null(8);
    for (int index = 9; index <= 12; index++)
        insert.bind(index, now);
    auto inserted = insert.run();
    if (!inserted)
        return std::unexpected(inserted.error());
    uint64_t id = (uint64_t)database.last_insert_id();

    Statement add_name(database,
                       "INSERT INTO folder_contents (folder_id, name, file_id) VALUES (?, ?, ?)");
    add_name.bind(1, (int64_t)folder_id);
    add_name.bind(2, name);
    add_name.bind(3, (int64_t)id);
    auto named = add_name.run();
    if (!named)
        return std::unexpected(named.error());

    auto touched = touch_folder(database, folder_id, kind == FileKind::Folder ? 1 : 0);
    if (!touched)
        return std::unexpected(touched.error());
    auto committed = transaction.commit();
    if (!committed)
        return std::unexpected(committed.error());

    open_counts[id].lookups++;
    return load_attributes(database, id);
}

// Deletes a file's row once nothing refers to it: no names (checked in SQL), no lookups the
// kernel still remembers, and no open handles
void Filesystem::delete_if_unused(uint64_t id) {
    if (id == ROOT_FILE_ID)
        return;
    OpenCounts& counts = open_counts[id];
    if (counts.lookups > 0 || counts.open_handles > 0)
        return;
    open_counts.erase(id);

    Statement remove(database, "DELETE FROM files WHERE file_id = ? AND name_count = 0");
    remove.bind(1, (int64_t)id);
    auto removed = remove.run();
    if (!removed)
        std::fprintf(stderr, "sqlfs: could not delete unused file %llu\n", id);
}
