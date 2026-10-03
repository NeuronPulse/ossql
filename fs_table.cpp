// fs_table.cpp - filesystem virtual table: path/size/type/ext/mode/time/owner.
// Pushdown: ext = ? (filter) and path LIKE 'prefix%' (prune traversal root).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // expose statx()/AT_* on glibc
#endif
#include "vtab.h"
#include <sys/stat.h>
#include <sys/sysmacros.h>  // makedev/major/minor
#include <fcntl.h>     // AT_FDCWD, AT_SYMLINK_NOFOLLOW
#include <algorithm>

// Longest non-% prefix of a LIKE pattern (used to prune the walk root).
static std::string likePrefix(const std::string& pat) {
    auto pos = pat.find('%');
    if (pos == std::string::npos) return pat;
    return pat.substr(0, pos);
}

struct FsCursor : public OSCursor {
    std::optional<FsWalker> walker;

    std::optional<std::vector<Cell>> buildRow(const std::filesystem::path& p) {
        // statx gives btime (creation time). Fall back to stat if unavailable.
        long long size = 0, atime = 0, ctime = 0, mtime = 0, btime = 0;
        long long inode = 0, nlink = 0, uid = 0, gid = 0, mode = 0;
        long long blocks = 0, blksize = 0, dev = 0;
        bool symlink = false, isdir = false, isfile = false;

        struct statx lstx;
        bool haveStatx = statx(AT_FDCWD, p.c_str(), AT_SYMLINK_NOFOLLOW,
                               STATX_BASIC_STATS | STATX_BTIME, &lstx) == 0;
        if (haveStatx) {
            symlink = S_ISLNK(lstx.stx_mode);
            const struct statx* x = &lstx;
            struct statx fstx;
            if (symlink && statx(AT_FDCWD, p.c_str(), 0,
                                 STATX_BASIC_STATS | STATX_BTIME, &fstx) == 0)
                x = &fstx;  // follow symlink for real type/size/owner
            size = x->stx_size;
            mode = x->stx_mode;
            uid = x->stx_uid;  gid = x->stx_gid;
            mtime = x->stx_mtime.tv_sec;
            atime = x->stx_atime.tv_sec;
            ctime = x->stx_ctime.tv_sec;
            if (lstx.stx_mask & STATX_BTIME) btime = lstx.stx_btime.tv_sec;
            inode = x->stx_ino;
            nlink = x->stx_nlink;
            blocks = lstx.stx_blocks;
            blksize = lstx.stx_blksize;
            dev = makedev(lstx.stx_dev_major, lstx.stx_dev_minor);
            isdir = S_ISDIR(x->stx_mode);
            isfile = S_ISREG(x->stx_mode);
        } else {
            struct stat st;
            if (lstat(p.c_str(), &st) != 0) return std::nullopt;
            symlink = S_ISLNK(st.st_mode);
            struct stat fst = st;
            if (symlink) stat(p.c_str(), &fst);  // follow for real type/size
            size = fst.st_size; mode = fst.st_mode;
            uid = fst.st_uid; gid = fst.st_gid;
            mtime = fst.st_mtime; atime = fst.st_atime; ctime = fst.st_ctime;
            // btime has no struct stat equivalent on Linux
            inode = fst.st_ino; nlink = fst.st_nlink;
            blocks = fst.st_blocks; blksize = fst.st_blksize;
            dev = makedev(major(fst.st_dev), minor(fst.st_dev));
            isdir = S_ISDIR(fst.st_mode); isfile = S_ISREG(fst.st_mode);
        }

        std::vector<Cell> r;
        r.push_back(cellText(p.string()));                          // 0 path
        r.push_back(cellText(p.filename().string()));               // 1 name
        r.push_back(cellInt(size));                                 // 2 size
        r.push_back(cellInt((sqlite3_int64)isdir));                 // 3 is_dir
        r.push_back(cellInt((sqlite3_int64)isfile));                // 4 is_file
        r.push_back(cellInt((sqlite3_int64)symlink));               // 5 is_symlink
        r.push_back(cellText(fileExt(p)));                          // 6 ext
        r.push_back(cellInt(mode & 07777));                         // 7 mode
        r.push_back(cellInt(mtime));                                // 8 mtime
        r.push_back(cellInt(atime));                                // 9 atime
        r.push_back(cellInt(ctime));                                // 10 ctime
        r.push_back(cellInt(btime));                                // 11 btime (creation)
        r.push_back(cellInt(inode));                                // 12 inode
        r.push_back(cellInt(nlink));                                // 13 nlink
        r.push_back(cellInt(uid));                                  // 14 uid
        r.push_back(cellInt(gid));                                  // 15 gid
        r.push_back(cellInt(blocks));                               // 16 blocks (512B units)
        r.push_back(cellInt(blksize));                              // 17 blksize
        r.push_back(cellInt(dev));                                  // 18 dev (maj*256+min)
        return r;
    }

    int advance() {
        if (plan.hasPathEq) { eof = true; current.reset(); return SQLITE_OK; }
        if (!walker) { eof = true; return SQLITE_OK; }
        std::filesystem::path p;
        while (walker->next(p)) {
            if (plan.hasExt && fileExt(p) != plan.ext) continue;
            if (plan.hasPathLike && !sqlLike(plan.pathLike, p.string())) continue;
            current = buildRow(p);
            if (current) { ++rowid; return SQLITE_OK; }
        }
        eof = true;
        current.reset();
        return SQLITE_OK;
    }

    int Filter(int idxNum, sqlite3_value** argv) {
        int extArgv, pathArgv, pidArgv, commArgv;
        unpackIdx(idxNum, extArgv, pathArgv, pidArgv, commArgv);
        plan = IndexPlan{};
        if (extArgv) {
            if (sqlite3_value_type(argv[extArgv - 1]) == SQLITE_NULL) plan.noRows = true;
            else {
                std::string e = valText(argv[extArgv - 1]);
                if (!e.empty() && e[0] != '.') e = "." + e;   // normalize: add dot
                plan.ext = e; plan.hasExt = true;
            }
        }
        if (pathArgv) {
            if (sqlite3_value_type(argv[pathArgv - 1]) == SQLITE_NULL) plan.noRows = true;
            else {
                std::string pv = valText(argv[pathArgv - 1]);
                // A pattern with a wildcard becomes a LIKE (prefix-pruned walk);
                // a plain value is an exact path -> direct lookup, no traversal.
                if (pv.find('%') != std::string::npos || pv.find('_') != std::string::npos) {
                    plan.pathLike = pv; plan.hasPathLike = true;
                } else {
                    plan.pathEq = pv; plan.hasPathEq = true;
                }
            }
        }
        eof = false; rowid = 0; current.reset();
        if (plan.noRows) { eof = true; return SQLITE_OK; }

        // Exact path: stat the file directly (one row). Much faster and correct
        // regardless of where it lives in the tree.
        if (plan.hasPathEq) {
            std::filesystem::path ep(plan.pathEq);
            auto r = buildRow(ep);
            if (r && (!plan.hasExt || fileExt(ep) == plan.ext)) {
                current = std::move(r); ++rowid;
            } else {
                eof = true; current.reset();
            }
            return SQLITE_OK;
        }

        // Walk root: parent dir of the LIKE prefix. Even if the prefix itself is
        // not a dir (e.g. '/home/foo'), '/home/foobar' still matches; the actual
        // match is confirmed by sqlLike in advance().
        std::filesystem::path root = "/";
        if (plan.hasPathLike) {
            std::string pre = likePrefix(plan.pathLike);
            if (!pre.empty()) {
                std::filesystem::path p = std::filesystem::path(pre).parent_path();
                if (!p.empty()) root = p;
            }
        }
        walker.emplace(root);
        return advance();
    }

    int Next() { return advance(); }
};

struct FsVtab : public OSVtab {
    FsVtab() {
        cols = {
            {"path", "TEXT"}, {"name", "TEXT"}, {"size", "INTEGER"},
            {"is_dir", "INTEGER"}, {"is_file", "INTEGER"}, {"is_symlink", "INTEGER"},
            {"ext", "TEXT"}, {"mode", "INTEGER"}, {"mtime", "INTEGER"},
            {"atime", "INTEGER"}, {"ctime", "INTEGER"}, {"btime", "INTEGER"},
            {"inode", "INTEGER"}, {"nlink", "INTEGER"},
            {"uid", "INTEGER"}, {"gid", "INTEGER"},
            {"blocks", "INTEGER"}, {"blksize", "INTEGER"}, {"dev", "INTEGER"},
        };
    }

    OSCursor* CreateCursor() {
        FsCursor* c = new FsCursor();
        c->freeCursor = [](OSCursor* p) { delete static_cast<FsCursor*>(p); };
        c->doNext = [](OSCursor* p) { return static_cast<FsCursor*>(p)->advance(); };
        c->doFilter = [](OSCursor* p, int n, sqlite3_value** a) {
            return static_cast<FsCursor*>(p)->Filter(n, a);
        };
        return c;
    }

    int BestIndex(sqlite3_index_info* info) {
        int extArgv = 0, pathArgv = 0, nextArg = 1;
        double cost = 1e6;
        for (int i = 0; i < info->nConstraint; ++i) {
            auto& c = info->aConstraint[i];
            if (!c.usable) continue;
            int col = c.iColumn;
            if (c.op == SQLITE_INDEX_CONSTRAINT_EQ && col == colIndex("ext")) {
                extArgv = nextArg++;
                info->aConstraintUsage[i].argvIndex = extArgv;
                info->aConstraintUsage[i].omit = 1;
                cost = std::min(cost, 1e2);
            } else if (col == colIndex("path") &&
                       (c.op == SQLITE_INDEX_CONSTRAINT_LIKE ||
                        c.op == SQLITE_INDEX_CONSTRAINT_EQ)) {
                pathArgv = nextArg++;
                info->aConstraintUsage[i].argvIndex = pathArgv;
                info->aConstraintUsage[i].omit = 1;
                // Exact path is a single-file lookup; LIKE needs a pruned walk.
                cost = std::min(cost, c.op == SQLITE_INDEX_CONSTRAINT_EQ ? 1.0 : 1e3);
            }
        }
        info->idxNum = packIdx(extArgv, pathArgv, 0, 0);
        info->estimatedCost = cost;
        info->estimatedRows = (cost < 1e6) ? 1000 : 1000000;
        return SQLITE_OK;
    }
};

static sqlite3_module g_fs_module = makeModule<FsVtab>();

void registerFs(sqlite3* db) {
    sqlite3_create_module(db, "fs", &g_fs_module, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE fs USING fs", nullptr, nullptr, nullptr);
}
