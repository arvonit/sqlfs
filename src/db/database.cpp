#include "db/database.h"

#include "schema.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

int sqlite_error_to_errno(sqlite3* connection, int code) {
    std::fprintf(stderr, "sqlfs: SQLite error %d: %s\n", code, sqlite3_errmsg(connection));
    switch (code & 0xff) {
    case SQLITE_CONSTRAINT:
        return EEXIST;
    case SQLITE_TOOBIG:
        return EFBIG;
    case SQLITE_FULL:
        return ENOSPC;
    default:
        return EIO;
    }
}

Database::~Database() {
    sqlite3_close(connection);
}

FSResult<void> Database::open(const std::string& path) {
    int code = sqlite3_open(path.c_str(), &connection);
    if (code != SQLITE_OK)
        return std::unexpected(sqlite_error_to_errno(connection, code));
    return execute(SCHEMA_SQL);
}

FSResult<void> Database::execute(const char* sql) {
    int code = sqlite3_exec(connection, sql, nullptr, nullptr, nullptr);
    if (code != SQLITE_OK)
        return std::unexpected(sqlite_error_to_errno(connection, code));
    return {};
}

int64_t Database::last_insert_id() const {
    return sqlite3_last_insert_rowid(connection);
}

Statement::Statement(Database& database, const char* sql) : connection(database.handle()) {
    int code = sqlite3_prepare_v2(connection, sql, -1, &statement, nullptr);
    // A failed prepare means the SQL in our source code is wrong, so fail loudly
    if (code != SQLITE_OK) {
        std::fprintf(stderr, "sqlfs: bad SQL: %s\n%s\n", sqlite3_errmsg(connection), sql);
        std::abort();
    }
}

Statement::~Statement() {
    sqlite3_finalize(statement);
}

void Statement::bind(int index, int64_t value) {
    sqlite3_bind_int64(statement, index, value);
}

void Statement::bind(int index, std::string_view text) {
    sqlite3_bind_text(statement, index, text.data(), (int)text.size(), SQLITE_TRANSIENT);
}

void Statement::bind_blob(int index, std::string_view bytes) {
    // zeroblob keeps empty files as an empty BLOB instead of NULL
    if (bytes.empty())
        sqlite3_bind_zeroblob(statement, index, 0);
    else
        sqlite3_bind_blob64(statement, index, bytes.data(), bytes.size(), SQLITE_TRANSIENT);
}

void Statement::bind_null(int index) {
    sqlite3_bind_null(statement, index);
}

FSResult<bool> Statement::step() {
    int code = sqlite3_step(statement);
    if (code == SQLITE_ROW)
        return true;
    if (code == SQLITE_DONE)
        return false;
    return std::unexpected(sqlite_error_to_errno(connection, code));
}

FSResult<void> Statement::run() {
    auto result = step();
    if (!result)
        return std::unexpected(result.error());
    return {};
}

int64_t Statement::column_int(int index) {
    return sqlite3_column_int64(statement, index);
}

std::string Statement::column_text(int index) {
    auto text = (const char*)sqlite3_column_text(statement, index);
    return text ? std::string(text, sqlite3_column_bytes(statement, index)) : std::string();
}

std::string Statement::column_blob(int index) {
    auto bytes = (const char*)sqlite3_column_blob(statement, index);
    return bytes ? std::string(bytes, sqlite3_column_bytes(statement, index)) : std::string();
}

bool Statement::column_is_null(int index) {
    return sqlite3_column_type(statement, index) == SQLITE_NULL;
}

Transaction::Transaction(Database& database) : database(database) {
    // IMMEDIATE takes the write lock up front instead of failing halfway through
    begun = database.execute("BEGIN IMMEDIATE").has_value();
}

Transaction::~Transaction() {
    if (begun && !committed)
        database.execute("ROLLBACK");
}

FSResult<void> Transaction::commit() {
    if (!begun)
        return std::unexpected(EIO);
    auto result = database.execute("COMMIT");
    if (result)
        committed = true;
    return result;
}
