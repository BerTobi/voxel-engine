/* net.c - Host-authoritative TCP multiplayer protocol (see net.h).
 *
 * Platform-agnostic: connection state + per-socket framing, the version-gated
 * handshake, the host relay + player/avatar table, and the inbound-edit queue.
 * Every socket syscall is a net_sys_* primitive (net_linux.c / net_win32.c).
 * Single-threaded; net_poll() pumps everything once per frame, never blocks.
 *
 * WIRE FORMAT (little-endian, explicitly packed - endian-independent on the wire,
 * with NET_MAGIC in the handshake to refuse a mismatched/garbled peer):
 *   frame  = u8 type | u16 len | payload[len]
 *   HELLO  = u32 magic | u16 proto | u32 game_ver | u32 gen_ver | u64 seed | u8 id  (23B)
 *   POSE   = u8 id | f32 pos[3] | f32 heading[3]                                    (25B)
 *   EDIT   = i32 wx | i32 wy | i32 wz | u32 voxel                                   (16B)
 *   LEAVE  = u8 id                                                                  (1B)
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- message types + payload sizes --------------------------------------- */
#define MSG_HELLO 1u
#define MSG_POSE  2u
#define MSG_EDIT  3u
#define MSG_LEAVE 4u

#define LEN_HELLO 23
#define LEN_POSE  25
#define LEN_EDIT  16
#define LEN_LEAVE 1

#define NET_RECV_CAP  8192
#define NET_SEND_CAP  16384
#define NET_EDIT_CAP  1024          /* inbound edits between drains (ring)         */
#define NET_HDR       3             /* u8 type + u16 len                           */
#define NET_HANDSHAKE_TIMEOUT_MS 6000

/* Pose interpolation (hide internet jitter): keep a short ring of timestamped
 * samples per remote player and render them slightly in the PAST so there is
 * always a pair to interpolate between. On a LAN the delay is imperceptible. */
#define NET_POSE_HIST            8      /* timestamped samples kept per player        */
#define NET_INTERP_DELAY_MS      100.0  /* render remote players this far in the past */
#define NET_POSE_MIN_INTERVAL_MS 30.0   /* outbound pose cap (~33 Hz)                  */

typedef struct { int wx, wy, wz; uint32_t voxel; } NetEdit;

typedef struct {
    int           used;             /* slot occupied                              */
    int           dead;             /* mark for reap at end of poll               */
    int           connecting;       /* client side: connect() still in progress   */
    int           live;             /* host: a valid POSE/EDIT has arrived         */
    double        accept_ms;        /* host: net_sys_now_ms() at accept (idle gate)*/
    net_sock      sock;
    int           id;               /* player id bound to this connection         */
    int           rlen, slen;
    unsigned char rbuf[NET_RECV_CAP];
    unsigned char sbuf[NET_SEND_CAP];
} Conn;

typedef struct { double t; float pos[3], hd[3]; } PoseSample;

typedef struct {
    int        active;
    int        id;
    float      color[3];
    PoseSample hist[NET_POSE_HIST];   /* hist[0] = oldest .. hist[hcount-1] = newest */
    int        hcount;
} Player;

struct NetState {
    NetMode  mode;
    uint32_t game_version, gen_version;
    uint64_t seed;
    int      local_id;
    int      ready;
    int      failed;

    net_sock listener;                       /* host only                          */
    Conn     conns[NET_MAX_PLAYERS];          /* host: client links; client: 1 link */
    int      id_used[NET_MAX_PLAYERS];        /* host: which player ids are taken    */

    Player   players[NET_MAX_PLAYERS];

    NetEdit  edits[NET_EDIT_CAP];
    int      ehead, ecount;
    double   last_pose_ms;                    /* outbound pose throttle */
};

/* Stable per-id avatar colour (id 0 = host). */
static const float PLAYER_COLORS[NET_MAX_PLAYERS][3] = {
    { 1.00f, 0.85f, 0.20f },   /* 0 host  - gold  */
    { 0.30f, 0.70f, 1.00f },   /* 1 client - blue  */
    { 0.40f, 1.00f, 0.45f },   /* 2 client - green */
    { 1.00f, 0.45f, 0.70f },   /* 3 client - pink  */
};

/* ======================================================================== *
 *  Little-endian pack / unpack (endian-independent on any host)             *
 * ======================================================================== */
static void put_u8 (unsigned char **p, uint8_t v)  { *(*p)++ = v; }
static void put_u16(unsigned char **p, uint16_t v) { (*p)[0]=(unsigned char)v; (*p)[1]=(unsigned char)(v>>8); *p+=2; }
static void put_u32(unsigned char **p, uint32_t v) { int i; for(i=0;i<4;i++)(*p)[i]=(unsigned char)(v>>(8*i)); *p+=4; }
static void put_u64(unsigned char **p, uint64_t v) { int i; for(i=0;i<8;i++)(*p)[i]=(unsigned char)(v>>(8*i)); *p+=8; }
static void put_i32(unsigned char **p, int32_t v)  { put_u32(p,(uint32_t)v); }
static void put_f32(unsigned char **p, float f)    { uint32_t u; memcpy(&u,&f,4); put_u32(p,u); }

static uint8_t  get_u8 (const unsigned char **p) { return *(*p)++; }
static uint16_t get_u16(const unsigned char **p) { uint16_t v=(uint16_t)((*p)[0]|((*p)[1]<<8)); *p+=2; return v; }
static uint32_t get_u32(const unsigned char **p) { uint32_t v=0; int i; for(i=0;i<4;i++) v|=(uint32_t)(*p)[i]<<(8*i); *p+=4; return v; }
static uint64_t get_u64(const unsigned char **p) { uint64_t v=0; int i; for(i=0;i<8;i++) v|=(uint64_t)(*p)[i]<<(8*i); *p+=8; return v; }
static int32_t  get_i32(const unsigned char **p) { return (int32_t)get_u32(p); }
static float    get_f32(const unsigned char **p) { uint32_t u=get_u32(p); float f; memcpy(&f,&u,4); return f; }

/* ======================================================================== *
 *  Connection helpers                                                       *
 * ======================================================================== */

/* Append a framed message to a connection's send queue; mark dead if it would
 * overflow (a peer that cannot drain in time is treated as gone). */
static void conn_queue(Conn *c, uint8_t type, const unsigned char *payload, int len)
{
    if (!c->used || c->dead) return;
    if (c->slen + NET_HDR + len > NET_SEND_CAP) { c->dead = 1; return; }
    c->sbuf[c->slen++] = type;
    c->sbuf[c->slen++] = (unsigned char)(len & 0xff);
    c->sbuf[c->slen++] = (unsigned char)((len >> 8) & 0xff);
    if (len > 0) { memcpy(c->sbuf + c->slen, payload, (size_t)len); c->slen += len; }
}

/* Queue to every live connection, optionally skipping `except`. */
static void broadcast(NetState *n, uint8_t type, const unsigned char *payload, int len, Conn *except)
{
    int i;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i) {
        Conn *c = &n->conns[i];
        if (c->used && !c->dead && !c->connecting && c != except)
            conn_queue(c, type, payload, len);
    }
}

static void player_update(NetState *n, int id, const float pos[3], const float hd[3])
{
    Player *pl;
    PoseSample *s;
    if (id < 0 || id >= (int)NET_MAX_PLAYERS) return;
    pl = &n->players[id];
    pl->active = 1;
    pl->id     = id;
    memcpy(pl->color, PLAYER_COLORS[id], sizeof pl->color);

    if (pl->hcount == NET_POSE_HIST) {                 /* ring full: drop oldest */
        memmove(&pl->hist[0], &pl->hist[1], sizeof(PoseSample) * (NET_POSE_HIST - 1));
        pl->hcount = NET_POSE_HIST - 1;
    }
    s = &pl->hist[pl->hcount++];
    s->t = net_sys_now_ms();
    memcpy(s->pos, pos, sizeof s->pos);
    memcpy(s->hd,  hd,  sizeof s->hd);
}

static void edit_push(NetState *n, int wx, int wy, int wz, uint32_t voxel)
{
    NetEdit *e;
    if (n->ecount >= NET_EDIT_CAP) return;   /* drained every frame; should never fill */
    e = &n->edits[(n->ehead + n->ecount) % NET_EDIT_CAP];
    e->wx = wx; e->wy = wy; e->wz = wz; e->voxel = voxel;
    n->ecount++;
}

/* ======================================================================== *
 *  Inbound message dispatch                                                 *
 * ======================================================================== */
static void on_message(NetState *n, Conn *c, uint8_t type, const unsigned char *pl, int len)
{
    switch (type) {
    case MSG_HELLO: {
        const unsigned char *p = pl;
        uint32_t magic, game, gen; uint16_t proto; uint64_t seed; uint8_t id;
        if (n->mode != NET_CLIENT || len != LEN_HELLO) { c->dead = 1; return; }
        magic = get_u32(&p); proto = get_u16(&p); game = get_u32(&p);
        gen   = get_u32(&p); seed  = get_u64(&p); id   = get_u8(&p);
        if (magic != NET_MAGIC || proto != NET_PROTOCOL_VERSION ||
            game != n->game_version || gen != n->gen_version) {
            fprintf(stderr, "net: handshake refused (incompatible host build: "
                            "magic/proto/game/gen mismatch)\n");
            n->failed = 1; c->dead = 1; return;
        }
        if (id < 1 || id >= (int)NET_MAX_PLAYERS) { n->failed = 1; c->dead = 1; return; }
        n->seed     = seed;
        n->local_id = id;
        n->ready    = 1;
        break;
    }
    case MSG_POSE: {
        const unsigned char *p = pl;
        int id; float pos[3], hd[3]; int k;
        if (len != LEN_POSE) { c->dead = 1; return; }
        id = get_u8(&p);
        for (k = 0; k < 3; ++k) pos[k] = get_f32(&p);
        for (k = 0; k < 3; ++k) hd[k]  = get_f32(&p);
        if (n->mode == NET_HOST) id = c->id;             /* trust the link, not the claim */
        if (id < 0 || id >= (int)NET_MAX_PLAYERS) { c->dead = 1; return; }
        player_update(n, id, pos, hd);
        c->live = 1;                                     /* real participant traffic (idle gate) */
        if (n->mode == NET_HOST) {                       /* relay so clients see each other */
            unsigned char buf[LEN_POSE], *q = buf;
            put_u8(&q, (uint8_t)id);
            for (k = 0; k < 3; ++k) put_f32(&q, pos[k]);
            for (k = 0; k < 3; ++k) put_f32(&q, hd[k]);
            broadcast(n, MSG_POSE, buf, LEN_POSE, c);
        }
        break;
    }
    case MSG_EDIT: {
        const unsigned char *p = pl;
        int wx, wy, wz; uint32_t voxel;
        if (len != LEN_EDIT) { c->dead = 1; return; }
        wx = get_i32(&p); wy = get_i32(&p); wz = get_i32(&p); voxel = get_u32(&p);
        edit_push(n, wx, wy, wz, voxel);
        c->live = 1;                                     /* real participant traffic (idle gate) */
        if (n->mode == NET_HOST)
            broadcast(n, MSG_EDIT, pl, LEN_EDIT, c);     /* relay verbatim, skip sender */
        break;
    }
    case MSG_LEAVE: {
        const unsigned char *p = pl;
        int id;
        if (len != LEN_LEAVE) { c->dead = 1; return; }
        id = get_u8(&p);
        if (id >= 0 && id < (int)NET_MAX_PLAYERS && id != n->local_id)
            memset(&n->players[id], 0, sizeof n->players[id]);  /* clear ring too (id reuse) */
        break;
    }
    default:
        c->dead = 1;                                     /* unknown type -> drop */
    }
}

/* Parse every complete frame currently in c->rbuf; shift the remainder down. */
static void parse_messages(NetState *n, Conn *c)
{
    int off = 0;
    while (c->rlen - off >= NET_HDR) {
        const unsigned char *h = c->rbuf + off;
        int len = h[1] | (h[2] << 8);
        if (len < 0 || NET_HDR + len > NET_RECV_CAP) { c->dead = 1; return; }  /* malformed */
        if (c->rlen - off < NET_HDR + len) break;        /* incomplete - wait for more */
        on_message(n, c, h[0], h + NET_HDR, len);
        if (c->dead) return;
        off += NET_HDR + len;
    }
    if (off > 0) {
        memmove(c->rbuf, c->rbuf + off, (size_t)(c->rlen - off));
        c->rlen -= off;
    }
}

static void conn_flush(Conn *c)
{
    while (c->slen > 0) {
        long n = net_sys_send(c->sock, c->sbuf, c->slen);
        if (n < 0)  { c->dead = 1; return; }
        if (n == 0) return;                              /* would-block: try next poll */
        if (n < c->slen) memmove(c->sbuf, c->sbuf + n, (size_t)(c->slen - n));
        c->slen -= (int)n;
    }
}

static void conn_recv(NetState *n, Conn *c)
{
    for (;;) {
        long got, room = (long)NET_RECV_CAP - c->rlen;
        /* A single frame larger than the whole buffer is impossible for a valid
         * message (max NET_HDR+LEN_POSE = 28 B) and parse_messages rejects an
         * oversized len before we could get here, so room==0 is a defensive
         * backstop, not a backpressure kill. */
        if (room <= 0) { c->dead = 1; return; }
        got = net_sys_recv(c->sock, c->rbuf + c->rlen, room);
        if (got < 0)  { c->dead = 1; return; }           /* closed / fatal */
        if (got == 0) break;                             /* would-block: done this poll */
        c->rlen += (int)got;
        /* DRAIN as we read: parse complete frames immediately so a burst of many
         * small valid frames never fills rbuf (the old code checked "full" before
         * ever parsing, so a ~8 KB backpressure burst of perfectly good poses
         * dropped the peer). After this, rbuf holds at most a partial trailing
         * frame, so the next iteration always has room. */
        parse_messages(n, c);
        if (c->dead) return;
    }
}

/* ======================================================================== *
 *  Lifecycle                                                                *
 * ======================================================================== */

static int alloc_id(NetState *n)        /* host: smallest free client id, or -1 */
{
    int i;
    for (i = 1; i < (int)NET_MAX_PLAYERS; ++i)
        if (!n->id_used[i]) { n->id_used[i] = 1; return i; }
    return -1;
}

static Conn *alloc_conn(NetState *n)
{
    int i;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i)
        if (!n->conns[i].used) { memset(&n->conns[i], 0, sizeof(Conn)); return &n->conns[i]; }
    return NULL;
}

static void host_accept(NetState *n)
{
    for (;;) {
        net_sock s = net_sys_accept(n->listener);
        Conn *c; int id;
        if (s == NET_SOCK_INVALID) break;                /* none pending */
        id = alloc_id(n);
        c  = id >= 0 ? alloc_conn(n) : NULL;
        if (c == NULL) { net_sys_close(s); if (id >= 0) n->id_used[id] = 0; continue; } /* full */
        c->used = 1; c->sock = s; c->id = id;
        c->accept_ms = net_sys_now_ms();   /* start the "must send a valid frame" clock */
        c->live = 0;
        {   /* queue HELLO immediately (flushed this same poll) */
            unsigned char buf[LEN_HELLO], *q = buf;
            put_u32(&q, NET_MAGIC); put_u16(&q, (uint16_t)NET_PROTOCOL_VERSION);
            put_u32(&q, n->game_version); put_u32(&q, n->gen_version);
            put_u64(&q, n->seed); put_u8(&q, (uint8_t)id);
            conn_queue(c, MSG_HELLO, buf, LEN_HELLO);
        }
    }
}

static void reap(NetState *n)
{
    int i;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i) {
        Conn *c = &n->conns[i];
        if (!c->used || !c->dead) continue;
        if (n->mode == NET_HOST) {
            unsigned char idb = (unsigned char)c->id;
            n->id_used[c->id] = 0;
            /* Clear the WHOLE Player (incl. the pose-sample ring), not just
             * active: ids are reused (alloc_id), and a leftover ring would make a
             * rejoining occupant interpolate in from the previous one's position. */
            memset(&n->players[c->id], 0, sizeof n->players[c->id]);
            net_sys_close(c->sock);
            c->used = 0;
            broadcast(n, MSG_LEAVE, &idb, LEN_LEAVE, NULL);  /* tell remaining clients */
        } else {
            net_sys_close(c->sock);
            c->used   = 0;
            n->failed = 1;                                   /* lost the host */
            memset(n->players, 0, sizeof n->players);        /* clear avatars */
        }
    }
}

void net_poll(NetState *n)
{
    int i;
    if (n == NULL) return;
    if (n->mode == NET_HOST) {
        double now = net_sys_now_ms();
        host_accept(n);
        /* Reap a connection that completed the TCP handshake but never sent a
         * valid POSE/EDIT within the window: a silent squatter would otherwise
         * hold a player id + slot forever (slot-exhaustion DoS from anyone who
         * can reach the port). A real client sends a pose within ~1 s of joining. */
        for (i = 0; i < (int)NET_MAX_PLAYERS; ++i) {
            Conn *c = &n->conns[i];
            if (c->used && !c->dead && !c->live &&
                (now - c->accept_ms) > (double)NET_HANDSHAKE_TIMEOUT_MS)
                c->dead = 1;
        }
    }

    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i) {
        Conn *c = &n->conns[i];
        if (!c->used || c->dead) continue;
        if (c->connecting) {                              /* client: finish connect() */
            int r = net_sys_connect_done(c->sock);
            if (r < 0) { c->dead = 1; continue; }
            if (r == 0) continue;                         /* still pending */
            c->connecting = 0;                            /* connected; await HELLO */
        }
        conn_flush(c);
        if (!c->dead) conn_recv(n, c);
    }
    reap(n);
}

/* ---- constructors -------------------------------------------------------- */

static NetState *net_alloc(NetMode mode, uint32_t game_version, uint32_t gen_version)
{
    NetState *n = (NetState *)calloc(1, sizeof(NetState));
    if (!n) return NULL;
    n->mode = mode;
    n->game_version = game_version;
    n->gen_version  = gen_version;
    n->listener = NET_SOCK_INVALID;
    return n;
}

NetState *net_host(unsigned short port, uint64_t seed,
                   uint32_t game_version, uint32_t gen_version)
{
    NetState *n;
    if (net_sys_init() != 0) return NULL;
    n = net_alloc(NET_HOST, game_version, gen_version);
    if (!n) { net_sys_cleanup(); return NULL; }
    n->seed     = seed;
    n->local_id = 0;
    n->ready    = 1;
    n->id_used[0] = 1;                                    /* the host is player 0 */
    n->listener = net_sys_listen(port);
    if (n->listener == NET_SOCK_INVALID) {
        fprintf(stderr, "net: failed to bind/listen on port %u\n", (unsigned)port);
        free(n); net_sys_cleanup(); return NULL;
    }
    return n;
}

NetState *net_client(const char *ip, unsigned short port,
                     uint32_t game_version, uint32_t gen_version)
{
    NetState *n; Conn *c; net_sock s;
    if (net_sys_init() != 0) return NULL;
    s = net_sys_connect(ip, port);
    if (s == NET_SOCK_INVALID) { net_sys_cleanup(); return NULL; }
    n = net_alloc(NET_CLIENT, game_version, gen_version);
    if (!n) { net_sys_close(s); net_sys_cleanup(); return NULL; }
    n->local_id = -1;                                    /* assigned by HELLO */
    c = &n->conns[0];
    c->used = 1; c->connecting = 1; c->sock = s; c->id = 0; /* link to host (id 0) */
    return n;
}

NetMode  net_mode(const NetState *n)     { return n ? n->mode : NET_OFF; }
int      net_ready(const NetState *n)    { return n ? n->ready : 0; }
int      net_failed(const NetState *n)   { return n ? n->failed : 0; }
uint64_t net_seed(const NetState *n)     { return n ? n->seed : 0; }
int      net_local_id(const NetState *n) { return n ? n->local_id : 0; }

/* ---- send helpers -------------------------------------------------------- */

void net_send_pose(NetState *n, const float pos[3], const float hd[3])
{
    unsigned char buf[LEN_POSE], *q = buf;
    int k;
    double now;
    if (n == NULL || n->mode == NET_OFF) return;
    if (n->local_id < 0) return;                         /* not handshaked yet */
    now = net_sys_now_ms();
    if (now - n->last_pose_ms < NET_POSE_MIN_INTERVAL_MS) return;   /* ~33 Hz cap */
    n->last_pose_ms = now;
    put_u8(&q, (uint8_t)n->local_id);
    for (k = 0; k < 3; ++k) put_f32(&q, pos[k]);
    for (k = 0; k < 3; ++k) put_f32(&q, hd[k]);
    /* The host's own entry (players[0]) is never rendered (avatar iteration skips
     * local_id), so there is no self-update here. */
    broadcast(n, MSG_POSE, buf, LEN_POSE, NULL);             /* host->clients, client->host */
}

void net_send_edit(NetState *n, int wx, int wy, int wz, uint32_t voxel)
{
    unsigned char buf[LEN_EDIT], *q = buf;
    if (n == NULL || n->mode == NET_OFF) return;
    put_i32(&q, wx); put_i32(&q, wy); put_i32(&q, wz); put_u32(&q, voxel);
    broadcast(n, MSG_EDIT, buf, LEN_EDIT, NULL);
}

void net_drain_edits(NetState *n,
                     void (*apply)(int, int, int, uint32_t, void *), void *user)
{
    if (n == NULL || apply == NULL) return;
    while (n->ecount > 0) {
        NetEdit e = n->edits[n->ehead];
        n->ehead = (n->ehead + 1) % NET_EDIT_CAP;
        n->ecount--;
        apply(e.wx, e.wy, e.wz, e.voxel, user);
    }
}

/* ---- avatars ------------------------------------------------------------- */

int net_avatar_count(const NetState *n)
{
    int i, count = 0;
    if (n == NULL) return 0;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i)
        if (n->players[i].active && i != n->local_id) count++;
    return count;
}

static void lerp3(float out[3], const float a[3], const float b[3], float t)
{
    int k;
    for (k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * t;
}

/* Fill pos/hd for a remote player by snapshot interpolation: sample the ring at
 * (now - NET_INTERP_DELAY_MS), lerping the two bracketing samples. Behind the
 * buffer -> oldest; ahead of it (peer went quiet) -> hold newest (no
 * extrapolation). One sample -> that sample. */
static void avatar_interp(const Player *pl, float pos[3], float hd[3])
{
    int last = pl->hcount - 1, i;
    double target;
    if (pl->hcount <= 0) {
        if (pos) { pos[0] = pos[1] = pos[2] = 0.0f; }
        if (hd)  { hd[0] = hd[1] = hd[2] = 0.0f; }
        return;
    }
    if (pl->hcount == 1) {
        if (pos) memcpy(pos, pl->hist[0].pos, sizeof pl->hist[0].pos);
        if (hd)  memcpy(hd,  pl->hist[0].hd,  sizeof pl->hist[0].hd);
        return;
    }
    target = net_sys_now_ms() - NET_INTERP_DELAY_MS;
    if (target <= pl->hist[0].t) {                 /* behind the buffer -> oldest */
        if (pos) memcpy(pos, pl->hist[0].pos, sizeof pl->hist[0].pos);
        if (hd)  memcpy(hd,  pl->hist[0].hd,  sizeof pl->hist[0].hd);
        return;
    }
    if (target >= pl->hist[last].t) {              /* ahead -> hold newest */
        if (pos) memcpy(pos, pl->hist[last].pos, sizeof pl->hist[last].pos);
        if (hd)  memcpy(hd,  pl->hist[last].hd,  sizeof pl->hist[last].hd);
        return;
    }
    for (i = 0; i < last; ++i) {
        if (target >= pl->hist[i].t && target < pl->hist[i + 1].t) {
            double span = pl->hist[i + 1].t - pl->hist[i].t;
            float  a    = span > 0.0 ? (float)((target - pl->hist[i].t) / span) : 0.0f;
            if (pos) lerp3(pos, pl->hist[i].pos, pl->hist[i + 1].pos, a);
            if (hd)  lerp3(hd,  pl->hist[i].hd,  pl->hist[i + 1].hd,  a);
            return;
        }
    }
    if (pos) memcpy(pos, pl->hist[last].pos, sizeof pl->hist[last].pos);  /* unreachable guard */
    if (hd)  memcpy(hd,  pl->hist[last].hd,  sizeof pl->hist[last].hd);
}

int net_get_avatar(const NetState *n, int idx,
                   float pos[3], float hd[3], int *id, float color[3])
{
    int i, seen = 0;
    if (n == NULL || idx < 0) return 0;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i) {
        const Player *pl = &n->players[i];
        if (!pl->active || i == n->local_id) continue;
        if (seen == idx) {
            if (id)    *id = pl->id;
            if (color) memcpy(color, pl->color, sizeof pl->color);
            avatar_interp(pl, pos, hd);
            return 1;
        }
        seen++;
    }
    return 0;
}

void net_shutdown(NetState *n)
{
    int i;
    if (n == NULL) return;
    for (i = 0; i < (int)NET_MAX_PLAYERS; ++i)
        if (n->conns[i].used) net_sys_close(n->conns[i].sock);
    if (n->listener != NET_SOCK_INVALID) net_sys_close(n->listener);
    net_sys_cleanup();
    free(n);
}

/* ======================================================================== *
 *  Env-driven entry (main.c)                                                *
 * ======================================================================== */

/* Parse "host:port" / ":port" / "port" / "" into ip (may be empty) + port. */
static void parse_hostport(const char *s, char *ip, size_t ipcap, unsigned short *port)
{
    const char *colon;
    ip[0] = '\0';
    *port = NET_DEFAULT_PORT;
    if (s == NULL || s[0] == '\0') return;
    colon = strrchr(s, ':');
    if (colon == NULL) {
        /* no colon: all-digits -> a bare port, else a bare host */
        const char *d; int alldig = 1;
        for (d = s; *d; ++d) if (*d < '0' || *d > '9') { alldig = 0; break; }
        if (alldig) { int p = atoi(s); if (p > 0 && p < 65536) *port = (unsigned short)p; }
        else        { size_t len = strlen(s); if (len < ipcap) memcpy(ip, s, len + 1); }
        return;
    }
    {
        size_t hlen = (size_t)(colon - s);
        if (hlen >= ipcap) hlen = ipcap - 1;
        memcpy(ip, s, hlen); ip[hlen] = '\0';
        if (colon[1] != '\0') { int p = atoi(colon + 1); if (p > 0 && p < 65536) *port = (unsigned short)p; }
    }
}

NetState *net_init_from_env(uint64_t local_seed,
                            uint32_t game_version, uint32_t gen_version)
{
    const char *host = getenv("VOXEL_HOST");
    const char *conn = getenv("VOXEL_CONNECT");
    char ip[64];
    unsigned short port;

    if (host && host[0]) {
        parse_hostport(host, ip, sizeof ip, &port);
        {
            NetState *n = net_host(port, local_seed, game_version, gen_version);
            if (n) fprintf(stderr, "net: hosting on port %u (seed %016llx), waiting for players\n",
                           (unsigned)port, (unsigned long long)local_seed);
            return n;
        }
    }
    if (conn && conn[0]) {
        int waited;
        NetState *n;
        parse_hostport(conn, ip, sizeof ip, &port);
        if (ip[0] == '\0') { fprintf(stderr, "net: VOXEL_CONNECT needs an IP (got '%s')\n", conn); return NULL; }
        n = net_client(ip, port, game_version, gen_version);
        if (!n) { fprintf(stderr, "net: connect to %s:%u failed\n", ip, (unsigned)port); return NULL; }
        fprintf(stderr, "net: connecting to %s:%u ...\n", ip, (unsigned)port);
        for (waited = 0; waited < NET_HANDSHAKE_TIMEOUT_MS; waited += 5) {
            net_poll(n);
            if (net_ready(n) || net_failed(n)) break;
            net_sys_sleep_ms(5);
        }
        if (net_failed(n) || !net_ready(n)) {
            fprintf(stderr, "net: handshake with %s:%u %s\n", ip, (unsigned)port,
                    net_failed(n) ? "refused/dropped" : "timed out");
            net_shutdown(n);
            return NULL;
        }
        fprintf(stderr, "net: joined %s:%u as player %d (seed %016llx)\n",
                ip, (unsigned)port, net_local_id(n), (unsigned long long)net_seed(n));
        return n;
    }
    (void)local_seed;
    return NULL;   /* NET_OFF: single-player */
}
