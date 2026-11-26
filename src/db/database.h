#pragma once

#include "core/fs_result.h"

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

// Owns one SQLite connection. Closes it when destroyed
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (or creates) the database file and applies the schema
    FSResult<void> open(const std::string& path);

    // Runs SQL that returns no rows. May contain several statements
    FSResult<void> execute(const char* sql);

    int64_t last_insert_id() const;
    sqlite3* handle() const {
        return connection;
    }

private:
    sqlite3* connection = nullptr;
};

// One prepared SQL statement. Bind parameters with 1-based indexes, call step(), then read
// columns with 0-based indexes
class Statement {
public:
    Statement(Database& database, const char* sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, int64_t value);
    void bind(int index, std::string_view text);
    void bind_blob(int index, std::string_view bytes);
    void bind_null(int index);

    // true when a row is ready to read, false when the statement is finished
    FSResult<bool> step();

    // Convenience for INSERT, UPDATE, and DELETE
    FSResult<void> run();

    int64_t column_int(int index);
    std::string column_text(int index);
    std::string column_blob(int index);
    bool column_is_null(int index);

private:
    sqlite3* connection;
    sqlite3_stmt* statement = nullptr;
};

// Groups changes so they all apply or none do. Rolls back when destroyed unless commit() ran,
// so returning early on an error undoes everything automatically
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    FSResult<void> commit();

private:
    Database& database;
    bool begun = false;
    bool committed = false;
};

// Logs the SQLite error message and turns the code into an errno value
int sqlite_error_to_errno(sqlite3* connection, int code);
