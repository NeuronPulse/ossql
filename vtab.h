#pragma once
// vtab.h - shared helpers for the SQLite virtual tables.
//
// Design:
//   - OSVtab's first member must be sqlite3_vtab, and OSCursor's first member
//     must be sqlite3_vtab_cursor, so the C callbacks can reinterpret_cast and
//     call the type-erased dispatch functions without a vtable pointer at offset 0.
//   - All tables reuse one set of generic C callbacks. A new table only needs to
//     subclass OSVtab, implement CreateCursor()/BestIndex(), and call makeModule<T>().
//   - Predicate pushdown is encoded in idxNum as argvIndex slots (8 bits each):
//     ext(0..7) path(8..15) pid(16..23) comm(24..31). Join-safe, no shared state.

#include <sqlite3.h>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <map>
#include <functional>

namespace fs = std::filesystem;

// One cell of a row.
struct Cell {
    enum Kind { Null, Int, Text, Lazy, Real } kind = Null;
    sqlite3_int64 i = 0;
    std::string s;
    double d = 0;
};
inline Cell cellNull() { Cell c; c.kind = Cell::Null; return c; }
inline Cell cellInt(sqlite3_int64 v) { Cell c; c.kind = Cell::Int; c.i = v; return c; }
inline Cell cellText(std::string v) { Cell c; c.kind = Cell::Text; c.s = std::move(v); return c; }
inline Cell cellReal(double v) { Cell c; c.kind = Cell::Real; c.d = v; return c; }
// Lazy cell: value is fetched on demand via OSCursor::lazyGet (e.g. /proc cmdline).
inline Cell cellLazy() { Cell c; c.kind = Cell::Lazy; return c; }

struct ColumnDef {
    std::string name;
    std::string type;
};

// Constraint plan: extracted by BestIndex, consumed by xFilter.
struct IndexPlan {
    bool hasExt = false;      std::string ext;        // fs: ext = ?
    bool hasPathLike = false; std::string pathLike;   // fs: path LIKE '...'
    bool hasPathEq = false;   std::string pathEq;     // fs: path = '/exact/file'
    bool hasPid = false;      long pid = 0;           // proc: pid = ?
    bool hasCommLike = false; std::string commLike;   // proc: comm LIKE '...'
    bool noRows = false;      // any pushed-down bound is NULL -> no rows
};

// SQLite LIKE match (% and _ only, no ESCAPE). Byte-wise; sufficient here.
inline bool sqlLike(const std::string& pat, const std::string& val) {
    size_t pi = 0, vi = 0;
    size_t star = std::string::npos, pstar = 0, vstar = 0;
    while (vi < val.size()) {
        if (pi < pat.size() && pat[pi] == '%') {
            star = pi; pstar = pi + 1; vstar = vi;
            ++pi;
            continue;
        }
        if (pi < pat.size() && (pat[pi] == '_' || pat[pi] == val[vi])) {
            ++pi; ++vi; continue;
        }
        if (star != std::string::npos) {
            pi = pstar; vi = vstar + 1; vstar = vi; continue;
        }
        return false;
    }
    while (pi < pat.size() && pat[pi] == '%') ++pi;
    return pi == pat.size();
}

// Text of a sqlite3_value (NULL -> "").
inline std::string valText(sqlite3_value* v) {
    if (!v || sqlite3_value_type(v) == SQLITE_NULL) return std::string();
    const unsigned char* t = sqlite3_value_text(v);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

struct OSCursor {
    sqlite3_vtab_cursor base;          // must be first member (no vptr -> offset 0)
    bool eof = true;
    sqlite3_int64 rowid = 0;
    std::optional<std::vector<Cell>> current;
    IndexPlan plan;

    // Fetch a Lazy column on demand. Set by the concrete cursor (e.g. proc cmdline).
    std::string (*lazyGet)(OSCursor*, int col) = nullptr;

    // Generic column output: read from the current row vector.
    int Column(sqlite3_context* ctx, int col) {
        if (!current || col < 0 || col >= (int)current->size()) {
            sqlite3_result_null(ctx);
            return SQLITE_OK;
        }
        const Cell& c = (*current)[col];
        if (c.kind == Cell::Lazy) {
            std::string v = lazyGet ? lazyGet(this, col) : std::string();
            sqlite3_result_text(ctx, v.c_str(), -1, SQLITE_TRANSIENT);
            return SQLITE_OK;
        }
        switch (c.kind) {
            case Cell::Null: sqlite3_result_null(ctx); break;
            case Cell::Int:  sqlite3_result_int64(ctx, c.i); break;
            case Cell::Text: sqlite3_result_text(ctx, c.s.c_str(), -1, SQLITE_TRANSIENT); break;
            case Cell::Real: sqlite3_result_double(ctx, c.d); break;
            default: break;
        }
        return SQLITE_OK;
    }
    int Eof() { return eof ? 1 : 0; }

    // Type-erased dispatch (set by CreateCursor). Avoids a vtable pointer at
    // offset 0, which would break the sqlite3_vtab_cursor-at-offset-0 contract.
    int (*doNext)(OSCursor*) = nullptr;
    int (*doFilter)(OSCursor*, int, sqlite3_value**) = nullptr;
    void (*freeCursor)(OSCursor*) = nullptr;
};

struct OSVtab {
    sqlite3_vtab base;                 // must be first member (no vptr -> offset 0)
    sqlite3* db = nullptr;
    std::string errMsg;
    std::vector<ColumnDef> cols;

    // Build the schema from cols and declare the virtual table.
    int Init() {
        std::string sql = "CREATE TABLE x(";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) sql += ",";
            sql += cols[i].name + " " + cols[i].type;
        }
        sql += ")";
        int rc = sqlite3_declare_vtab(db, sql.c_str());
        if (rc != SQLITE_OK) errMsg = "sqlite3_declare_vtab failed";
        return rc;
    }
    int colIndex(const char* name) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i].name == name) return (int)i;
        return -1;
    }
    void setErr(const char* msg) {
        sqlite3_free(base.zErrMsg);
        base.zErrMsg = sqlite3_mprintf("%s", msg);
    }
    OSCursor* CreateCursor() { return nullptr; }       // overridden by concrete tables
    int BestIndex(sqlite3_index_info*) { return SQLITE_ERROR; }
};

// ---- C callback bridge ----

inline int vt_xColumn(sqlite3_vtab_cursor* cur, sqlite3_context* ctx, int col) {
    return reinterpret_cast<OSCursor*>(cur)->Column(ctx, col);
}
inline int vt_xNext(sqlite3_vtab_cursor* cur) {
    OSCursor* c = reinterpret_cast<OSCursor*>(cur);
    return c->doNext ? c->doNext(c) : SQLITE_ERROR;
}
inline int vt_xEof(sqlite3_vtab_cursor* cur) {
    return reinterpret_cast<OSCursor*>(cur)->Eof();
}
inline int vt_xFilter(sqlite3_vtab_cursor* cur, int idxNum, const char*, int,
                      sqlite3_value** argv) {
    OSCursor* c = reinterpret_cast<OSCursor*>(cur);
    return c->doFilter ? c->doFilter(c, idxNum, argv) : SQLITE_ERROR;
}
inline int vt_xRowid(sqlite3_vtab_cursor* cur, sqlite3_int64* pRowid) {
    *pRowid = reinterpret_cast<OSCursor*>(cur)->rowid;
    return SQLITE_OK;
}
inline int vt_xClose(sqlite3_vtab_cursor* cur) {
    OSCursor* c = reinterpret_cast<OSCursor*>(cur);
    if (c->freeCursor) c->freeCursor(c);
    else delete c;
    return SQLITE_OK;
}

// vtab callbacks are templated to call the concrete table type T directly.
template <class T>
int vt_xBestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
    return reinterpret_cast<T*>(tab)->BestIndex(info);
}
template <class T>
int vt_xOpen(sqlite3_vtab* tab, sqlite3_vtab_cursor** pp) {
    OSCursor* c = reinterpret_cast<T*>(tab)->CreateCursor();
    if (!c) return SQLITE_NOMEM;
    *pp = reinterpret_cast<sqlite3_vtab_cursor*>(c);
    return SQLITE_OK;
}
template <class T>
int vt_xDisconnect(sqlite3_vtab* tab) {
    delete reinterpret_cast<T*>(tab);
    return SQLITE_OK;
}
template <class T>
int vt_xConnect(sqlite3* db, void*, int, const char* const*,
                sqlite3_vtab** pp, char** pzErr) {
    T* v = new T();
    v->db = db;
    int rc = v->Init();
    if (rc != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("%s", v->errMsg.c_str());
        delete v;
        return rc;
    }
    *pp = reinterpret_cast<sqlite3_vtab*>(v);
    return SQLITE_OK;
}

template <class T>
sqlite3_module makeModule() {
    sqlite3_module m{};
    m.iVersion = 1;
    m.xCreate = vt_xConnect<T>;
    m.xConnect = vt_xConnect<T>;
    m.xBestIndex = vt_xBestIndex<T>;
    m.xDisconnect = vt_xDisconnect<T>;
    m.xDestroy = vt_xDisconnect<T>;
    m.xOpen = vt_xOpen<T>;
    m.xClose = vt_xClose;
    m.xFilter = vt_xFilter;
    m.xNext = vt_xNext;
    m.xEof = vt_xEof;
    m.xColumn = vt_xColumn;
    m.xRowid = vt_xRowid;
    return m;
}

// idxNum pack/unpack helpers: 8 bits per slot.
inline int packIdx(int extArgv, int pathArgv, int pidArgv, int commArgv) {
    return (extArgv & 0xff) | ((pathArgv & 0xff) << 8) |
           ((pidArgv & 0xff) << 16) | ((commArgv & 0xff) << 24);
}
inline void unpackIdx(int idxNum, int& extArgv, int& pathArgv, int& pidArgv, int& commArgv) {
    extArgv = idxNum & 0xff;
    pathArgv = (idxNum >> 8) & 0xff;
    pidArgv = (idxNum >> 16) & 0xff;
    commArgv = (idxNum >> 24) & 0xff;
}

// ---- List-backed virtual tables ----
// Many OS sources are small text files or a scoped /proc scan that are easiest
// to materialize once into memory, then iterate as a plain vector. ListVtab
// handles that: a table supplies `cols` and a `gen(pid)` generator; iteration
// is a vector walk. `pidCol >= 0` enables `pid = ?` pushdown so fd/env scan
// only the one requested process instead of all of /proc.
struct ListCursor : public OSCursor {
    std::vector<std::vector<Cell>> rows;
    size_t pos = 0;
    int advance() {
        if (pos < rows.size()) { current = std::move(rows[pos]); ++rowid; ++pos; eof = false; return SQLITE_OK; }
        eof = true; current.reset(); return SQLITE_OK;
    }
    int Next() { return advance(); }
};

struct ListVtab : public OSVtab {
    int pidCol = -1;  // column index usable for `pid = ?`; -1 disables pushdown
    std::function<std::vector<std::vector<Cell>>(long pid)> gen;

    OSCursor* CreateCursor() {
        ListCursor* c = new ListCursor();
        c->freeCursor = [](OSCursor* p) { delete static_cast<ListCursor*>(p); };
        c->doNext = [](OSCursor* p) { return static_cast<ListCursor*>(p)->advance(); };
        // Stateless (no capture) so it fits the doFilter function pointer. The
        // owning ListVtab is reached via the cursor's pVtab.
        c->doFilter = [](OSCursor* p, int idxNum, sqlite3_value** argv) {
            ListCursor* c = static_cast<ListCursor*>(p);
            ListVtab* v = static_cast<ListVtab*>(reinterpret_cast<OSVtab*>(p->base.pVtab));
            int extArgv, pathArgv, pidArgv, commArgv;
            unpackIdx(idxNum, extArgv, pathArgv, pidArgv, commArgv);
            long pid = -1;
            if (pidArgv) {
                if (sqlite3_value_type(argv[pidArgv - 1]) == SQLITE_NULL) { c->rows.clear(); c->eof = true; return SQLITE_OK; }
                try { pid = std::stol(valText(argv[pidArgv - 1])); }
                catch (...) { c->rows.clear(); c->eof = true; return SQLITE_OK; }
            }
            c->rows = v->gen(pid);
            c->pos = 0; c->rowid = 0;
            // Position on the first row now: sqlite calls xColumn right after
            // xFilter (before any xNext), so `current` must be set, not just eof.
            return c->advance();
        };
        return c;
    }
    int BestIndex(sqlite3_index_info* info) {
        int pidArgv = 0, nextArg = 1;
        double cost = 1e6;
        if (pidCol >= 0) {
            for (int i = 0; i < info->nConstraint; ++i) {
                auto& c = info->aConstraint[i];
                if (!c.usable) continue;
                if (c.op == SQLITE_INDEX_CONSTRAINT_EQ && c.iColumn == pidCol) {
                    pidArgv = nextArg++;
                    info->aConstraintUsage[i].argvIndex = pidArgv;
                    info->aConstraintUsage[i].omit = 1;
                    cost = 1.0;
                }
            }
        }
        info->idxNum = packIdx(0, 0, pidArgv, 0);
        info->estimatedCost = cost;
        info->estimatedRows = (cost < 1e6) ? 200 : 100000;
        return SQLITE_OK;
    }
};

// ---- OS state parsers (see snap.cpp) ----

// Fault-tolerant recursive directory walker (explicit DFS frame stack). Each frame
// keeps its own directory_iterator, so siblings are never lost. Unreadable dirs are
// skipped, and symlink dirs are returned but not descended into (no symlink cycles).
struct FsWalker {
    struct Frame {
        std::filesystem::path dir;
        std::filesystem::directory_iterator it;
        bool ok = true;
    };
    std::vector<Frame> st;
    std::filesystem::path nextPath;
    bool hasNext = false;
    explicit FsWalker(const std::filesystem::path& root) {
        std::error_code ec;
        st.push_back(Frame{root, std::filesystem::directory_iterator(root, ec), !ec});
        step();
    }
    void step() {
        hasNext = false;
        while (!st.empty()) {
            Frame& f = st.back();
            if (!f.ok) { st.pop_back(); continue; }
            if (f.it == std::filesystem::directory_iterator()) { st.pop_back(); continue; }
            std::error_code ec2;
            auto e = *f.it;                       // cache entry before advancing
            std::filesystem::path p = e.path();
            f.it.increment(ec2);                 // advance to next sibling now
            if (e.is_symlink(ec2)) { nextPath = p; hasNext = true; return; }
            if (ec2) continue;
            bool isDir = e.is_directory(ec2);
            if (ec2) continue;
            if (isDir) {                         // descend (frame stays below on stack)
                std::error_code ec3;
                st.push_back(Frame{p, std::filesystem::directory_iterator(p, ec3), !ec3});
            }
            nextPath = p; hasNext = true;
            return;
        }
    }
    bool next(std::filesystem::path& out) {
        if (!hasNext) return false;
        out = nextPath;
        step();
        return true;
    }
};

// Extension with dot, e.g. "main.go" -> ".go"; none -> "".
std::string fileExt(const std::filesystem::path& p);

// Process info parsed from /proc. Times are in clock ticks; starttime is stat
// field 22 (PID reuse guard). rss is bytes; vsize is bytes.
struct ProcInfo {
    long pid = 0;
    long ppid = 0;
    std::string comm;
    char state = '?';
    long uid = -1;         // real uid (from /proc/<pid> dir owner)
    long gid = -1;         // real gid
    long long starttime = 0;
    long long rss = 0;     // resident set size, in bytes
    long long vsize = 0;   // virtual memory size, in bytes
    long threads = 0;
    long long utime = 0;   // CPU time in user mode (ticks)
    long long stime = 0;   // CPU time in kernel mode (ticks)
    long nice = 0;
    long priority = 0;
    long tty = 0;          // controlling terminal device nr (0 = none)
    // cmdline/euid/egid are read lazily on demand (ProcCursor::lazyGet).
};

std::vector<long> listProcPids();                 // numeric /proc directories
bool readProcInfo(long pid, ProcInfo& out);       // false if process vanished
std::string readProcCmdline(long pid);            // lazy cmdline read

// Read a file's full content; returns false if larger than maxBytes (file_text()).
bool readFileText(const std::string& path, std::string& out, size_t maxBytes);

// Registration entry points (see *_table.cpp / functions.cpp).
void registerFs(sqlite3* db);
void registerProc(sqlite3* db);
void registerSystem(sqlite3* db);
void registerPasswd(sqlite3* db);
void registerGroup(sqlite3* db);
void registerMounts(sqlite3* db);
void registerNet(sqlite3* db);
void registerFd(sqlite3* db);
void registerEnv(sqlite3* db);
void registerOssqlFunctions(sqlite3* db);
