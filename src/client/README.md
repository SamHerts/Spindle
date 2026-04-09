# Spindle Client

The client component is injected into each application process and is responsible for transparently intercepting dynamic library and file I/O calls, routing them through the Spindle distributed cache system.

## Overview

At job startup, the front-end sets `LD_AUDIT` to point to a Spindle audit library. When the application launches, glibc loads the audit library and calls Spindle's hooks before resolving any library names. Spindle rewrites the paths to point to the local server daemon's cache, which holds files already broadcast from the shared filesystem.

For file I/O calls (`open`, `stat`, `exec`), Spindle patches the application's GOT to redirect those calls to its own wrappers, which similarly route requests through the local server.

## Directory Structure

```
src/client/
├── auditclient/      - rtld-audit interface: intercepts dlopen/library loading
├── client/           - Core client logic: file lookup, system call interception
├── client_comlib/    - Client-to-server communication (socket, pipe, biter backends)
├── beboot/           - Bootstrap program: runs once per node to init server connection
├── spindle_api/      - Public C API (libspindle.so, spindle.h)
├── subaudit/         - Workaround for glibc rtld-audit bugs
├── shm_cache/        - Per-node shared memory cache
├── ldsolookup/       - Utilities for inspecting the dynamic linker
├── logging/          - Logging infrastructure
└── biter/            - Binary tree collective communication protocol
```

## Subsystems

### auditclient — Library Load Interception

Uses glibc's rtld-audit interface to hook into the dynamic linker. Key callbacks:

- `la_objsearch()` — called when resolving a library name; rewrites the path to the cached location by calling into the core client.
- `la_objopen()` — called after a library is loaded; triggers GOT patching.
- `la_activity()` — called on loader state transitions; finalizes data relocations and DTV fixups.

Architecture-specific files (`auditclient_x86_64.c`, `auditclient_aarch64.c`, `auditclient_ppc64.c`) handle ISA-specific GOT layout differences. `writablegot.c` and `bindgot.c` handle making read-only GOT sections writable via `mprotect` and patching them.

### client — Core Logic

Manages client state and implements interception wrappers:

- **`client.c`** — initializes the client from environment variables set by the front-end (`LDCS_LOCATION`, `LDCS_CONNECTION`, `LDCS_OPTIONS`, etc.), opens the server connection.
- **`intercept_open.c`**, **`intercept_stat.c`**, **`intercept_readlink.c`** — wrappers for `open`/`fopen`, `stat`/`lstat`, and `readlink` that redirect calls through Spindle when appropriate.
- **`intercept_exec.c`** — intercepts `execv`/`execve`/`fork` variants to propagate Spindle into child processes.
- **`lookup.c`**, **`should_intercept.c`** — determine whether a given file should go through Spindle, check the shared memory cache first, and query the server if needed.
- **`realpath.c`** — custom `realpath` implementation that resolves symlinks through Spindle rather than directly accessing the filesystem.
- **`patch_interception.c`** — installs GOT patches so that libc calls within the application are redirected to Spindle's wrappers.

### client_comlib — Communication Backends

Three pluggable backends for messaging between the client and its local server daemon:

| Backend | File | Use case |
|---------|------|----------|
| Socket | `client_api_socket.c` | TCP over loopback |
| Pipe | `client_api_pipe.c` | Named pipes (localhost only) |
| Biter | `client_api_biter.c` | Collective tree-based protocol |

All backends share the same message API (`client_api.h`): `send_file_query()`, `send_stat_request()`, `send_dirlists_request()`, etc. The build produces one library variant per backend (`libclient_socket.la`, etc.), and the audit library links against the appropriate one.

### beboot — Node Bootstrap

`spindle_bootstrap` is a small, statically-linked executable that runs once per node before the application starts:

1. Reads Spindle environment variables set by the front-end.
2. Opens a connection to the local server daemon.
3. Pre-populates the shared memory cache with connection metadata.
4. Propagates the server connection string to child processes via environment variables.

### spindle_api — Public C API

`libspindle.so` exposes a stable C interface for applications that want to interact with Spindle explicitly:

```c
int spindle_is_present(void);

// Spindle-aware I/O (route through cache when present)
int   spindle_open(const char *path, int flags, ...);
int   spindle_stat(const char *path, struct stat *buf);
int   spindle_lstat(const char *path, struct stat *buf);
FILE *spindle_fopen(const char *path, const char *mode);

// Per-thread enable/disable (reference-counted)
void enable_spindle(void);
void disable_spindle(void);
int  is_spindle_enabled(void);
```

When Spindle is not active, `spindle_api.c` provides default implementations that forward to standard libc. When Spindle is active, `intercept_spindleapi.c` overrides these with the cache-aware implementations.

See `doc/spindle_launch_README.md` for full API documentation.

### subaudit — Glibc Audit Workaround

Some glibc versions have bugs where the standard audit interface does not reliably fire symbol binding notifications or corrupts TLS/DTV state. The subaudit path works around this with a two-stage mechanism:

1. **`libspindleint.so`** (`preloadlib.c`) — a minimal preload library that loads the real audit library after glibc has initialized.
2. **`update_pltbind.c`** — after libraries are loaded, manually walks all loaded objects and rewrites PLT entries to point to the correct relocated symbols, bypassing the unreliable audit hooks.
3. **`subaudit.c`** — coordinates the above and manages memory protection (`mprotect`) around PLT modifications.

### shm_cache — Shared Memory Cache

Multiple application processes on the same node share a single cache in shared memory to avoid sending duplicate queries to the server. The cache is a hash table with LRU eviction backed by the "sheep" allocator:

- `shmcache_lookup_or_add()` — atomic lookup; marks entry as in-progress on miss.
- `shmcache_waitfor_update()` — other processes block until the first resolver writes the result.
- `shmcache_update()` — writes the resolved path, waking any waiters.

## Build Outputs

| Artifact | Location | Description |
|----------|----------|-------------|
| `libspindle.so` | `$(libdir)` | Public API library (installed) |
| `spindle.h` | `$(includedir)` | Public API header (installed) |
| `libspindle_audit_{socket,pipe,biter}.so` | `$(pkglibdir)` | Audit libraries (one per comm backend) |
| `libspindle_subaudit_{socket,pipe,biter}.so` | `$(pkglibdir)` | Subaudit libraries |
| `libspindleint.so` | `$(pkglibdir)` | Preload library for subaudit path |
| `spindle_bootstrap` | `$(pkglibexecdir)` | Node bootstrap executable |

## Environment Variables

These are set by the front-end and read by the client at startup:

| Variable | Description |
|----------|-------------|
| `LDCS_LOCATION` | Path to the local server cache directory |
| `LDCS_ORIG_LOCATION` | Symbolic/canonical cache path |
| `LDCS_CONNECTION` | Connection string to the local server daemon |
| `LDCS_NUMBER` | Process rank |
| `LDCS_RANKINFO` | Rank topology (`mylrank:mylsize:mdrank:mdsize`) |
| `LDCS_OPTIONS` | Bitmask of options (`OPT_FOLLOWFORK`, `OPT_SHMCACHE`, etc.) |
| `LDCS_CACHESIZE` | Shared memory cache size in KB |
| `LD_AUDIT` | Points to the appropriate `libspindle_audit_*.so` |

## Data Flow

```
Application process
    │
    ├─ LD_AUDIT → libspindle_audit_*.so
    │       │
    │   la_objsearch()         ← library name interception
    │   la_objopen()           ← GOT patching after load
    │   la_activity()          ← DTV/relocation fixups
    │
    └─ GOT patches → open/stat/exec wrappers (intercept_*.c)
            │
            ▼
      client/lookup.c
            │
      ┌─────┴──────┐
      │             │
  shm_cache     (cache miss)
  (cache hit)        │
      │         client_comlib
      │         (socket/pipe/biter)
      │              │
      └──────────────┘
                 ▼
         local server daemon
```
