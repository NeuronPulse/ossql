// mounts_table.cpp - /proc/self/mountinfo as a virtual table.
#include "vtab.h"
#include <fstream>
#include <sstream>

static std::vector<std::vector<Cell>> genMounts(long) {
    std::vector<std::vector<Cell>> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> t;
        while (ss >> tok) t.push_back(tok);
        if (t.size() < 7) continue;
        size_t dash = t.size();
        for (size_t i = 6; i < t.size(); ++i)
            if (t[i] == "-") { dash = i; break; }
        if (dash + 3 > t.size()) continue;  // need fstype + source + super opts
        if (t[0].empty() || t[4].empty() || t[dash + 1].empty() || t[dash + 2].empty()) {
            std::fprintf(stderr, "mounts: skip malformed line: %s\n", line.c_str());
            continue;
        }
        std::string opts;
        for (size_t i = 5; i < dash; ++i) { if (i > 5) opts += ' '; opts += t[i]; }
        out.push_back({cellInt(std::stol(t[0])), cellInt(std::stol(t[1])), cellText(t[2]),
                       cellText(t[3]), cellText(t[4]), cellText(t[dash + 1]),
                       cellText(t[dash + 2]), cellText(opts)});
    }
    return out;
}

struct MountsVtab : public ListVtab {
    MountsVtab() {
        cols = {{"id", "INTEGER"}, {"parent", "INTEGER"}, {"dev", "TEXT"},
                {"root", "TEXT"}, {"mountpoint", "TEXT"}, {"fstype", "TEXT"},
                {"source", "TEXT"}, {"opts", "TEXT"}};
        gen = genMounts;
    }
};

static sqlite3_module g_mounts_mod = makeModule<MountsVtab>();

void registerMounts(sqlite3* db) {
    sqlite3_create_module(db, "mounts", &g_mounts_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE mounts USING mounts", nullptr, nullptr, nullptr);
}
