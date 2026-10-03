// cgroup_table.cpp - per-process cgroup limits (/sys/fs/cgroup, v1 + v2).
// Answers "is this process near its memory limit?" and joins to proc.
// `pid = ?` is pushed down so only that process is inspected; without it, every
// process is scanned (slow) -- so always scope it.
#include "vtab.h"
#include <fstream>

// Read a single integer from a cgroup file; "max" (unlimited) -> -1.
static bool readCgroupVal(const std::string& p, long long& out) {
    std::ifstream f(p);
    if (!f) return false;
    std::string s;
    if (!(f >> s)) return false;
    if (s == "max") { out = -1; return true; }
    try { out = std::stoll(s); return true; }
    catch (...) { return false; }
}

// Resolve the memory cgroup path for a pid and read its limits/usage.
// Supports cgroup v2 (unified "0::/path") and v1 (controller "memory:/path").
static bool readCgroup(long pid, std::string& path, long long& memMax,
                       long long& memCur, long long& pidsMax, long long& pidsCur) {
    memMax = memCur = pidsMax = pidsCur = -1;
    std::ifstream f("/proc/" + std::to_string(pid) + "/cgroup");
    if (!f) return false;
    bool v2 = false;
    std::string line;
    while (std::getline(f, line)) {
        auto p1 = line.find(':');
        if (p1 == std::string::npos) continue;
        auto p2 = line.find(':', p1 + 1);
        if (p2 == std::string::npos) continue;
        std::string ctrls = line.substr(p1 + 1, p2 - p1 - 1);
        std::string cpath = line.substr(p2 + 1);
        if (ctrls.empty()) { v2 = true; path = cpath; }        // v2 unified hierarchy
        else if (ctrls.find("memory") != std::string::npos) path = cpath;
    }
    if (path.empty()) return false;
    if (v2) {
        readCgroupVal("/sys/fs/cgroup" + path + "/memory.max", memMax);
        readCgroupVal("/sys/fs/cgroup" + path + "/memory.current", memCur);
        readCgroupVal("/sys/fs/cgroup" + path + "/pids.max", pidsMax);
        readCgroupVal("/sys/fs/cgroup" + path + "/pids.current", pidsCur);
    } else {
        readCgroupVal("/sys/fs/cgroup/memory" + path + "/memory.limit_in_bytes", memMax);
        readCgroupVal("/sys/fs/cgroup/memory" + path + "/memory.usage_in_bytes", memCur);
        readCgroupVal("/sys/fs/cgroup/pids" + path + "/pids.max", pidsMax);
        readCgroupVal("/sys/fs/cgroup/pids" + path + "/pids.current", pidsCur);
    }
    return true;
}

static std::vector<std::vector<Cell>> genCgroup(long pid) {
    std::vector<std::vector<Cell>> out;
    auto emit = [&](long p) {
        std::string path;
        long long mm = -1, mc = -1, pm = -1, pc = -1;
        if (readCgroup(p, path, mm, mc, pm, pc)) {
            out.push_back({
                cellInt(p),
                cellText(path),
                mm < 0 ? cellNull() : cellInt(mm),
                mc < 0 ? cellNull() : cellInt(mc),
                pm < 0 ? cellNull() : cellInt(pm),
                pc < 0 ? cellNull() : cellInt(pc),
            });
        }
    };
    if (pid >= 0) emit(pid);
    else for (long p : listProcPids()) emit(p);
    return out;
}

struct CgroupVtab : public ListVtab {
    CgroupVtab() {
        cols = {
            {"pid", "INTEGER"}, {"cgroup", "TEXT"},
            {"memory_max", "INTEGER"}, {"memory_current", "INTEGER"},
            {"pids_max", "INTEGER"}, {"pids_current", "INTEGER"},
        };
        pidCol = 0;            // `pid = ?` pushdown
        gen = genCgroup;
    }
};

static sqlite3_module g_cgroup_module = makeModule<CgroupVtab>();

void registerCgroup(sqlite3* db) {
    sqlite3_create_module(db, "cgroup", &g_cgroup_module, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE cgroup USING cgroup", nullptr, nullptr, nullptr);
}
