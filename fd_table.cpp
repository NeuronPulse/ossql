// fd_table.cpp - /proc/<pid>/fd as a virtual table (an `lsof` replacement).
// `pid = ?` is pushed down so only that process is scanned. `inode` matches
// `net.inode` (sockets/pipes) so connections attribute back to a process.
#include "vtab.h"
#include <filesystem>

static long long parseBracket(const std::string& s) {
    auto a = s.find('['), b = s.find(']');
    if (a == std::string::npos || b == std::string::npos) return 0;
    try { return std::stoll(s.substr(a + 1, b - a - 1)); }
    catch (...) { return 0; }
}

static std::vector<std::vector<Cell>> genFd(long pid) {
    std::vector<std::vector<Cell>> out;
    std::vector<long> pids = (pid >= 0) ? std::vector<long>{pid} : listProcPids();
    for (long p : pids) {
        std::error_code ec;
        std::filesystem::path dir = "/proc/" + std::to_string(p) + "/fd";
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            long fd = -1;
            try { fd = std::stol(e.path().filename().string()); }
            catch (...) { continue; }
            std::error_code lec;
            std::string target = std::filesystem::read_symlink(e.path(), lec);
            if (lec) continue;
            std::string type = "file";
            long long inode = 0;
            if (target.rfind("socket:", 0) == 0) { type = "socket"; inode = parseBracket(target); }
            else if (target.rfind("pipe:", 0) == 0) { type = "pipe"; inode = parseBracket(target); }
            else if (target.rfind("anon_inode:", 0) == 0) type = "anon_inode";
            else if (target.rfind("/memfd:", 0) == 0) type = "memfd";
            out.push_back({cellInt(p), cellInt(fd), cellText(target), cellText(type), cellInt(inode)});
        }
    }
    return out;
}

struct FdVtab : public ListVtab {
    FdVtab() {
        cols = {{"pid", "INTEGER"}, {"fd", "INTEGER"}, {"path", "TEXT"},
                {"type", "TEXT"}, {"inode", "INTEGER"}};
        pidCol = colIndex("pid");
        gen = genFd;
    }
};

static sqlite3_module g_fd_mod = makeModule<FdVtab>();

void registerFd(sqlite3* db) {
    sqlite3_create_module(db, "fd", &g_fd_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE fd USING fd", nullptr, nullptr, nullptr);
}
