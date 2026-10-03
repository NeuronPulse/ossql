// net_table.cpp - /proc/net/{tcp,udp,tcp6,udp6} as a virtual table.
// Decodes hex addresses to human form. Join `inode` with `fd.inode` then
// `fd.pid` to attribute a socket to its owning process (an `ss` replacement).
#include "vtab.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>

static std::vector<unsigned char> hexToBytesLE(const std::string& h) {
    std::vector<unsigned char> b(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2)
        b[i / 2] = (unsigned char)std::stoi(h.substr(i, 2), nullptr, 16);
    std::reverse(b.begin(), b.end());  // little-endian on disk -> network order
    return b;
}

static std::string decAddr(const std::string& hex, bool ipv6) {
    auto b = hexToBytesLE(hex);
    if (!ipv6) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return buf;
    }
    std::string s;
    char buf[8];
    for (size_t i = 0; i < b.size(); i += 2) {
        if (i) s += ':';
        std::snprintf(buf, sizeof buf, "%02x%02x", b[i], b[i + 1]);
        s += buf;
    }
    return s;
}

static const char* tcpState(const std::string& s) {
    static const char* m[14] = {
        "UNKNOWN", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2",
        "TIME_WAIT", "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING",
        "NEW_SYN_RECV", "UNKNOWN"};
    int v = std::stoi(s, nullptr, 16);
    return (v >= 1 && v <= 13) ? m[v] : "UNKNOWN";
}

static void parseNetFile(const std::string& path, const std::string& proto, bool ipv6,
                         std::vector<std::vector<Cell>>& out) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; }  // skip header
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> t;
        while (ss >> tok) t.push_back(tok);
        if (t.size() < 10) continue;
        auto split = [](const std::string& s, std::string& addr, long& port) {
            auto p = s.find(':');
            addr = s.substr(0, p);
            port = std::stol(s.substr(p + 1), nullptr, 16);
        };
        std::string la, ra;
        long lp = 0, rp = 0;
        split(t[1], la, lp);
        split(t[2], ra, rp);
        out.push_back({cellText(proto), cellText(decAddr(la, ipv6)), cellInt(lp),
                       cellText(decAddr(ra, ipv6)), cellInt(rp), cellText(tcpState(t[3])),
                       cellInt(std::stol(t[9]))});
    }
}

static const char* unixState(const std::string& s) {
    static const char* m[5] = {"FREE", "UNCONNECTED", "CONNECTING",
                               "CONNECTED", "DISCONNECTING"};
    int v = std::stoi(s, nullptr, 16);
    return (v >= 0 && v <= 4) ? m[v] : "UNKNOWN";
}

// /proc/net/unix: Num RefCount Protocol Flags Type St Inode Path...
// Path may contain spaces, so reconstruct it after the inode token.
static void parseUnixFile(std::vector<std::vector<Cell>>& out) {
    std::ifstream f("/proc/net/unix");
    if (!f) return;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; }  // skip header
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> t;
        while (ss >> tok) t.push_back(tok);
        if (t.size() < 7) continue;
        std::string path;
        size_t pos = line.find(t[6]);
        if (pos != std::string::npos) {
            size_t p2 = line.find_first_not_of(" \t", pos + t[6].size());
            if (p2 != std::string::npos) path = line.substr(p2);
        }
        long inode = 0;
        try { inode = std::stol(t[6]); } catch (...) { continue; }
        // proto, local_addr, local_port, remote_addr, remote_port, state, inode
        out.push_back({cellText("unix"), cellText(path), cellNull(), cellNull(),
                       cellNull(), cellText(unixState(t[5])), cellInt(inode)});
    }
}

static std::vector<std::vector<Cell>> genNet(long) {
    std::vector<std::vector<Cell>> out;
    parseNetFile("/proc/net/tcp", "tcp", false, out);
    parseNetFile("/proc/net/tcp6", "tcp6", true, out);
    parseNetFile("/proc/net/udp", "udp", false, out);
    parseNetFile("/proc/net/udp6", "udp6", true, out);
    parseUnixFile(out);
    return out;
}

struct NetVtab : public ListVtab {
    NetVtab() {
        cols = {{"proto", "TEXT"}, {"local_addr", "TEXT"}, {"local_port", "INTEGER"},
                {"remote_addr", "TEXT"}, {"remote_port", "INTEGER"},
                {"state", "TEXT"}, {"inode", "INTEGER"}};
        gen = genNet;
    }
};

static sqlite3_module g_net_mod = makeModule<NetVtab>();

void registerNet(sqlite3* db) {
    sqlite3_create_module(db, "net", &g_net_mod, nullptr);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE net USING net", nullptr, nullptr, nullptr);
}
