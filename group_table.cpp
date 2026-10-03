// group_table.cpp - /etc/group as a virtual table (gid -> name + members).
#include "vtab.h"
#include <fstream>
#include <sstream>

static std::vector<std::vector<Cell>> genGroup(long) {
    std::vector<std::vector<Cell>> out;
    std::ifstream f("/etc/group");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> t;
        std::istringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ':')) t.push_back(tok);
        if (t.size() < 4) continue;
        out.push_back({cellText(t[0]), cellInt(std::stol(t[2])), cellText(t[3])});
    }
    return out;
}

struct GroupVtab : public ListVtab {
    GroupVtab() {
        cols = {{"name", "TEXT"}, {"gid", "INTEGER"}, {"members", "TEXT"}};
        gen = genGroup;
    }
};

static sqlite3_module g_group_mod = makeModule<GroupVtab>();

void registerGroup(sqlite3* db) {
    sqlite3_create_module(db, "group", &g_group_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE \"group\" USING \"group\"", nullptr, nullptr, nullptr);
}
