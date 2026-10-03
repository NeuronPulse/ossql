// passwd_table.cpp - /etc/passwd as a virtual table (uid/gid -> name joins).
#include "vtab.h"
#include <fstream>
#include <sstream>

static std::vector<std::vector<Cell>> genPasswd(long) {
    std::vector<std::vector<Cell>> out;
    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> t;
        std::istringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ':')) t.push_back(tok);
        if (t.size() < 7) continue;
        out.push_back({cellText(t[0]), cellInt(std::stol(t[2])), cellInt(std::stol(t[3])),
                       cellText(t[4]), cellText(t[5]), cellText(t[6])});
    }
    return out;
}

struct PasswdVtab : public ListVtab {
    PasswdVtab() {
        cols = {{"name", "TEXT"}, {"uid", "INTEGER"}, {"gid", "INTEGER"},
                {"gecos", "TEXT"}, {"home", "TEXT"}, {"shell", "TEXT"}};
        gen = genPasswd;
    }
};

static sqlite3_module g_passwd_mod = makeModule<PasswdVtab>();

void registerPasswd(sqlite3* db) {
    sqlite3_create_module(db, "passwd", &g_passwd_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE passwd USING passwd", nullptr, nullptr, nullptr);
}
