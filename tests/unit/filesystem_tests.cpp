#include "core/filesystem.h"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

const Owner ME{501, 20};

// A Filesystem backed by a fresh database file that is deleted after the test
struct TestFilesystem {
    std::string path;
    std::unique_ptr<Filesystem> fs;

    TestFilesystem() {
        static int counter = 0;
        path = (std::filesystem::temp_directory_path() / ("sqlfs-test-" + std::to_string(getpid()) +
                                                          "-" + std::to_string(counter++) + ".db"))
                   .string();
        reopen();
    }

    ~TestFilesystem() {
        fs.reset();
        for (const char* suffix : {"", "-wal", "-shm"})
            std::filesystem::remove(path + suffix);
    }

    // Simulates unmounting and mounting again
    void reopen() {
        fs = std::make_unique<Filesystem>();
        REQUIRE(fs->open_database(path));
    }

    uint64_t create(uint64_t folder_id, const std::string& name, const std::string& text = "") {
        auto file = fs->create_file(folder_id, name, 0644, ME);
        REQUIRE(file);
        if (!text.empty())
            REQUIRE(fs->write(file->id, 0, text));
        return file->id;
    }

    uint64_t mkdir(uint64_t folder_id, const std::string& name) {
        auto folder = fs->make_folder(folder_id, name, 0755, ME);
        REQUIRE(folder);
        return folder->id;
    }

    std::string read_all(uint64_t id) {
        auto data = fs->read(id, 0, 1 << 20);
        REQUIRE(data);
        return *data;
    }
};

TEST_CASE("a new database has an empty root folder") {
    TestFilesystem t;
    auto root = t.fs->get_attributes(ROOT_FILE_ID);
    REQUIRE(root);
    CHECK(root->kind == FileKind::Folder);
    CHECK(root->name_count == 2);
    CHECK(t.fs->list_folder(ROOT_FILE_ID)->empty());
}

TEST_CASE("files can be created, written, and read back") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "notes.txt", "hello world");

    CHECK(t.read_all(id) == "hello world");
    CHECK(*t.fs->read(id, 6, 3) == "wor");
    CHECK(t.fs->read(id, 100, 10)->empty());
    CHECK(t.fs->get_attributes(id)->size == 11);

    auto found = t.fs->lookup(ROOT_FILE_ID, "notes.txt");
    REQUIRE(found);
    CHECK(found->id == id);
    CHECK(t.fs->lookup(ROOT_FILE_ID, "missing.txt").error() == ENOENT);
}

TEST_CASE("creating a name that already exists fails") {
    TestFilesystem t;
    t.create(ROOT_FILE_ID, "a");
    CHECK(t.fs->create_file(ROOT_FILE_ID, "a", 0644, ME).error() == EEXIST);
    CHECK(t.fs->make_folder(ROOT_FILE_ID, "a", 0755, ME).error() == EEXIST);
}

TEST_CASE("writing past the end fills the gap with zeros") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "sparse", "ab");
    REQUIRE(t.fs->write(id, 5, "xy"));
    CHECK(t.read_all(id) == std::string("ab\0\0\0xy", 7));
}

TEST_CASE("truncate shrinks and grows files") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "f", "hello world");

    AttributeChanges shrink;
    shrink.size = 5;
    REQUIRE(t.fs->set_attributes(id, shrink));
    CHECK(t.read_all(id) == "hello");

    AttributeChanges grow;
    grow.size = 7;
    REQUIRE(t.fs->set_attributes(id, grow));
    CHECK(t.read_all(id) == std::string("hello\0\0", 7));
}

TEST_CASE("chmod and timestamps change only what was asked") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "f");
    auto before = *t.fs->get_attributes(id);

    AttributeChanges changes;
    changes.permissions = 0100600; // file type bits must be ignored
    changes.modified_ns = 42;
    auto after = t.fs->set_attributes(id, changes);
    REQUIRE(after);
    CHECK(after->permissions == 0600);
    CHECK(after->modified_ns == 42);
    CHECK(after->uid == before.uid);
    CHECK(after->accessed_ns == before.accessed_ns);
}

TEST_CASE("folders track subfolders in name_count and list their contents in order") {
    TestFilesystem t;
    uint64_t docs = t.mkdir(ROOT_FILE_ID, "docs");
    t.create(docs, "b.txt");
    t.create(docs, "a.txt");
    t.mkdir(docs, "sub");

    CHECK(t.fs->get_attributes(ROOT_FILE_ID)->name_count == 3);
    CHECK(t.fs->get_attributes(docs)->name_count == 3);

    auto entries = t.fs->list_folder(docs);
    REQUIRE(entries);
    REQUIRE(entries->size() == 3);
    CHECK((*entries)[0].name == "a.txt");
    CHECK((*entries)[1].name == "b.txt");
    CHECK((*entries)[2].kind == FileKind::Folder);
}

TEST_CASE("removing folders") {
    TestFilesystem t;
    uint64_t docs = t.mkdir(ROOT_FILE_ID, "docs");
    t.create(docs, "f");

    CHECK(t.fs->remove_folder(ROOT_FILE_ID, "docs").error() == ENOTEMPTY);
    CHECK(t.fs->remove_file(ROOT_FILE_ID, "docs").error() == EPERM);
    REQUIRE(t.fs->remove_file(docs, "f"));
    REQUIRE(t.fs->remove_folder(ROOT_FILE_ID, "docs"));
    CHECK(t.fs->get_attributes(ROOT_FILE_ID)->name_count == 2);
    CHECK(t.fs->lookup(ROOT_FILE_ID, "docs").error() == ENOENT);
}

TEST_CASE("a deleted file stays readable while open and disappears after release") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "tmp.txt", "still here");
    REQUIRE(t.fs->open(id));

    REQUIRE(t.fs->remove_file(ROOT_FILE_ID, "tmp.txt"));
    CHECK(t.fs->lookup(ROOT_FILE_ID, "tmp.txt").error() == ENOENT);
    CHECK(t.read_all(id) == "still here");

    t.fs->release(id);
    CHECK(t.fs->get_attributes(id)); // the kernel still remembers the ID

    t.fs->forget(id, 1);
    CHECK(t.fs->get_attributes(id).error() == ENOENT);
}

TEST_CASE("files deleted while open are cleaned up after a crash") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "tmp.txt", "data");
    REQUIRE(t.fs->open(id));
    REQUIRE(t.fs->remove_file(ROOT_FILE_ID, "tmp.txt"));
    CHECK(t.fs->get_attributes(id));

    t.reopen(); // no release() or forget(), like a crash
    CHECK(t.fs->get_attributes(id).error() == ENOENT);
}

TEST_CASE("hard links share one file") {
    TestFilesystem t;
    uint64_t id = t.create(ROOT_FILE_ID, "original", "shared");
    auto link = t.fs->make_hard_link(id, ROOT_FILE_ID, "link");
    REQUIRE(link);
    CHECK(link->id == id);
    CHECK(link->name_count == 2);

    REQUIRE(t.fs->remove_file(ROOT_FILE_ID, "original"));
    auto remaining = t.fs->lookup(ROOT_FILE_ID, "link");
    REQUIRE(remaining);
    CHECK(remaining->name_count == 1);
    CHECK(t.read_all(id) == "shared");

    uint64_t folder = t.mkdir(ROOT_FILE_ID, "folder");
    CHECK(t.fs->make_hard_link(folder, ROOT_FILE_ID, "folder-link").error() == EPERM);
}

TEST_CASE("symlinks store their target") {
    TestFilesystem t;
    auto link = t.fs->make_symlink(ROOT_FILE_ID, "shortcut", "/etc/hosts", ME);
    REQUIRE(link);
    CHECK(link->kind == FileKind::Symlink);
    CHECK(*t.fs->read_symlink(link->id) == "/etc/hosts");
    CHECK(link->size == 10);

    uint64_t file = t.create(ROOT_FILE_ID, "plain");
    CHECK(t.fs->read_symlink(file).error() == EINVAL);
}

TEST_CASE("rename moves names between folders") {
    TestFilesystem t;
    uint64_t a = t.mkdir(ROOT_FILE_ID, "a");
    uint64_t b = t.mkdir(ROOT_FILE_ID, "b");
    uint64_t file = t.create(a, "f", "x");
    uint64_t sub = t.mkdir(a, "sub");

    REQUIRE(t.fs->rename(a, "f", b, "g", true));
    CHECK(t.fs->lookup(b, "g")->id == file);
    CHECK(t.fs->lookup(a, "f").error() == ENOENT);

    REQUIRE(t.fs->rename(a, "sub", b, "sub", true));
    CHECK(t.fs->lookup(b, "sub")->id == sub);
    CHECK(t.fs->get_attributes(a)->name_count == 2);
    CHECK(t.fs->get_attributes(b)->name_count == 3);
}

TEST_CASE("rename replaces an existing file") {
    TestFilesystem t;
    uint64_t source = t.create(ROOT_FILE_ID, "new", "new contents");
    uint64_t target = t.create(ROOT_FILE_ID, "old", "old contents");

    CHECK(t.fs->rename(ROOT_FILE_ID, "new", ROOT_FILE_ID, "old", false).error() == EEXIST);
    REQUIRE(t.fs->rename(ROOT_FILE_ID, "new", ROOT_FILE_ID, "old", true));

    auto found = t.fs->lookup(ROOT_FILE_ID, "old");
    REQUIRE(found);
    CHECK(found->id == source);
    CHECK(t.read_all(source) == "new contents");
    CHECK(t.fs->list_folder(ROOT_FILE_ID)->size() == 1);

    t.fs->forget(target, 1);
    CHECK(t.fs->get_attributes(target).error() == ENOENT);
}

TEST_CASE("rename refuses invalid moves") {
    TestFilesystem t;
    uint64_t parent = t.mkdir(ROOT_FILE_ID, "parent");
    uint64_t child = t.mkdir(parent, "child");
    t.create(ROOT_FILE_ID, "file");
    uint64_t full = t.mkdir(ROOT_FILE_ID, "full");
    t.create(full, "inside");

    CHECK(t.fs->rename(ROOT_FILE_ID, "parent", child, "loop", true).error() == EINVAL);
    CHECK(t.fs->rename(ROOT_FILE_ID, "parent", parent, "self", true).error() == EINVAL);
    CHECK(t.fs->rename(ROOT_FILE_ID, "file", ROOT_FILE_ID, "parent", true).error() == EISDIR);
    CHECK(t.fs->rename(ROOT_FILE_ID, "parent", ROOT_FILE_ID, "file", true).error() == ENOTDIR);
    CHECK(t.fs->rename(ROOT_FILE_ID, "parent", ROOT_FILE_ID, "full", true).error() == ENOTEMPTY);
    CHECK(t.fs->rename(ROOT_FILE_ID, "missing", ROOT_FILE_ID, "x", true).error() == ENOENT);
}

TEST_CASE("data survives remounting") {
    TestFilesystem t;
    uint64_t docs = t.mkdir(ROOT_FILE_ID, "docs");
    t.create(docs, "keep.txt", "persistent");

    t.reopen();
    auto found_docs = t.fs->lookup(ROOT_FILE_ID, "docs");
    REQUIRE(found_docs);
    auto file = t.fs->lookup(found_docs->id, "keep.txt");
    REQUIRE(file);
    CHECK(t.read_all(file->id) == "persistent");
    REQUIRE(t.fs->sync());
}
