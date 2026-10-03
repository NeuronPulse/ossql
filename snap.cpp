// snap.cpp - OS state parsers: file extension, /proc process info, file read.
#include "vtab.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

std::string fileExt(const std::filesystem::path& p) {
    std::string name = p.filename().string();
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return std::string();
    return name.substr(dot);
}

std::vector<long> listProcPids() {
    std::vector<long> pids;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator("/proc", ec)) {
        if (!e.is_directory()) continue;
        std::string name = e.path().filename().string();
        if (name.empty() || !std::isdigit((unsigned char)name[0])) continue;
        try { pids.push_back(std::stol(name)); }
        catch (...) { /* skip non-numeric */ }
    }
    return pids;
}

bool readProcInfo(long pid, ProcInfo& out) {
    out = ProcInfo{};
    out.pid = pid;

    // uid/gid: owner of /proc/<pid> (process real uid/gid). Avoids reading status.
    struct stat dst;
    if (lstat(("/proc/" + std::to_string(pid)).c_str(), &dst) == 0) {
        out.uid = dst.st_uid;
        out.gid = dst.st_gid;
    }

    // stat: comm/state/ppid/starttime/rss/threads.
    {
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
        if (!f) return false;
        std::string line;
        std::getline(f, line);
        if (line.empty()) return false;
        // comm may contain spaces/parens; split on outer parens.
        auto ob = line.find('(');
        auto cb = line.find(')', ob);
        if (ob == std::string::npos || cb == std::string::npos) return false;
        out.comm = line.substr(ob + 1, cb - ob - 1);
        std::istringstream iss(line.substr(cb + 1));
        std::string st;
        long ppid = 0;
        if (!(iss >> st >> ppid)) return false;
        out.state = st.empty() ? '?' : st[0];
        out.ppid = ppid;
        // Tokens after ')': fields 5..24. Capture the commonly useful ones:
        //   tty_nr=7  utime=14  stime=15  priority=18  nice=19  num_threads=20
        //   starttime=22  vsize=23 (bytes)  rss=24 (pages)
        long tok;
        for (int i = 0; i < 20; ++i) {
            if (!(iss >> tok)) break;
            switch (i) {
                case 2:  out.tty = tok; break;        // tty_nr
                case 9:  out.utime = tok; break;      // user CPU (ticks)
                case 10: out.stime = tok; break;      // system CPU (ticks)
                case 13: out.priority = tok; break;
                case 14: out.nice = tok; break;
                case 15: out.threads = tok; break;    // num_threads
                case 17: out.starttime = tok; break;
                case 18: out.vsize = tok; break;      // bytes
                case 19: out.rss = tok; break;        // pages
                default: break;
            }
        }
        static long pagesize = sysconf(_SC_PAGESIZE);
        out.rss *= pagesize;
    }
    return true;
}

// Lazy: only read when the cmdline column is actually selected/referenced.
std::string readProcCmdline(long pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f) return std::string();
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::replace(buf.begin(), buf.end(), '\0', ' ');
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();
    return buf;
}

bool readFileText(const std::string& path, std::string& out, size_t maxBytes) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return false;
    if (size > maxBytes) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}
