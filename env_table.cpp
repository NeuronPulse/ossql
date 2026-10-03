// env_table.cpp - /proc/<pid>/environ as a virtual table (one row per var).
// `pid = ?` is pushed down; without it, every process is scanned (slow), so
// always scope the query: SELECT key,value FROM env WHERE pid = 1234.
#include "vtab.h"
#include <fstream>
#include <iterator>

static std::vector<std::vector<Cell>> genEnv(long pid) {
    std::vector<std::vector<Cell>> out;
    std::vector<long> pids = (pid >= 0) ? std::vector<long>{pid} : listProcPids();
    for (long p : pids) {
        std::ifstream f("/proc/" + std::to_string(p) + "/environ", std::ios::binary);
        if (!f) continue;
        std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t start = 0;
        while (start < buf.size()) {
            size_t end = buf.find('\0', start);
            if (end == std::string::npos) end = buf.size();
            std::string kv = buf.substr(start, end - start);
            auto eq = kv.find('=');
            std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
            std::string val = (eq == std::string::npos) ? std::string() : kv.substr(eq + 1);
            out.push_back({cellInt(p), cellText(key), cellText(val)});
            start = end + 1;
        }
    }
    return out;
}

struct EnvVtab : public ListVtab {
    EnvVtab() {
        cols = {{"pid", "INTEGER"}, {"key", "TEXT"}, {"value", "TEXT"}};
        pidCol = colIndex("pid");
        gen = genEnv;
    }
};

static sqlite3_module g_env_mod = makeModule<EnvVtab>();

void registerEnv(sqlite3* db) {
    sqlite3_create_module(db, "env", &g_env_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE env USING env", nullptr, nullptr, nullptr);
}
