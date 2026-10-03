// main.cpp - CLI: open an in-memory DB, register virtual tables/functions, run SQL.
// Unix philosophy: plain-text output that pipes cleanly to grep/awk/cut.
//   ossql "SELECT ..."     run one SQL statement
//   ossql                   interactive REPL (.tables/.schema/.mode/.headers/.exit)
//   -csv -list -table      output mode (-list is the default when output is piped)
//   -headers on|off        toggle column headers
#include "vtab.h"
#include <sqlite3.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <unistd.h>

enum class Mode { Table, Csv, List };

struct OutOpts {
    Mode mode = Mode::Table;
    bool headers = true;
    char sep = '|';   // List-mode column separator
    sqlite3* outDb = nullptr;  // if set, write result to a `snapshot` table here
};

static const char* modeName(Mode m) {
    return m == Mode::Csv ? "csv" : (m == Mode::List ? "list" : "table");
}

static std::string colText(sqlite3_stmt* stmt, int c) {
    if (sqlite3_column_type(stmt, c) == SQLITE_NULL) return std::string();
    const unsigned char* t = sqlite3_column_text(stmt, c);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

static int writeSnapshot(sqlite3* out, const std::vector<std::string>& header,
                         const std::vector<std::vector<std::string>>& rows);

// Run one prepared statement, collect all rows, print in the chosen mode.
static int runStatement(sqlite3* db, sqlite3_stmt* stmt, const OutOpts& o, std::ostream& out) {
    int ncol = sqlite3_column_count(stmt);
    std::vector<std::string> header;
    for (int c = 0; c < ncol; ++c) header.push_back(sqlite3_column_name(stmt, c));

    std::vector<std::vector<std::string>> rows;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows.emplace_back();
        for (int c = 0; c < ncol; ++c) rows.back().push_back(colText(stmt, c));
    }
    if (rc != SQLITE_DONE) {
        std::cerr << "error: " << sqlite3_errmsg(db) << "\n";
        return rc;
    }
    if (ncol == 0) return SQLITE_OK;  // non-SELECT: no output

    // Snapshot mode: persist the result to a real sqlite file (a `snapshot`
    // table) for later diffing, instead of printing. Diffing is then just
    // plain sqlite3 -- ossql stays a one-shot reader.
    if (o.outDb) return writeSnapshot(o.outDb, header, rows);

    if (o.mode == Mode::Csv) {
        auto csvCell = [](const std::string& s) -> std::string {
            if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
            std::string r = "\"";
            for (char ch : s) { if (ch == '"') r += "\"\""; else r += ch; }
            return r + "\"";
        };
        if (o.headers) {
            for (size_t c = 0; c < header.size(); ++c) { if (c) out << ','; out << csvCell(header[c]); }
            out << "\n";
        }
        for (auto& r : rows) {
            for (size_t c = 0; c < r.size(); ++c) { if (c) out << ','; out << csvCell(r[c]); }
            out << "\n";
        }
    } else if (o.mode == Mode::List) {
        auto joinRow = [&](const std::vector<std::string>& cells) {
            for (size_t c = 0; c < cells.size(); ++c) { if (c) out << o.sep; out << cells[c]; }
            out << "\n";
        };
        if (o.headers) joinRow(header);
        for (auto& r : rows) joinRow(r);
    } else {
        // Table: box-drawn, padded to column width.
        std::vector<size_t> w(header.size(), 0);
        for (size_t c = 0; c < header.size(); ++c) w[c] = header[c].size();
        for (auto& r : rows)
            for (size_t c = 0; c < r.size(); ++c)
                w[c] = std::max(w[c], r[c].size());
        auto line = [&](const std::vector<std::string>& cells) {
            out << "+";
            for (size_t c = 0; c < cells.size(); ++c) out << std::string(w[c] + 2, '-') << "+";
            out << "\n";
        };
        auto rowOut = [&](const std::vector<std::string>& cells) {
            out << "|";
            for (size_t c = 0; c < cells.size(); ++c)
                out << " " << cells[c] << std::string(w[c] - cells[c].size(), ' ') << " |";
            out << "\n";
        };
        line(header); rowOut(header); line(header);
        for (auto& r : rows) rowOut(r);
        line(header);
    }
    return SQLITE_OK;
}

// Write a result set into a `snapshot` table in the output database. Column
// types are inferred per-column (INTEGER if every non-empty value is numeric).
static int writeSnapshot(sqlite3* out, const std::vector<std::string>& header,
                         const std::vector<std::vector<std::string>>& rows) {
    auto isIntVal = [](const std::string& v) -> bool {
        if (v.empty()) return false;
        size_t i = (v[0] == '-') ? 1 : 0;
        if (i >= v.size()) return false;
        for (; i < v.size(); ++i) if (!std::isdigit((unsigned char)v[i])) return false;
        return true;
    };
    std::string colDefs;
    for (size_t c = 0; c < header.size(); ++c) {
        if (c) colDefs += ",";
        bool allInt = true;
        for (auto& r : rows) { if (!isIntVal(r[c])) { allInt = false; break; } }
        colDefs += "\"" + header[c] + "\" " + (allInt ? "INTEGER" : "TEXT");
    }
    char* err = nullptr;
    if (sqlite3_exec(out, ("CREATE TABLE snapshot(" + colDefs + ")").c_str(),
                     nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "snapshot error: " << (err ? err : "") << "\n";
        sqlite3_free(err);
        return SQLITE_ERROR;
    }
    std::string ph;
    for (size_t i = 0; i < header.size(); ++i) { if (i) ph += ","; ph += "?"; }
    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(out, ("INSERT INTO snapshot VALUES(" + ph + ")").c_str(),
                           -1, &ins, nullptr) != SQLITE_OK) {
        std::cerr << "snapshot prep error: " << sqlite3_errmsg(out) << "\n";
        return SQLITE_ERROR;
    }
    for (auto& r : rows) {
        for (size_t c = 0; c < r.size(); ++c) {
            const std::string& v = r[c];
            if (v.empty()) sqlite3_bind_null(ins, (int)c + 1);
            else if (isIntVal(v)) sqlite3_bind_int64(ins, (int)c + 1, std::stoll(v));
            else sqlite3_bind_text(ins, (int)c + 1, v.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return SQLITE_OK;
}

// Run a whole SQL string (multiple statements), stop on first error.
static int runSql(sqlite3* db, const std::string& sql, const OutOpts& o, std::ostream& out) {
    if (o.outDb) sqlite3_exec(o.outDb, "DROP TABLE IF EXISTS snapshot;", nullptr, nullptr, nullptr);
    const char* ptr = sql.c_str();
    int rc = SQLITE_OK;
    while (ptr && *ptr) {
        sqlite3_stmt* stmt = nullptr;
        const char* tail = nullptr;
        rc = sqlite3_prepare_v2(db, ptr, -1, &stmt, &tail);
        if (rc != SQLITE_OK) {
            std::cerr << "SQL error: " << sqlite3_errmsg(db) << "\n";
            return rc;
        }
        if (stmt) {
            rc = runStatement(db, stmt, o, out);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_OK) return rc;
        }
        ptr = tail;
    }
    return SQLITE_OK;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static int repl(sqlite3* db, OutOpts& o) {
    std::string line;
    std::cout << "ossql> ";
    while (std::getline(std::cin, line)) {
        std::string s = trim(line);
        if (s.empty()) { std::cout << "ossql> "; continue; }
        if (s[0] == '.') {
            if (s == ".exit" || s == ".quit") break;
            else if (s == ".tables")
                runSql(db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name", o, std::cout);
            else if (s.rfind(".mode", 0) == 0) {
                std::string m = trim(s.substr(5));
                if (m == "csv") o.mode = Mode::Csv;
                else if (m == "list") o.mode = Mode::List;
                else if (m == "table") o.mode = Mode::Table;
                else if (m.empty()) std::cout << "mode: " << modeName(o.mode) << "\n";
                else std::cout << "usage: .mode csv|list|table\n";
            }
            else if (s.rfind(".headers", 0) == 0) {
                std::string m = trim(s.substr(8));
                if (m == "on") o.headers = true;
                else if (m == "off") o.headers = false;
                else if (m.empty()) std::cout << "headers: " << (o.headers ? "on" : "off") << "\n";
                else std::cout << "usage: .headers on|off\n";
            }
            else if (s.rfind(".separator", 0) == 0) {
                std::string m = trim(s.substr(10));
                if (!m.empty()) o.sep = m[0];
                else std::cout << "usage: .separator <char>\n";
            }
            else if (s.rfind(".schema", 0) == 0) {
                std::string arg = trim(s.substr(7));
                std::string q = "SELECT sql FROM sqlite_master WHERE sql IS NOT NULL";
                if (!arg.empty()) {
                    std::string esc;
                    for (char ch : arg) { if (ch == '\'') esc += "''"; else esc += ch; }
                    q += " AND name='" + esc + "'";
                }
                runSql(db, q, o, std::cout);
            }
            else if (s == ".help") {
                std::cout << ".tables               list virtual tables\n"
                          << ".schema [name]        show CREATE TABLE\n"
                          << ".mode csv|list|table  set output mode\n"
                          << ".headers on|off       toggle column headers\n"
                          << ".separator <char>     set List-mode separator\n"
                          << ".exit / .quit         quit\n";
            }
            else std::cout << "unknown command: " << s << " (try .help)\n";
            std::cout << "ossql> ";
            continue;
        }
        runSql(db, s, o, std::cout);
        std::cout << "ossql> ";
    }
    std::cout << "\n";
    return SQLITE_OK;
}

int main(int argc, char** argv) {
    OutOpts o;
    // Default mode: pretty table on a terminal, pipe-friendly list otherwise.
    if (!isatty(STDOUT_FILENO)) o.mode = Mode::List;

    std::string sql;
    std::string outFile;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-csv") o.mode = Mode::Csv;
        else if (a == "-list") o.mode = Mode::List;
        else if (a == "-table") o.mode = Mode::Table;
        else if (a.rfind("-headers", 0) == 0) {
            std::string v = a.find('=') != std::string::npos
                ? a.substr(a.find('=') + 1)
                : (i + 1 < argc ? std::string(argv[++i]) : std::string());
            o.headers = (v == "on" || v == "1");
        }
        else if (a == "-output" || a == "-o") {
            outFile = (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        }
        else if (a == "-V" || a == "--version") {
            std::cout << "ossql 0.1.0\n";
            return 0;
        }
        else if (a == "-h" || a == "--help") {
            std::cout << "ossql - query OS state with SQL\n"
                      << "usage:\n"
                      << "  ossql \"SELECT ...\"     run one SQL statement\n"
                      << "  ossql                  interactive REPL\n"
                      << "  -csv -list -table      output mode (list is default when piped)\n"
                      << "  -headers on|off        toggle column headers\n"
                      << "  -output FILE           write result to a `snapshot` table in FILE (sqlite)\n"
                      << "  -V, --version          show version\n";
            return 0;
        }
        else if (!sql.empty()) { sql += "\n"; sql += a; }
        else sql = a;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        std::cerr << "cannot open in-memory database: " << sqlite3_errmsg(db) << "\n";
        return 1;
    }
    registerFs(db);
    registerProc(db);
    registerSystem(db);
    registerCgroup(db);
    registerPasswd(db);
    registerGroup(db);
    registerMounts(db);
    registerNet(db);
    registerFd(db);
    registerEnv(db);
    registerOssqlFunctions(db);

    sqlite3* outDb = nullptr;
    if (!outFile.empty()) {
        if (sqlite3_open(outFile.c_str(), &outDb) != SQLITE_OK) {
            std::cerr << "cannot open output db: " << sqlite3_errmsg(outDb) << "\n";
            sqlite3_close(db);
            return 1;
        }
        o.outDb = outDb;
    }

    int rc = SQLITE_OK;
    if (!sql.empty()) rc = runSql(db, sql, o, std::cout);
    else rc = repl(db, o);
    sqlite3_close(db);
    if (outDb) sqlite3_close(outDb);
    return rc == SQLITE_OK ? 0 : 2;
}
