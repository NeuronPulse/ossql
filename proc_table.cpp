// proc_table.cpp - process virtual table: pid/ppid/comm/cmdline/state/uid/gid/...
// Pushdown: pid = ? (direct lookup) and comm LIKE ? (pre-filter via /proc/comm).
// cmdline is read lazily (only when selected), rss/threads come from stat.
#include "vtab.h"
#include <fstream>

// Column order (must match cols in ProcVtab and buildRow).
enum {
    C_PID, C_PPID, C_COMM, C_CMDLINE, C_STATE,
    C_UID, C_GID, C_EUID, C_EGID, C_STARTTIME,
    C_RSS, C_VSIZE, C_THREADS, C_UTIME, C_STIME,
    C_NICE, C_PRIORITY, C_TTY,
    C_IO_READ, C_IO_WRITE, C_RLIMIT_NOFILE
};

static std::string readComm(long pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    if (!f) return std::string();
    std::string s;
    std::getline(f, s);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

// Lazy: effective uid/gid from /proc/<pid>/status (only when those columns are used).
static bool readProcEffectiveIds(long pid, long& euid, long& egid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    if (!f) return false;
    euid = egid = -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
            std::istringstream iss(line.substr(4));
            long r, e, s, f2;
            if (iss >> r >> e >> s >> f2) euid = e;
        } else if (line.compare(0, 4, "Gid:") == 0) {
            std::istringstream iss(line.substr(4));
            long r, e, s, f2;
            if (iss >> r >> e >> s >> f2) egid = e;
        }
    }
    return true;
}

struct ProcCursor : public OSCursor {
    std::vector<long> pids;
    size_t idx = 0;

    std::vector<Cell> buildRow(const ProcInfo& info) {
        std::vector<Cell> r;
        r.push_back(cellInt(info.pid));                       // 0 pid
        r.push_back(cellInt(info.ppid));                      // 1 ppid
        r.push_back(cellText(info.comm));                     // 2 comm
        r.push_back(cellLazy());                              // 3 cmdline (on demand)
        r.push_back(cellText(std::string(1, info.state)));    // 4 state
        r.push_back(cellInt(info.uid));                       // 5 uid (real)
        r.push_back(cellInt(info.gid));                       // 6 gid (real)
        r.push_back(cellLazy());                              // 7 euid (on demand)
        r.push_back(cellLazy());                              // 8 egid (on demand)
        r.push_back(cellInt(info.starttime));                 // 9 starttime (ticks)
        r.push_back(cellInt(info.rss));                       // 10 rss (bytes)
        r.push_back(cellInt(info.vsize));                     // 11 vsize (bytes)
        r.push_back(cellInt(info.threads));                   // 12 threads
        r.push_back(cellInt(info.utime));                     // 13 utime (ticks)
        r.push_back(cellInt(info.stime));                     // 14 stime (ticks)
        r.push_back(cellInt(info.nice));                      // 15 nice
        r.push_back(cellInt(info.priority));                  // 16 priority
        r.push_back(cellInt(info.tty));                       // 17 tty (0 = none)
        r.push_back(cellLazy());                              // 18 io_read (on demand)
        r.push_back(cellLazy());                              // 19 io_write (on demand)
        r.push_back(cellLazy());                              // 20 rlimit_nofile (on demand)
        return r;
    }

    // Lazily parsed extras for the current process (read once per pid).
    long extraPid = -1;
    long long ioRead = 0, ioWrite = 0;
    bool ioReadValid = false;       // false until first attempt for extraPid
    bool ioAvail = false;           // true if the io file was readable
    long nofile = -1;
    bool nofileAvail = false;

    void ensureExtra(long pid) {
        if (pid == extraPid) return;
        extraPid = pid;
        ioReadValid = true; ioAvail = false; nofileAvail = false;
        ioRead = ioWrite = 0; nofile = -1;
        // /proc/<pid>/io (often unreadable for other users' processes).
        {
            std::ifstream f("/proc/" + std::to_string(pid) + "/io");
            if (f) {
                std::string line;
                while (std::getline(f, line)) {
                    if (line.compare(0, 11, "read_bytes:") == 0) {
                        try { ioRead = std::stoll(line.substr(11)); ioAvail = true; }
                        catch (...) {}
                    } else if (line.compare(0, 12, "write_bytes:") == 0) {
                        try { ioWrite = std::stoll(line.substr(12)); ioAvail = true; }
                        catch (...) {}
                    }
                }
            }
        }
        // /proc/<pid>/limits: "Max open files  <soft>  <hard>  files".
        {
            std::ifstream f("/proc/" + std::to_string(pid) + "/limits");
            if (f) {
                std::string line;
                while (std::getline(f, line)) {
                    if (line.find("Max open files") != std::string::npos) {
                        std::istringstream iss(line);
                        std::string tok;
                        long nums[2] = {-1, -1}; int got = 0;
                        while (iss >> tok) {
                            try { nums[got < 2 ? got++ : 1] = std::stol(tok); }
                            catch (...) {}
                        }
                        if (got >= 1) { nofile = nums[0]; nofileAvail = true; }
                        break;
                    }
                }
            }
        }
    }

    int advance() {
        while (idx < pids.size()) {
            long pid = pids[idx++];
            ProcInfo info;
            if (!readProcInfo(pid, info)) continue;  // process vanished -> skip
            // Both pid and comm may be constrained: pid narrows the candidate set,
            // comm must still be re-checked here (BestIndex omits both).
            if (plan.hasCommLike && !sqlLike(plan.commLike, info.comm)) continue;
            current = buildRow(info);
            ++rowid;
            return SQLITE_OK;
        }
        eof = true;
        current.reset();
        return SQLITE_OK;
    }

    int Filter(int idxNum, sqlite3_value** argv) {
        int extArgv, pathArgv, pidArgv, commArgv;
        unpackIdx(idxNum, extArgv, pathArgv, pidArgv, commArgv);
        plan = IndexPlan{};
        if (pidArgv) {
            if (sqlite3_value_type(argv[pidArgv - 1]) == SQLITE_NULL) plan.noRows = true;
            else {
                try {
                    plan.pid = std::stol(valText(argv[pidArgv - 1]));
                    plan.hasPid = true;
                } catch (...) { plan.noRows = true; }  // invalid pid -> no rows
            }
        }
        if (commArgv) {
            if (sqlite3_value_type(argv[commArgv - 1]) == SQLITE_NULL) plan.noRows = true;
            else { plan.commLike = valText(argv[commArgv - 1]); plan.hasCommLike = true; }
        }
        eof = false; rowid = 0; current.reset(); idx = 0;
        if (plan.noRows) { eof = true; return SQLITE_OK; }

        pids.clear();
        if (plan.hasPid) {
            ProcInfo info;
            if (readProcInfo(plan.pid, info)) pids.push_back(plan.pid);
        } else if (plan.hasCommLike) {
            // Pre-filter by comm only; avoids stat/cmdline reads for other pids.
            for (long p : listProcPids())
                if (sqlLike(plan.commLike, readComm(p))) pids.push_back(p);
        } else {
            pids = listProcPids();
        }
        return advance();
    }

    int Next() { return advance(); }
};

struct ProcVtab : public OSVtab {
    ProcVtab() {
        cols = {
            {"pid", "INTEGER"}, {"ppid", "INTEGER"}, {"comm", "TEXT"},
            {"cmdline", "TEXT"}, {"state", "TEXT"}, {"uid", "INTEGER"},
            {"gid", "INTEGER"}, {"euid", "INTEGER"}, {"egid", "INTEGER"},
            {"starttime", "INTEGER"}, {"rss", "INTEGER"}, {"vsize", "INTEGER"},
            {"threads", "INTEGER"}, {"utime", "INTEGER"}, {"stime", "INTEGER"},
            {"nice", "INTEGER"}, {"priority", "INTEGER"}, {"tty", "INTEGER"},
            {"io_read", "INTEGER"}, {"io_write", "INTEGER"}, {"rlimit_nofile", "INTEGER"},
        };
    }

    OSCursor* CreateCursor() {
        ProcCursor* c = new ProcCursor();
        c->freeCursor = [](OSCursor* p) { delete static_cast<ProcCursor*>(p); };
        c->doNext = [](OSCursor* p) { return static_cast<ProcCursor*>(p)->advance(); };
        c->doFilter = [](OSCursor* p, int n, sqlite3_value** a) {
            return static_cast<ProcCursor*>(p)->Filter(n, a);
        };
        // Lazy columns: cmdline and effective ids are read on demand only.
        c->lazyGet = [](OSCursor* p, int col) -> std::string {
            ProcCursor* self = static_cast<ProcCursor*>(p);
            if (!self->current) return std::string();
            long pid = (long)(*self->current)[C_PID].i;
            if (col == C_CMDLINE) return readProcCmdline(pid);
            if (col == C_EUID || col == C_EGID) {
                long euid = -1, egid = -1;
                readProcEffectiveIds(pid, euid, egid);
                return std::to_string(col == C_EUID ? euid : egid);
            }
            self->ensureExtra(pid);
            if (col == C_IO_READ)  return self->ioAvail  ? std::to_string(self->ioRead)  : std::string();
            if (col == C_IO_WRITE) return self->ioAvail  ? std::to_string(self->ioWrite) : std::string();
            if (col == C_RLIMIT_NOFILE) return self->nofileAvail ? std::to_string(self->nofile) : std::string();
            return std::string();
        };
        return c;
    }

    int Init() {
#ifdef __linux__
        return OSVtab::Init();
#else
        setErr("proc table requires Linux /proc (unsupported platform)");
        return SQLITE_ERROR;
#endif
    }

    int BestIndex(sqlite3_index_info* info) {
        int pidArgv = 0, commArgv = 0, nextArg = 1;
        double cost = 1e6;
        for (int i = 0; i < info->nConstraint; ++i) {
            auto& c = info->aConstraint[i];
            if (!c.usable) continue;
            int col = c.iColumn;
            if (c.op == SQLITE_INDEX_CONSTRAINT_EQ && col == colIndex("pid")) {
                pidArgv = nextArg++;
                info->aConstraintUsage[i].argvIndex = pidArgv;
                info->aConstraintUsage[i].omit = 1;
                cost = std::min(cost, 1e2);
            } else if (c.op == SQLITE_INDEX_CONSTRAINT_LIKE && col == colIndex("comm")) {
                commArgv = nextArg++;
                info->aConstraintUsage[i].argvIndex = commArgv;
                info->aConstraintUsage[i].omit = 1;
                cost = std::min(cost, 1e3);
            }
        }
        info->idxNum = packIdx(0, 0, pidArgv, commArgv);
        info->estimatedCost = cost;
        info->estimatedRows = (cost < 1e6) ? 200 : 2000;
        return SQLITE_OK;
    }
};

static sqlite3_module g_proc_module = makeModule<ProcVtab>();

void registerProc(sqlite3* db) {
    sqlite3_create_module(db, "proc", &g_proc_module, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE proc USING proc", nullptr, nullptr, nullptr);
}
