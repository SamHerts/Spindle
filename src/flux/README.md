# Spindle Flux Integration

This directory implements two complementary Flux integration modes: a **job shell plugin** that activates Spindle per-job via jobspec options, and a **session mode** that runs persistent Spindle daemons across multiple jobs in an allocation.

## Overview

Flux integration has two entry points depending on the use case:

- **Job shell plugin** (`libspindleflux.so`) — loaded into every Flux job shell, activates Spindle for individual jobs when `--setopt=spindle=1` is present in the jobspec. The plugin starts the Spindle FE and BE daemons as part of the shell lifecycle.
- **Session mode** (`spindle_flux_session`) — starts long-lived Spindle daemons for the duration of a Flux allocation. Individual jobs detect the session via the Flux KVS and reuse the running daemons without starting new ones.

## Files

| File | Description |
|------|-------------|
| `flux-spindle.c` | Flux job shell plugin — implements `shell.init`, `task.init`, `shell.exit` callbacks |
| `fluxmgr.c` / `fluxmgr.h` | Flux connection and KVS management |
| `spindlefemgr.c` | FE session logic: encodes and publishes args to KVS, runs `spindleInitFE` |
| `spindlebemgr.c` | BE session logic: reads args from KVS, runs `spindleRunBE` |
| `sessionmgr.c` / `sessionmgr.h` | Session lifecycle: start/stop FE and BE services |
| `procmgr.c` / `procmgr.h` | Process management: fork services, pidfiles, ready-pipe synchronization |
| `main.cc` | Entry point for `spindle_flux_session` executable |
| `spindle_rc` | Template for `spindle.rc` — Flux plugin autoload configuration |

## Build Outputs

| Artifact | Location | Description |
|----------|----------|-------------|
| `libspindleflux.so` | `$(libdir)` | Job shell plugin (loaded by Flux) |
| `spindle_flux_session` | `$(pkglibexecdir)` | Session management executable |
| `spindle.rc` | `$(PKGSYSCONF_DIR)` | Flux RC file for automatic plugin loading |
| `libfluxsession.la` | (not installed) | Internal library shared by the above |

## Job Shell Plugin

### Activation

The plugin is loaded into all Flux job shells automatically via `spindle.rc`:

```
plugin.load { file="<libdir>/libspindleflux.so" }
```

It activates for a specific job only when the jobspec includes the `spindle` shell option:

```sh
flux run --setopt=spindle=1 ./app
```

Setting `SPINDLE=false` or `SPINDLE=0` in the job environment disables Spindle even when the option is present.

### Plugin Callbacks

**`shell.init` (`sp_init`):**

1. Checks that `spindle` is set in the job's shell options; returns immediately if not.
2. Propagates `SPINDLE_DEBUG`, `SPINDLE_TEST`, and `TMPDIR` from the job environment into the shell.
3. Creates a `spindle_ctx` with `unique_id` and `number` both derived from the Flux jobid (ensuring they are consistent across all shell ranks without coordination).
4. Calls `fillInSpindleArgsCmdlineFE()` to populate default `spindle_args_t` settings.
5. Reads per-job options from the `spindle` jobspec object (see Options below).
6. On **rank 0 only**: publishes `spindle_port` and `spindle_num_ports` into the `shell.init` exec eventlog event so all other ranks can learn the port without a separate broadcast.
7. All ranks watch the `guest.exec.eventlog` for the `shell.init` event. Once it fires with the port information, they fork a backend child running `spindleRunBE`, and rank 0 also calls `spindleInitFE`.

**`task.init` (`sp_task`):**

Before any task is executed, prepends the Spindle bootstrap argv to the task's command line. In session mode the bootstrap argv comes from the Flux KVS; in standalone mode it comes from `getApplicationArgsFE`.

**`shell.exit` (`sp_exit`):**

Rank 0 calls `spindleCloseFE` to tear down the FE and broadcast a shutdown to the server daemons.

### Per-Job Options

Options are passed as a JSON object in the jobspec:

```sh
flux run --setopt='spindle={"level":"high","location":"/scratch/spindle","follow-fork":"yes"}' ./app
```

| Option | Type | Description |
|--------|------|-------------|
| `level` | `"high"` \| `"medium"` \| `"low"` \| `"off"` | Preset option bundles (see below) |
| `reloc-aout` | `"yes"/"no"` | Relocate the main executable |
| `reloc-libs` | `"yes"/"no"` | Relocate shared libraries |
| `reloc-exec` | `"yes"/"no"` | Relocate exec'd child processes |
| `reloc-python` | `"yes"/"no"` | Relocate Python modules |
| `follow-fork` | `"yes"/"no"` | Propagate Spindle into forked children |
| `push` / `pull` | integer flag | Transfer mode (push from FE vs pull from client) |
| `noclean` | integer flag | Do not clean cache on exit |
| `nostrip` | integer flag | Do not strip debug symbols from cached libs |
| `location` | string | Override cache directory |
| `preload` | string | Path to preload file |
| `python-prefix` | string | Additional Python prefix paths |
| `numa` | integer flag | Enable NUMA-aware caching |
| `numa-files` | string | Explicit NUMA file list |

**Level presets:**

| Level | `reloc-aout` | `reloc-libs` | `reloc-exec` | `reloc-python` | `follow-fork` | `stop-reloc` | `off` |
|-------|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| `high` | ✓ | ✓ | ✓ | ✓ | ✓ | — | — |
| `medium` | — | ✓ | — | ✓ | ✓ | — | — |
| `low` | — | — | — | — | ✓ | ✓ | — |
| `off` | — | — | — | — | ✓ | — | ✓ |

## Session Mode

Session mode runs a single set of Spindle daemons that persist across multiple jobs. This is beneficial when many short jobs run sequentially — the daemons warm up once, and each subsequent job benefits from an already-populated cache.

### Starting and Stopping a Session

```sh
# Start a persistent Spindle session (run inside a flux allocation)
spindle_flux_session start [SPINDLE OPTIONS]

# Stop the session
spindle_flux_session stop
```

`start` forks two background services:

- **FE service** (head node only, i.e., Flux rank 0): runs `run_fe()`, which calls `spindleInitFE()` and then blocks in `spindleWaitForCloseFE()` until the allocation ends.
- **BE service** (every node): runs `run_be()`, which blocks waiting for `daemon_args` to appear in the Flux KVS, then calls `spindleRunBE()`.

After forking the BE, `start` waits up to 40 seconds for the BE to signal readiness via a pipe. If the BE does not become ready in time, it calls `stop` and returns an error.

`stop` sends `SIGTERM` to both the FE and BE services by looking up their pids from files in the session directory (`$SPINDLE_LOC/spindle_session/fe.pid` and `be.pid`).

### How Jobs Detect Session Mode

The job shell plugin's `task.init` callback calls `spindle_in_session_mode()`, which looks for `bootstrap_args` in the Flux KVS `spindle` namespace. If found, the job is in session mode and the bootstrap args from KVS are prepended to the task command instead of starting a new FE/BE pair.

## Flux KVS Coordination

All configuration is passed through the Flux KVS under the `spindle` namespace:

| Key | Content | Written by | Read by |
|-----|---------|-----------|---------|
| `daemon_args` | `"<port> <num_ports> <unique_id> <sec_type>"` | FE (`spindlefemgr`) | BE (`spindlebemgr`) |
| `bootstrap_args` | Space-separated argv to prepend to application processes | FE (`spindlefemgr`) | Shell plugin (`flux-spindle`) |
| `session` | Session key string | FE (if default session) | Shell plugin |

The BE waits up to 30 seconds for `daemon_args` to appear in the KVS (`FLUX_KVS_WAITCREATE`). The namespace is removed on session stop (`fluxmgr_rm_from_kvs`).

## Module Relationships

```
flux-spindle.c (shell plugin)
    │
    ├─ shell.init  ──► fillInSpindleArgsCmdlineFE()   [from libspindlefe]
    │                  spindleInitFE()                 [from libspindlefe, rank 0 only]
    │                  spindleRunBE()                  [from libspindlebe, forked child]
    │
    ├─ task.init   ──► fluxmgr_get_bootstrap()        [session mode: get argv from KVS]
    │                  getApplicationArgsFE()          [standalone: get argv from params]
    │
    └─ shell.exit  ──► spindleCloseFE()               [rank 0 only]

spindle_flux_session (session executable)
    │
    ├─ start ──► sessionmgr: spindle_session_start()
    │                │
    │                ├─ procmgr: start_service("fe") ──► spindlefemgr: run_fe()
    │                │                                       fluxmgr: get hostlist
    │                │                                       setup_args_on_fe() ──► KVS
    │                │                                       spindleInitFE()
    │                │                                       spindleWaitForCloseFE()
    │                │
    │                └─ procmgr: start_service("be") ──► spindlebemgr: run_be()
    │                                                        fluxmgr: wait for KVS
    │                                                        spindleRunBE()
    │
    └─ stop  ──► sessionmgr: spindle_session_stop()
                    procmgr: stop_service("fe") ──► SIGTERM
                    procmgr: stop_service("be") ──► SIGTERM
```
