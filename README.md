# ossql

Query live OS state with SQL. A read-only `sqlite3` virtual-table shell over
`/proc` and the filesystem. Two tables, plain-text output, zero config.

```
$ ossql "SELECT pid, comm, rss/1048576.0 AS mb FROM proc ORDER BY rss DESC LIMIT 3"
+---------+--------+------------+
| pid     | comm   | mb         |
+---------+--------+------------+
| 1196664 | qq     | 812.4      |
| 775411  | chrome | 401.9      |
| 28967   | buddy  | 233.7      |
+---------+--------+------------+
```

## Philosophy

- **Do one thing well.** ossql is a thin SQL view over the OS. No daemon, no
  state, no writes. Every query reads `/proc` and the filesystem at call time.
- **Plain text, pipe friendly.** Output defaults to a boxed `table` on a
  terminal and a pipe-separated `list` when stdout is not a tty, so it composes
  with `grep`, `awk`, `cut`, `sort`, `xargs`:
  ```
  ossql -headers off "SELECT pid FROM proc WHERE state='Z'" | xargs -r kill
  ```
- **Small surface.** Two virtual tables (`fs`, `proc`), one helper function
  (`file_text`), and the standard sqlite dot-commands. Adding a table is ~50
  lines (see *Design*).
- **Fast by default.** Predicate pushdown prunes the walk; columns that are
  expensive to read (`cmdline`, effective ids) are lazy and only fetched when
  selected.

## Build

Requires `libsqlite3-dev` (or `sqlite3` on the system) and a C++17 compiler.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ossql "SELECT count(*) FROM proc"
```

Sanitized debug build:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g"
cmake --build build-asan -j
```

## Usage

```
ossql "SELECT ..."     run one SQL statement and exit
ossql                   interactive REPL
```

Output and mode flags (also settable as dot-commands inside the REPL):

```
-csv  -list  -table      output mode   (list is default when piped)
-headers on|off          toggle column headers
-output FILE             write the result to a `snapshot` table in FILE (sqlite)
```

REPL dot-commands: `.tables`, `.schema [name]`, `.mode csv|list|table`,
`.headers on|off`, `.separator <char>`, `.help`, `.exit`/`.quit`.

`-output FILE` writes the result set into a `snapshot` table in a real sqlite
database (rather than printing). Diffing two captures is then plain `sqlite3` —
ossql stays a one-shot reader and the `diff` is left to standard tools:

```
ossql -output before.db "SELECT pid, comm, rss FROM proc"
# ... later ...
ossql -output after.db  "SELECT pid, comm, rss FROM proc"
sqlite3 before.db "SELECT a.pid FROM snapshot a LEFT JOIN snapshot b ... "   # your diff
```

Time columns are Unix epoch seconds; render them with sqlite's `datetime()` /
`strftime()`. Permission bits are decimal (`st_mode & 07777`); display octal
with `printf('%o', mode)`.

## Tables

### `proc` — one row per process (`/proc/<pid>`)

| column     | type    | notes                                        |
|------------|---------|----------------------------------------------|
| pid        | INTEGER | process id                                   |
| ppid       | INTEGER | parent pid                                   |
| comm       | TEXT    | process name (truncated by kernel to 15c)    |
| cmdline    | TEXT    | **lazy** full command line                  |
| state      | TEXT    | `R` `S` `D` `T` `Z` `I` ...                  |
| uid        | INTEGER | real uid (from `/proc/<pid>` dir owner)      |
| gid        | INTEGER | real gid                                     |
| euid       | INTEGER | **lazy** effective uid (`/proc/<pid>/status`)|
| egid       | INTEGER | **lazy** effective gid                        |
| starttime  | INTEGER | boot-relative ticks (pid-reuse guard)        |
| rss        | INTEGER | resident memory, bytes                       |
| vsize      | INTEGER | virtual memory, bytes                        |
| threads    | INTEGER | thread count (from `stat`)                   |
| utime      | INTEGER | user CPU, clock ticks                        |
| stime      | INTEGER | system CPU, clock ticks                      |
| nice       | INTEGER | scheduling nice                             |
| priority   | INTEGER | dynamic priority                            |
| tty        | INTEGER | controlling terminal dev nr (0 = none)       |
| io_read    | INTEGER | **lazy** bytes read (`/proc/<pid>/io`)       |
| io_write   | INTEGER | **lazy** bytes written (`/proc/<pid>/io`)    |
| rlimit_nofile | INTEGER | **lazy** max open files, soft limit (`/proc/<pid>/limits`) |

Pushdown: `pid = ?` (direct lookup), `comm LIKE ?` (pre-filter by name).
`io_read`/`io_write`/`rlimit_nofile` are read lazily (only when selected) and
may be `NULL` for processes you lack permission to inspect.

### `fs` — recursive walk of the filesystem

| column    | type    | notes                                         |
|-----------|---------|-----------------------------------------------|
| path      | TEXT    | absolute path                                 |
| name      | TEXT    | basename                                      |
| size      | INTEGER | bytes (0 for dirs)                            |
| is_dir    | INTEGER | 1 if directory                                |
| is_file   | INTEGER | 1 if regular file                            |
| is_symlink| INTEGER | 1 if symlink                                  |
| ext       | TEXT    | extension incl. dot, e.g. `.go` (none = `''`)|
| mode      | INTEGER | permission bits only (`st_mode & 07777`)      |
| mtime     | INTEGER | modified time, epoch sec                      |
| atime     | INTEGER | access time, epoch sec                        |
| ctime     | INTEGER | inode change time, epoch sec                  |
| btime     | INTEGER | creation time, epoch sec (`statx`; 0 if n/a) |
| inode     | INTEGER | inode number                                  |
| nlink     | INTEGER | hard link count                               |
| uid       | INTEGER | owner uid                                     |
| gid       | INTEGER | owner gid                                     |
| blocks    | INTEGER | disk blocks used (512 B units; true footprint)|
| blksize   | INTEGER | preferred I/O block size                     |
| dev       | INTEGER | device id (maj*256+min)                       |

Pushdown: `ext = ?` (exact match), `path LIKE '/prefix%'` (prunes the walk
root to the prefix's parent — fast), `path = '/exact/file'` (single `stat`,
no traversal). **No constraint scans from `/`** and is slow by nature; always
scope with `path LIKE`.

### `file_text(path)` — helper function

Reads a file's content (capped at 16 MB) for in-SQL content search. Combine
with `fs` to grep the filesystem:

```
SELECT path FROM fs WHERE path LIKE '/etc/%' AND file_text(path) LIKE '%PasswordAuthentication no%';
```

### `file_grep(path, pattern)` — helper function

Returns `1` if the file (capped at 16 MB) contains `pattern`, else `0`. Cheaper
than `file_text` when you only need a match test:

```
SELECT name FROM fs WHERE path LIKE '/home/user/project/%' AND file_grep(path, 'Unix philosophy') = 1;
```

## More tables

The same virtual-table machinery backs several read-only views over other OS
sources. They join freely with `proc`/`fs` (all keyed by `pid`/`uid`/`inode`).

### `passwd` and `group` — `/etc/passwd`, `/etc/group`

Resolve numeric `uid`/`gid` to names and list group membership:

| passwd column | type |  | group column | type |
|---|---|---|---|---|
| name | TEXT |  | name | TEXT |
| uid  | INTEGER |  | gid | INTEGER |
| gid  | INTEGER |  | members | TEXT (comma-separated) |
| gecos | TEXT |  | | |
| home | TEXT |  | | |
| shell | TEXT |  | | |

```
SELECT p.name, count(*) AS procs FROM proc JOIN passwd p ON proc.uid = p.uid
GROUP BY p.name ORDER BY procs DESC;
```

### `net` — `/proc/net/{tcp,udp,tcp6,udp6}`

Open sockets with decoded addresses/states. `inode` joins to `fd.inode` then
`fd.pid` to attribute a socket to its process (an `ss`/`netstat` replacement):

| column | type | notes |
|---|---|---|
| proto | TEXT | `tcp` `tcp6` `udp` `udp6` |
| local_addr | TEXT | dotted IP (IPv4) or `:`-groups (IPv6) |
| local_port | INTEGER | |
| remote_addr | TEXT | |
| remote_port | INTEGER | |
| state | TEXT | `LISTEN` `ESTABLISHED` `TIME_WAIT` ... |
| inode | INTEGER | socket/pipe inode (joins `fd`) |

`proto` also includes `unix` rows from `/proc/net/unix`: `local_addr` is the
socket path (or empty for abstract sockets) and `local_port`/`remote_port` are
`NULL`. Their `inode` joins `fd` exactly like TCP/UDP, so you can attribute a
Unix-domain socket to its owning process.

```
SELECT n.local_port, f.pid, pr.comm
FROM net n JOIN fd f ON f.inode = n.inode JOIN proc pr ON pr.pid = f.pid
WHERE n.state = 'LISTEN';
```

### `system` — one-row host snapshot

A single row with uptime, load average, memory, CPU count, and boot time,
read from `/proc/uptime`, `/proc/loadavg`, `/proc/meminfo`, `/proc/stat`, and
`/proc/sys/kernel/hostname`. A zero-traversal view that replaces a one-off
`uptime`/`free`/`nproc`:

| column | type | notes |
|---|---|---|
| hostname | TEXT | `kernel.hostname` |
| uptime | REAL | seconds since boot |
| idle | REAL | seconds idle |
| load1 / load5 / load15 | REAL | 1/5/15-min load average |
| boot_time | INTEGER | boot epoch seconds (`datetime()`-friendly) |
| nproc | INTEGER | logical CPU count |
| mem_total / mem_free / mem_available / buffers / cached | INTEGER | bytes |

```
SELECT hostname, printf('%.2f', load1) AS load1, nproc,
       printf('%.1f', mem_available/1048576.0)||'M' AS avail
FROM system;
```

### `fd` — `/proc/<pid>/fd`

Open file descriptors per process (an `lsof` replacement). `pid = ?` is pushed
down so only that process is scanned; without it, every process is scanned
(slow). `inode` matches `net.inode` for sockets/pipes.

| column | type | notes |
|---|---|---|
| pid | INTEGER | pushdown: `pid = ?` |
| fd | INTEGER | descriptor number |
| path | TEXT | symlink target (file path, or `socket:[n]`/`pipe:[n]`) |
| type | TEXT | `file` `socket` `pipe` `anon_inode` `memfd` |
| inode | INTEGER | socket/pipe inode (0 for regular files) |

### `env` — `/proc/<pid>/environ`

One row per environment variable. `pid = ?` is pushed down; without it, every
process is scanned (slow), so always scope it:

```
SELECT key, value FROM env WHERE pid = 1234 AND key IN ('HOME','PATH');
```

### `mounts` — `/proc/self/mountinfo`

| column | type |
|---|---|
| id | INTEGER |
| parent | INTEGER |
| dev | TEXT (`maj:min`) |
| root | TEXT |
| mountpoint | TEXT |
| fstype | TEXT |
| source | TEXT |
| opts | TEXT |

## Examples

Find zombies and uninterruptibly-sleeping processes:

```sql
SELECT pid, comm, ppid, state FROM proc
WHERE state IN ('Z','D','T') ORDER BY state;
```

Who is using the most memory, by user:

```sql
SELECT uid, count(*) AS procs, printf('%.1f', sum(rss)/1048576.0)||'M' AS mem
FROM proc GROUP BY uid ORDER BY sum(rss) DESC LIMIT 5;
```

Setuid binaries (privilege-escalation surface). Note: sqlite has **no octal
literals** — setuid is decimal `2048`:

```sql
SELECT name, printf('%o', mode) AS mode FROM fs
WHERE path LIKE '/usr/bin/%' AND (mode & 2048) != 0 ORDER BY name;
```

Cross-table join: running processes and the size of their on-disk binary:

```sql
SELECT p.pid, p.comm, f.size AS bin_bytes FROM proc p
JOIN fs f ON f.name = p.comm AND f.is_file = 1
WHERE f.path LIKE '/usr/bin/%' ORDER BY f.size DESC LIMIT 5;
```

Disk cleanup: files over 1 MB untouched for two years:

```sql
SELECT path, printf('%.1f', size/1048576.0)||'M' AS mb,
       datetime(mtime, 'unixepoch') AS modified
FROM fs WHERE is_file = 1
  AND mtime < CAST(strftime('%s','now','-730 days') AS INTEGER)
  AND size > 1048576 AND path LIKE '/usr/%'
ORDER BY size DESC LIMIT 8;
```

Recently created files in a tree (`btime` comes from `statx`):

```sql
SELECT name, datetime(btime, 'unixepoch', 'localtime') AS created FROM fs
WHERE btime > 0 AND path LIKE '/home/user/project/%' ORDER BY btime DESC LIMIT 10;
```

Security sweep: root-owned and world-writable (should be empty):

```sql
SELECT count(*) AS risky FROM fs
WHERE path LIKE '/usr/bin/%' AND uid = 0 AND (mode & 2) != 0;
```

Pipe it (Unix way):

```bash
ossql -headers off "SELECT pid, comm FROM proc WHERE uid = 0" | wc -l
ossql "SELECT comm, count(*) FROM proc GROUP BY comm" | sort -t'|' -k2 -n
```

Resolve numeric ids to names (join `passwd`/`group`):

```sql
SELECT p.name AS user, g.name AS group, count(*) AS procs
FROM proc JOIN passwd p ON proc.uid = p.uid
JOIN "group" g ON proc.gid = g.gid
GROUP BY p.name, g.name ORDER BY procs DESC LIMIT 5;
```

Attribute a listening socket to its process (`net` -> `fd` -> `proc`):

```sql
SELECT n.proto, n.local_port, pr.pid, pr.comm
FROM net n JOIN fd f ON f.inode = n.inode
JOIN proc pr ON pr.pid = f.pid
WHERE n.state = 'LISTEN' ORDER BY n.local_port;
```

Find which process holds a given file (`fd` -> `fs`):

```sql
SELECT f.pid, pr.comm, d.path FROM fd d
JOIN proc pr ON pr.pid = d.pid
JOIN fs s ON s.path = d.path
WHERE s.name = 'ossql' AND d.type = 'file';
```

## Design

- One shared set of generic sqlite vtab callbacks (`vtab.h`). A new table
  subclasses `OSVtab`, implements `CreateCursor()`/`BestIndex()`, and calls
  `makeModule<T>()`. No boilerplate per table.
- `OSCursor` keeps `sqlite3_vtab_cursor` at offset 0 (no vtable pointer), so
  dispatch uses type-erased function pointers (`doNext`/`doFilter`/`lazyGet`)
  instead of virtual methods.
- Predicate pushdown is encoded in `idxNum` as four 8-bit `argvIndex` slots
  (`ext`, `path`, `pid`, `comm`); the optimizer reuses one cursor type safely.
- `Cell::Lazy` + `OSCursor::lazyGet` defer expensive reads (`/proc/<pid>/cmdline`,
  effective ids) until the column is actually projected.
- `FsWalker` is a manual DFS frame stack, not `recursive_directory_iterator`,
  so one unreadable directory (e.g. `/dev`) skips instead of aborting the
  whole scan, and symlink directories are reported but not descended into.
- Times are stored as raw epoch seconds / clock ticks so sqlite's built-in
  `datetime()`/`strftime()` and arithmetic work directly.

## Limitations

- Read-only. No inserts, updates, or deletes.
- `fs` without a `path` constraint walks from `/`; scope queries.
- `btime` is 0 where the filesystem lacks creation-time support.
- `proc.comm` is the kernel-truncated 15-char name; use `cmdline` for the full
  command.
- Unix only (relies on `/proc` and `statx`).

## Roadmap

Implemented (each a read-only virtual table or helper, reusing the same
machinery): `system`, `passwd`, `group`, `mounts`, `net` (incl. Unix-domain
sockets), `fd`, `env`, the `file_grep` function, `proc` I/O + rlimit columns
(`io_read`/`io_write`/`rlimit_nofile`), `fs` disk-usage columns
(`blocks`/`blksize`/`dev`), and `-output` snapshot capture.

Possible future, kept small and in-spirit:

- `cgroup` / `mem` — per-process or system cgroup limits from
  `/sys/fs/cgroup` (memory/CPU caps), keyed by `pid`.
- `sessions` — `utmp`/`loginctl` style login records, joined to `proc`.
- A `diff` helper that compares two `-output` snapshots directly (still leaves
  the actual diffing to `sqlite3`, per the Unix philosophy).

Out of scope (would break the read-only, one-shot-reader model): live CPU
percentages (need two samples), and any writes.
