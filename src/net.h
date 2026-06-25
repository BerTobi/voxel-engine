/* net.h - Host-authoritative TCP multiplayer for the voxel engine (0.3).
 *
 * DESIGN (see the 0.3 plan): one HOST that also plays (player id 0) + up to
 * NET_MAX_PLAYERS-1 clients. The world is a deterministic function of the seed,
 * so NOTHING but the seed is transferred at join; thereafter only PLAYER POSES
 * and BLOCK EDITS cross the wire. Movement is client-authoritative (each instance
 * runs its own player_step; poses are relayed for avatars). Block edits are
 * host-RELAYED: the host forwards every edit to all other clients, and every
 * instance applies edits to its own deterministic world via world_edit_voxel.
 *
 * This module is WORLD-AGNOSTIC: it moves bytes and tracks a small player table;
 * it never includes world.h/voxel.h. main.c owns the world and calls back into
 * it (net_drain_edits) to apply received edits. The on-wire format is little-
 * endian with a magic sentinel, the SAME convention persist.c uses (both the
 * Linux dev host and the XP target are little-endian x86; a wrong-endian or
 * mismatched-build peer is REFUSED at handshake, never silently mis-read).
 *
 * SINGLE-THREADED, NON-BLOCKING: all sockets are non-blocking and pumped once
 * per frame by net_poll() from the main loop - no threads (matches the engine
 * and avoids XP threading). The platform socket syscalls live behind the
 * net_sys_* primitives (net_linux.c / net_win32.c); everything else is in net.c.
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>

/* ---- Tunables / wire constants ------------------------------------------- */
#define NET_MAX_PLAYERS      4u           /* host (id 0) + up to 3 clients      */
#define NET_DEFAULT_PORT     9001         /* used when host/connect omits :port */
#define NET_MAGIC            0x314E5856u   /* 'V''X''N''1' LE - handshake sentinel */
#define NET_PROTOCOL_VERSION 4u           /* bump on ANY wire-format change (4: 0.5 RLE chunk-delta) */

/* Max bytes of a serialized chunk delta (the MSG_CDATA payload main.c builds).
 * 0.5: the delta is RLE'd into runs of (u16 start, u16 len, u32 voxel) = 8 B each
 * (see chunksync.h). A fully-flooded/uniform chunk collapses to ONE run (22 B);
 * the WORST case is a chunk where no two index-consecutive voxels share a canon
 * word (e.g. a fine heat gradient) -> 4096 runs = 14 + 4096*8 = 32782 B. The
 * serializer never fragments, so the buffer MUST fit that worst case or it would
 * truncate (drop voxels -> client desync). net.c sizes its frame buffers to this. */
#define NET_CHUNK_MAX        36864u       /* >= 14 + 4096*8 (32782), with headroom */

/* ======================================================================== *
 *  PUBLIC API  (consumed by main.c and test_net.c)                          *
 * ======================================================================== */

typedef enum { NET_OFF = 0, NET_HOST, NET_CLIENT } NetMode;

/* Opaque; defined in net.c. */
typedef struct NetState NetState;

/* App entry: read VOXEL_HOST / VOXEL_CONNECT from the environment.
 *   VOXEL_HOST=":9001" or "0.0.0.0:9001"  -> HOST (binds, uses local_seed)
 *   VOXEL_CONNECT="1.2.3.4:9001"          -> CLIENT (connects, adopts host seed)
 *   neither set                           -> returns NULL (single-player; the
 *                                            caller keeps local_seed unchanged)
 * For a CLIENT this also drives the handshake to completion (bounded wait), so on
 * return net_seed() is the host's seed. game_version/gen_version are the caller's
 * compiled VOXEL_VERSION_PACKED / WG_GEN_VERSION - the handshake refuses a peer
 * whose values differ. Returns NULL on NET_OFF or any fatal/refused connection
 * (logged to stderr); the caller then runs single-player with local_seed. */
NetState *net_init_from_env(uint64_t local_seed,
                            uint32_t game_version, uint32_t gen_version);

/* Connect to a host + drive the handshake to completion (bounded wait), so on a
 * non-NULL return net_seed() is the host's seed. net_join_str accepts "ip" or
 * "ip:port" (default port). Used by the in-game connect screen. NULL (logged) on
 * connect/handshake failure -> caller falls back to single-player / the menu. */
NetState *net_join(const char *ip, unsigned short port,
                   uint32_t game_version, uint32_t gen_version);
NetState *net_join_str(const char *ipport, uint32_t game_version, uint32_t gen_version);

/* Explicit constructors (used by net_init_from_env and by tests). Both return
 * immediately with a NON-BLOCKING socket; a client's handshake completes later
 * during net_poll() (poll until net_ready()). NULL on socket-setup failure. */
NetState *net_host(unsigned short port, uint64_t seed,
                   uint32_t game_version, uint32_t gen_version);
NetState *net_client(const char *ip, unsigned short port,
                     uint32_t game_version, uint32_t gen_version);

NetMode  net_mode(const NetState *n);
int      net_ready(const NetState *n);   /* host: 1 always; client: 1 once HELLO ok */
int      net_failed(const NetState *n);  /* client: 1 if the connection died/refused */
uint64_t net_seed(const NetState *n);    /* agreed world seed (host's)               */
int      net_local_id(const NetState *n);/* this peer's player id (host = 0)          */

/* Pump the sockets ONCE per frame (after plat_poll): host accepts pending clients
 * and assigns ids; both sides flush queued sends and parse complete inbound
 * messages into the avatar table + the inbound-edit queue. Never blocks. */
void net_poll(NetState *n);

/* Queue this peer's pose for the others (host broadcasts; client sends to host,
 * which relays). pos = world XYZ, heading = unit tangent-forward (3 floats each).
 * Cheap to call every frame. */
void net_send_pose(NetState *n, const float pos[3], const float heading[3]);

/* Submit a LOCAL block edit. Client sends it to the host; host broadcasts to all
 * clients. (The caller also applies it locally itself - this only tells peers.) */
void net_send_edit(NetState *n, int wx, int wy, int wz, uint32_t voxel);

/* Apply every REMOTE edit received since the last call, in arrival order, via the
 * caller's callback (which calls world_edit_voxel/edit_and_notify), then clear the
 * queue. The callback must not re-enter net_*. */
void net_drain_edits(NetState *n,
                     void (*apply)(int wx, int wy, int wz, uint32_t voxel, void *user),
                     void *user);

/* ---- Chunk-delta sync (Milestone C) -------------------------------------- *
 * The base terrain is regenerated from the shared seed on each machine; only the
 * EDITS (deltas from the seed) need transferring. As a client streams a chunk in
 * it asks the host for that chunk's authoritative state; the host answers with
 * the voxels that differ from the seed (or nothing, the common case). net.c only
 * moves opaque bytes - main.c does the (de)serialization through these hooks. */

/* CLIENT: request chunk (cx,cy,cz) from the host. No-op unless NET_CLIENT. */
void net_request_chunk(NetState *n, int cx, int cy, int cz);

/* HOST (0.4 M5): PUSH a chunk delta to every client, UNSOLICITED - the
 * host-authoritative CA streaming channel. `payload`/`len` is a MSG_CDATA body
 * (built by chunksync_serve, exactly like a CREQ reply); clients apply it through
 * the same chunk_apply hook they use for requested deltas. BACKPRESSURE-SAFE: a
 * client whose send buffer lacks room is SKIPPED (not killed). RETURNS the number
 * of live clients that were skipped (0 == delivered to all): the caller should keep
 * the chunk's MODIFIED_BY_SIM flag set when >0 so a settled chunk (whose CA stops
 * re-flagging it) is still re-pushed until every client receives its final state.
 * No-op (returns 0) unless NET_HOST. */
int net_host_push_chunk(NetState *n, const unsigned char *payload, int len);

/* HOST: install the server. On a request net.c calls serve(cx,cy,cz,out,cap,user);
 * write the MSG_CDATA payload into out (cap == NET_CHUNK_MAX) and return its length
 * (0 = decline / nothing to send). Invoked inline during net_poll. */
void net_set_chunk_server(NetState *n,
    int (*serve)(int cx, int cy, int cz, unsigned char *out, int cap, void *user),
    void *user);

/* CLIENT: install the apply hook. net.c calls apply(data,len,user) inline during
 * net_poll when chunk data arrives; data is the host's MSG_CDATA payload. */
void net_set_chunk_apply(NetState *n,
    void (*apply)(const unsigned char *data, int len, void *user), void *user);

/* Remote players to draw as avatars (excludes this peer). Iterate i in
 * [0, net_avatar_count). net_get_avatar fills the latest known pose + a stable
 * per-id colour; returns 0 if i is out of range. */
int  net_avatar_count(const NetState *n);
int  net_get_avatar(const NetState *n, int i,
                    float pos[3], float heading[3], int *id, float color[3]);

void net_shutdown(NetState *n);

/* ======================================================================== *
 *  INTERNAL PLATFORM PRIMITIVES  (net_linux.c / net_win32.c)                *
 * ------------------------------------------------------------------------ *
 * Thin, non-blocking socket syscalls. net.c is the only caller. Return-code
 * convention is normalised across BSD sockets / Winsock so net.c carries no
 * #ifdefs:                                                                   *
 *   net_sys_recv : >0 = bytes read; 0 = would-block (no data now); -1 = peer  *
 *                  closed OR fatal error (net.c drops the connection).        *
 *   net_sys_send : >=0 = bytes accepted (may be < len, or 0 = would-block);   *
 *                  -1 = fatal/closed.                                         *
 * A net_sock holds a POSIX fd or a Winsock SOCKET; NET_SOCK_INVALID is the    *
 * empty/error value (-1, == INVALID_SOCKET cast on Win).                      *
 * ======================================================================== */

typedef intptr_t net_sock;
#define NET_SOCK_INVALID ((net_sock)-1)

int      net_sys_init(void);          /* WSAStartup on Win; 0 on success        */
void     net_sys_cleanup(void);

net_sock net_sys_listen(unsigned short port);          /* bind+listen, nonblock  */
net_sock net_sys_accept(net_sock listener);            /* nonblock; INVALID=none */
net_sock net_sys_connect(const char *ip, unsigned short port); /* nonblock start */
int      net_sys_connect_done(net_sock s);             /* 1 ok, 0 pending, -1 err */

long     net_sys_send(net_sock s, const void *buf, long len);
long     net_sys_recv(net_sock s, void *buf, long len);
void     net_sys_close(net_sock s);
void     net_sys_sleep_ms(int ms);
double   net_sys_now_ms(void);        /* monotonic ms (relative; for idle timeouts) */

#endif /* NET_H */
