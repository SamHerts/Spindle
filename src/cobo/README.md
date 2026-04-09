# COBO — Collective Bootstrap Operations

COBO is a self-contained library that bootstraps a set of distributed processes into a TCP-connected binomial tree and provides MPI-style collective operations over that tree. It is the backbone of Spindle's startup and library-distribution protocol.

## Overview

At large scale, having every compute node contact a single server would create an N-to-1 bottleneck. COBO avoids this by arranging all participating nodes into a **binomial tree**: each node holds a socket to its parent and sockets to each of its children. Broadcasts travel down the tree in O(log N) steps, and gathers travel up in the same time, regardless of the total number of nodes.

The library has two roles that share the same source:

- **Server role** — taken by the Spindle front-end (the login/launch node). The server is *outside* the tree; it connects to rank 0 and seeds the hostlist from which all nodes derive their rank assignments and tree topology.
- **Client role** — taken by each server daemon on a compute node. Clients form the tree itself and expose the collective operations.

## Files

| File | Purpose |
|------|---------|
| `cobo.c` | All tree logic: connection setup, topology computation, and collective operations |
| `cobo_comm.c` | Low-level reliable read/write wrappers (`ll_read`, `ll_write`, `write_msg`) used throughout Spindle's messaging layer |
| `cobo_handshake.c` | Wires the security handshake into the COBO connection layer; logs security errors to syslog |
| `handshake.c` | Authentication implementation — MUNGE, gcrypt key file, or null; called on every new TCP connection |
| `ldcs_cobo.h` | Public API header (namespaced to `ldcs_` via preprocessor macros) |
| `cobo_comm.h` | Header for `cobo_comm.c` |
| `handshake.h` | Header for the handshake protocol types and functions |

The source files are not built in place. Both `src/fe/cobo/Makefile.am` and `src/server/cobo/Makefile.am` compile them into a local `libldcs_cobo.la` that is linked into the front-end and server respectively.

## API

All public symbols are prefixed `ldcs_` at compile time via the `COBO_NAMESPACE` macro defined in `ldcs_cobo.h`. The logical names below omit the prefix for readability.

### Server interface (used by the front-end)

```c
// Seed the tree: connect to rank 0 and forward the hostlist.
// sessionid uniquely identifies this Spindle session.
// hostlist is the ordered list of compute node hostnames.
// portlist is the set of TCP ports nodes will attempt to bind.
int cobo_server_open(uint64_t sessionid,
                     char **hostlist, int num_hosts,
                     int  *portlist,  int num_ports);

// Tear down the server-side connection (leaves daemons running).
int cobo_server_close();

// Retrieve the fd to rank 0 for direct messaging after tree setup.
int cobo_server_get_root_socket(int *fd);
```

### Client interface (used by server daemons)

```c
// Join the tree. Blocks until topology is established and a barrier passes.
// Returns this node's rank and the total number of participants.
int cobo_open(uint64_t sessionid,
              int *portlist, int num_ports,
              int *rank, int *num_ranks);

// Shut down all tree connections and free state.
int cobo_close();

// Retrieve the fd to this node's parent (for direct messaging).
int cobo_get_parent_socket(int *fd);

// Retrieve the fd to child number `num`.
int cobo_get_child_socket(int num, int *fd);

// Number of direct children this node has in the tree.
int cobo_get_num_childs(int *num_childs);
```

### Collective operations (client only, all return `COBO_SUCCESS` or non-zero on error)

```c
int cobo_barrier();
int cobo_bcast    (void *buf, int count, int root);   // root must be 0
int cobo_bcast_down(void *buf, int count);            // send to children only, no receive
int cobo_gather   (void *sendbuf, int count, void *recvbuf, int root);
int cobo_scatter  (void *sendbuf, int count, void *recvbuf, int root);
int cobo_allgather(void *sendbuf, int count, void *recvbuf);
int cobo_allgather_str(char *sendstr, char ***recvstr, char **recvbuf);
```

All operations require root rank 0 (the front-end is not rank 0; rank 0 is the first compute node). `cobo_alltoall` is declared but not implemented.

## Tree Topology

COBO uses a **binomial tree** computed deterministically from each node's rank and the total number of participants (`cobo_compute_children` in `cobo.c`).

For N nodes, each node at rank `r` has children at positions determined by bisecting the rank range `[low, high]` recursively:

```
mid = (high - low) / 2 + (high - low) % 2 + low
```

This gives at most `⌈log₂ N⌉` children per node and a tree depth of `⌈log₂ N⌉`. At 1024 nodes the depth is 10, so a broadcast completes in 10 network round trips rather than 1024.

Each node stores:
- `cobo_parent` / `cobo_parent_fd` — rank and socket of parent
- `cobo_child[]` / `cobo_child_fd[]` — ranks and sockets of children
- `cobo_child_incl[]` — the subtree size rooted at each child (used by gather/scatter to size buffers correctly)

## Connection Setup Sequence

```
FE (server role)                      Rank 0 node            Rank k node
─────────────────                     ──────────────          ───────────
cobo_server_open(id, hosts, ports)
  │
  ├─ build hostlist data structure
  │
  ├─ connect to hosts[0]:port  ──────► cobo_open(id, ports)
  │                                      bind listening socket
  │                                      accept parent connection
  │                                      handshake + service/session ids
  │  send rank=0, nprocs, hostlist ──►   receive rank, nprocs, hostlist
  │                                      cobo_compute_children()
  │                                      connect to child (rank k) ────► accept
  │                                         handshake                     handshake
  │                                         send rank=k, hostlist ──────► receive rank, hostlist
  │                                                                        compute children ...
  │                                      cobo_barrier() ◄────────────────────── barrier
  └─ (tree is now live)
```

The server seeds only rank 0. Rank 0 then fans out to its children, and each child fans out to its own children, until every node has received its rank assignment and hostlist. After the tree is fully connected, a barrier confirms all nodes are up before `cobo_open` returns.

## Broadcast and Gather

**Broadcast** (`cobo_bcast_tree`):
1. Non-root nodes read `count` bytes from their parent socket.
2. All nodes write `count` bytes to each child socket.
3. The root writes first; data flows down the tree.

**Gather** (`cobo_gather_tree`):
1. Each node allocates a buffer of size `(subtree_size + 1) * sendcount`.
2. Copies its own data to offset 0.
3. Reads each child's contribution (child's entire subtree) into successive offsets.
4. Non-root nodes forward the aggregated buffer to their parent; rank 0 holds the result.

Scatter is the reverse of gather. `cobo_allgather` = gather to rank 0, then broadcast.

## Security — Handshake Protocol

Every TCP connection is authenticated before any data is exchanged. The security mechanism is selected at build time via configure:

| Mechanism | `handshake_security_t` | Description |
|-----------|----------------------|-------------|
| MUNGE | `hs_munge` | MUNGE credential exchange; tokens expire after `MUNGE_TTL_TIMEOUT_SEC` (default 30 s) |
| Key file | `hs_key_in_file` | AES/HMAC via gcrypt; key read from a file on a shared filesystem |
| Explicit key | `hs_explicit_key` | Same as key file but key passed directly in memory |
| None | `hs_none` | No authentication (insecure; for testing only) |

Handshake return codes:

| Code | Meaning |
|------|---------|
| `HSHAKE_SUCCESS` | Connection authenticated, proceed |
| `HSHAKE_DROP_CONNECTION` | Reject this connection (e.g., wrong session), retry |
| `HSHAKE_ABORT` | Security violation; log to syslog and abort |
| `HSHAKE_INTERNAL_ERROR` | Programming/system error |

The `cobo_handshake.c` file calls `cobo_set_handshake()` to register the chosen protocol with the tree layer, and calls `spindle_handshake_enable_read_timeout(10)` to set a 10-second timeout on handshake reads to prevent indefinite hangs.

## Connection Timeout and Retry

Nodes do not all start at exactly the same time. `cobo_connect_hostname` retries connections with configurable parameters (all tunable via environment variables):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `COBO_CONNECT_TIMEOUT` | 10 ms | Initial per-attempt `connect()` timeout |
| `COBO_CONNECT_BACKOFF` | 2 | Exponential backoff multiplier per retry |
| `COBO_CONNECT_SLEEP` | 10 ms | Sleep between port scan rounds |
| `COBO_CONNECT_TIMELIMIT` | 60 s | Total time before giving up |

If a connection attempt succeeds but the handshake or service/session ID check fails, the socket is closed and the scan continues, allowing COBO to tolerate stray connections from other processes that happen to bind the same port range.

## Namespace and Reuse

The `COBO_NAMESPACE` macro in `ldcs_cobo.h` prefixes every exported symbol with `ldcs_` (e.g., `cobo_open` becomes `ldcs_cobo_open`). This allows the same source to be compiled into multiple libraries within Spindle without symbol collisions between the FE and server builds.

## Low-Level I/O

`cobo_comm.c` provides the byte-level I/O primitives used by the rest of Spindle's messaging stack:

- `ldcs_cobo_read_fd` / `ldcs_cobo_write_fd` — poll-based read/write with automatic retry on `EINTR`/`EAGAIN`.
- `ll_read` / `ll_write` — similar wrappers used by the higher-level message layer.
- `write_msg(fd, ldcs_message_t *)` — writes a complete Spindle message (header + optional data payload) atomically.
