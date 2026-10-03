// system_table.cpp - one-row snapshot of the host: uptime, load, memory, cpus.
// Reads /proc/uptime, /proc/loadavg, /proc/meminfo, /proc/stat, and
// /proc/sys/kernel/hostname. A tiny, zero-traversal view that replaces a
// one-off `uptime`/`free`/`nproc`.
#include "vtab.h"
#include <fstream>
#include <sstream>

// First "key:  value" line in path whose key equals `key` -> integer value.
static bool readKV(const std::string& path, const std::string& key, long long& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, key.size(), key) == 0 && line[key.size()] == ':') {
            size_t i = key.size() + 1;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            try { out = std::stoll(line.substr(i)); return true; }
            catch (...) { return false; }
        }
    }
    return false;
}

// Count "cpuN" lines in /proc/stat (number of logical processors).
static long countCpus() {
    std::ifstream f("/proc/stat");
    if (!f) return 0;
    long n = 0;
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 3, "cpu") == 0 && std::isdigit((unsigned char)line[3])) ++n;
    return n;
}

static std::vector<std::vector<Cell>> genSystem(long) {
    std::vector<std::vector<Cell>> out;
    std::vector<Cell> r;

    std::string host;
    if (!readFileText("/proc/sys/kernel/hostname", host, 256))
        host = std::string();
    while (!host.empty() && (host.back() == '\n' || host.back() == '\r')) host.pop_back();
    r.push_back(cellText(host));                                   // hostname

    double up = 0, idle = 0;
    {
        std::ifstream f("/proc/uptime");
        if (f) f >> up >> idle;
    }
    r.push_back(cellReal(up));                                     // uptime (s)
    r.push_back(cellReal(idle));                                   // idle (s)

    double l1 = 0, l5 = 0, l15 = 0;
    {
        std::ifstream f("/proc/loadavg");
        if (f) f >> l1 >> l5 >> l15;
    }
    r.push_back(cellReal(l1));                                      // load1
    r.push_back(cellReal(l5));                                      // load5
    r.push_back(cellReal(l15));                                     // load15

    long long btime = 0;
    if (!readKV("/proc/stat", "btime", btime)) btime = 0;
    r.push_back(cellInt(btime));                                    // boot_time (epoch s)

    r.push_back(cellInt(countCpus()));                             // nproc

    // Memory figures from /proc/meminfo are in kB; convert to bytes.
    auto mem = [&](const std::string& key) -> sqlite3_int64 {
        long long kb = 0;
        return readKV("/proc/meminfo", key, kb) ? kb * 1024 : 0;
    };
    r.push_back(cellInt(mem("MemTotal")));                         // mem_total (bytes)
    r.push_back(cellInt(mem("MemFree")));                          // mem_free
    r.push_back(cellInt(mem("MemAvailable")));                     // mem_available
    r.push_back(cellInt(mem("Buffers")));                          // buffers
    r.push_back(cellInt(mem("Cached")));                           // cached

    out.push_back(std::move(r));
    return out;
}

struct SystemVtab : public ListVtab {
    SystemVtab() {
        cols = {
            {"hostname", "TEXT"}, {"uptime", "REAL"}, {"idle", "REAL"},
            {"load1", "REAL"}, {"load5", "REAL"}, {"load15", "REAL"},
            {"boot_time", "INTEGER"}, {"nproc", "INTEGER"},
            {"mem_total", "INTEGER"}, {"mem_free", "INTEGER"},
            {"mem_available", "INTEGER"}, {"buffers", "INTEGER"}, {"cached", "INTEGER"},
        };
        pidCol = -1;          // single-row table, no pid pushdown
        gen = genSystem;
    }
};

static sqlite3_module g_system_module = makeModule<SystemVtab>();

void registerSystem(sqlite3* db) {
    sqlite3_create_module(db, "system", &g_system_module, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE system USING system", nullptr, nullptr, nullptr);
}
