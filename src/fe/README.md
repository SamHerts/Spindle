# Spindle Front-End

The front-end runs on the login or launch node. It is the entry point for the user (`spindle [options] <launcher> <app>`), coordinates startup of the server daemons on compute nodes, and acts as the root of the COBO collective communication tree for the duration of the job.

## Overview

The front-end has three responsibilities:

1. **Parse** the user's command line, detect the job launcher (`srun`, `mpiexec`, `flux`, etc.), and inject Spindle's preload libraries into the application environment.
2. **Bootstrap** — spawn server daemons on every compute node and broadcast configuration to them via the COBO tree.
3. **Drive the session** — wait for all jobs to complete, then cleanly shut down daemons and free resources.

## Directory Structure

```
src/fe/
├── startup/            - Core coordination: launcher classes, session management, config
├── comlib/             - FE-side COBO tree root and broadcast primitives
├── cobo/               - Collective communication (tree-based broadcast/gather)
├── hostbin/            - Direct node-launch mechanism (no resource manager)
├── openmpi_intercept/  - OpenMPI ORTE launcher integration
├── launchmon/          - LaunchMON middleware integration
├── flux/               - Flux resource manager integration
└── logging/            - FE logging infrastructure
```

## Subsystems

### startup — Core Coordination

Contains the `main()` entry point and the principal logic for driving a Spindle session.

#### Entry point and event loop (`spindle_fe_main.cc`)

After parsing the command line and constructing a `Launcher` object, `main()` runs a state-machine event loop over `JobTask` events:

| Task | Action |
|------|--------|
| `init` | Call `spindleInitFE()`: open COBO root, broadcast settings, wait for daemons to acknowledge |
| `launch` | Modify application argv, spawn the job |
| `jobdone` | Collect application exit codes, notify session manager |
| `sessionshutdown` | Call `spindleCloseFE()`: close COBO tree, clean up security artifacts |
| `daemondone` | Reap daemon processes |

#### FE initialization (`spindle_fe.cc`)

`spindleInitFE()` opens the FE server (COBO tree root), packs a `spindle_args_t` struct containing port, session number, unique ID, and option flags, broadcasts it down the tree to all server daemons, then blocks until every daemon sends an alive acknowledgement (60-second timeout).

`spindleCloseFE()` broadcasts a shutdown message, closes the COBO tree, and cleans up key files or MUNGE credentials.

#### Launcher classes

Each resource manager has a `Launcher` subclass:

| Class | File | Resource manager |
|-------|------|-----------------|
| `SlurmLauncher` | `launch_slurm.cc` | SLURM — reads `SLURM_NODELIST`/`SLURM_NNODES` |
| `FluxLauncher` | `launch_flux.cc` | Flux — uses Flux KVS and `flux exec` |
| `LSFLauncher` | `launch_lsf.cc` | LSF/jsrun |
| `SerialLauncher` | `spindle_fe_serial.cc` | Single-node (no scheduler) |
| `HostbinLauncher` | `hostbin/launch_hostbin.cc` | Manual hostlist, no resource manager |
| `LaunchmonLauncher` | `launchmon/spindle_fe_lmon.cc` | LaunchMON/MRNET (legacy) |

Every launcher implements two methods:
- `setupDaemons()` — builds daemon argv and spawns `spindle_be` on every node.
- `setupJob()` / `spawnJob()` — modifies application argv to inject Spindle libraries and submits the job.

#### Session management (`spindle_session.cc`)

Supports multi-job sessions where a single set of daemons serves several sequential jobs without restarting. A Unix domain socket provides IPC between the session coordinator and individual job invocations. Each session has a random 8-character ID and a job queue.

#### Configuration (`config_mgr.cc`, `parse_launcher_args.cc`)

`config_mgr.cc` maps command-line flags and config file entries to `spindle_args_t`. It defines all tunable options: relocation policy, security model, port range, cache size, network parameters, and resource manager hints.

`parse_launcher_args.cc` detects the job launcher from argv (`srun`, `mpiexec`, `mpirun`, `jsrun`, `flux run`, etc.) and injects Spindle's preload libraries into the appropriate position. `LauncherParser` subclasses handle launcher-specific argument syntax.

### comlib — COBO Tree Root

Implements the FE side of the collective communication tree.

| File | Description |
|------|-------------|
| `cobo_fe_comm.c` | Primary implementation. Calls `cobo_server_open()` to bind the root listening socket, broadcasts `spindle_args_t` and preload messages down the tree, and waits for `LDCS_MSG_EXIT_READY` from servers on shutdown. |
| `msocket_fe_comm.c` | Disabled alternative that uses point-to-point sockets instead of a tree. |

The public interface (`fe_comm.h`) is:

```c
// Open FE server (tree root) — called once at startup
int ldcs_audit_server_fe_md_open(spindle_args_t *params);

// Broadcast a message to all server daemons
int ldcs_audit_server_fe_broadcast(ldcs_message_t *msg, void *md_data);

// Block until all daemons report alive
int ldcs_audit_server_fe_md_waitfor_alive(int timeout_ms);
```

### cobo — Collective Operations

Implements the tree-based broadcast and gather used to distribute library data and bootstrap configuration. The FE is always tree rank 0 (the root). Server daemons are internal nodes; they receive from their parent and forward to their children.

The tree degree is determined by the number of nodes. Each node-to-node connection is established via a handshake protocol over TCP.

### openmpi_intercept — OpenMPI ORTE Integration

Spindle cannot use SLURM-style daemon pre-launch with OpenMPI, because OpenMPI manages process placement itself. Instead, two components work together:

**`parse_openmpi.cc`** modifies the mpiexec command line to inject:
- `-x LD_PRELOAD=<libompiintercept.so>` — loads the intercept library into mpiexec itself.
- `-x SPINDLE_OMPI_INTERCEPT="<file> <breakpoint_addr> <proctable_addr> <size_addr>"` — tells the intercept library where to find OpenMPI's internal process table.

**`ompi_intercept.c`** is a shared library preloaded into `mpiexec`. Its constructor:
1. Parses `SPINDLE_OMPI_INTERCEPT` to read the addresses of OpenMPI's `MPIR_PROCDESC` proctable and `MPIR_Breakpoint` function.
2. Spawns a monitor thread that polls the proctable until all process entries are valid.
3. Calls `MPIR_Breakpoint()` to signal Spindle that the job has started.

This gives Spindle FE a reliable notification that all MPI processes are live, at which point it broadcasts settings and begins serving libraries.

### launchmon — LaunchMON Integration (Legacy)

`spindle_fe_lmon.cc` integrates with the LaunchMON debugger middleware, which manages its own MRNET communication tree. Spindle broadcasts `spindle_args_t` via LaunchMON's tree rather than COBO. This path is used in environments where LaunchMON is already deployed (e.g., older BlueGene systems).

### flux — Flux Integration

`FluxLauncher` connects to the Flux broker via `flux_open()`, retrieves the full hostlist, stores daemon startup arguments in the Flux KVS via `setup_args_on_fe()`, and launches `spindle_be` on every node with `flux exec --rank=all`. The Flux path enables session mode (`OPT_SESSION`, `OPT_PERSIST`) for multi-job workflows.

### hostbin — Direct Node Launch

`HostbinLauncher` spawns daemons directly on a configured list of nodes without going through a resource manager. Intended for manual testing or cluster configurations without a scheduler.

### logging — FE Logging

The FE logging subsystem decouples debug output from the process's stdout/stderr:

- **`spindlef_logd`** — a separate process that collects log messages and writes them to a file.
- **`libspindleflogc.la`** — linked into the FE executable; provides `LOGGING_INIT` / `LOGGING_FINI` macros and routes messages to the daemon via a socket.

## Startup Sequence

The following describes a typical SLURM run: `spindle srun -n 64 ./app`

```
1. Parse args → detect "srun", create SlurmLauncher
2. Read SLURM_NODELIST / SLURM_NNODES → build hostlist
3. setupDaemons():
     fork/exec spindle_be --spindle_slurm ... on each node
     (via srun or RSH depending on configuration)
4. spindleInitFE():
     cobo_server_open(unique_id, hostlist, ports) → bind tree root
     Pack spindle_args_t (port, number, opts, unique_id, ...)
     ldcs_audit_server_fe_broadcast(LDCS_MSG_SETTINGS)
     ldcs_audit_server_fe_md_waitfor_alive(60000)    ← blocks
          BE daemons start, join COBO tree, send alive ACK
5. setupJob():
     Inject LD_AUDIT=<libspindle_audit_*.so> into srun env
     Set LDCS_LOCATION, LDCS_CONNECTION, LDCS_OPTIONS, ...
6. spawnJob():
     exec srun -n 64 ./app (with modified environment)
7. Applications start:
     Audit library loaded → client_init() → connects to local BE
     dlopen() calls intercepted → served from local cache
8. Applications exit → jobdone task → collect exit codes
9. spindleCloseFE():
     Broadcast shutdown message down COBO tree
     Close tree, clean up credentials
```

## Build Outputs

| Artifact | Location | Description |
|----------|----------|-------------|
| `spindle` | `$(bindir)` | Main user-facing executable |
| `libspindlefe.so` | `$(libdir)` | Shared library for programmatic FE use |
| `libspindlefe_static.a` | `$(libdir)` | Static version |
| `libspindleompiintercept.so` | `$(pkglibdir)` | Preloaded into mpiexec for OpenMPI |
| `spindlef_logd` | `$(pkglibexecdir)` | FE logging daemon |

## Environment Variables Set by the Front-End

These are injected into the application environment at job launch and are read by the client and server daemons:

| Variable | Description |
|----------|-------------|
| `LD_AUDIT` | Path to the appropriate `libspindle_audit_*.so` |
| `LDCS_LOCATION` | Local server cache directory (e.g., `/tmp/spindle_<rank>`) |
| `LDCS_ORIG_LOCATION` | Canonical/symbolic cache path |
| `LDCS_CONNECTION` | Connection string to the local server daemon |
| `LDCS_NUMBER` | Session number |
| `LDCS_RANKINFO` | Rank topology (`mylrank:mylsize:mdrank:mdsize`) |
| `LDCS_OPTIONS` | Bitmask of options (`OPT_FOLLOWFORK`, `OPT_SHMCACHE`, ...) |
| `LDCS_CACHESIZE` | Shared memory cache size in KB |
| `SPINDLE_SERIAL_PORT` | Port number (serial/single-node mode only) |
| `SPINDLE_OMPI_INTERCEPT` | OpenMPI proctable addresses (OpenMPI mode only) |
