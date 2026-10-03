// functions.cpp - custom SQL functions: file_text(path) reads a file for content search.
#include "vtab.h"

static void sql_file_text(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 1) { sqlite3_result_null(ctx); return; }
    std::string path = valText(argv[0]);
    if (path.empty()) { sqlite3_result_null(ctx); return; }
    std::string content;
    // 16MB cap to avoid OOM on large/special files.
    if (!readFileText(path, content, 16 * 1024 * 1024)) {
        sqlite3_result_null(ctx);
        return;
    }
    sqlite3_result_text(ctx, content.c_str(), -1, SQLITE_TRANSIENT);
}

// file_grep(path, pattern) -> 1 if the file contains pattern (substring), else 0.
// Pairs with fs for content search without pulling the whole file into SQL.
static void sql_file_grep(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 2) { sqlite3_result_null(ctx); return; }
    std::string path = valText(argv[0]);
    std::string pat = valText(argv[1]);
    if (path.empty() || pat.empty()) { sqlite3_result_int(ctx, 0); return; }
    std::string content;
    if (!readFileText(path, content, 16 * 1024 * 1024)) { sqlite3_result_int(ctx, 0); return; }
    sqlite3_result_int(ctx, content.find(pat) != std::string::npos ? 1 : 0);
}

void registerOssqlFunctions(sqlite3* db) {
    sqlite3_create_function(db, "file_text", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            nullptr, sql_file_text, nullptr, nullptr);
    sqlite3_create_function(db, "file_grep", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            nullptr, sql_file_grep, nullptr, nullptr);
}
