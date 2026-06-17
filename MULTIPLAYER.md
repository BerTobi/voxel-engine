# Multiplayer (0.3)

Play the same voxel planet together — up to **4 players**, over a LAN or the
internet. One player **hosts** (and plays); the others **join**.

## How it works (why it's cheap)

The world is a deterministic function of its **seed**: the same seed produces a
byte-identical planet on every machine. So joining transfers **nothing but the
seed** — each player generates the planet locally, and only **player movement**
and **block edits** travel over the network. The host is authoritative: it owns
the canonical world (and the only save on disk) and relays everyone's edits.

Everyone must run the **same build** — the join handshake refuses a peer whose
game/generator version differs (you'll see a clear message and drop to
single-player). The seed is handled automatically: a client adopts the host's.

## Quick start

**Linux** (binary at `./build/voxel`):

```
# Host:
./scripts/host.sh                 # port 9001
./scripts/host.sh 9001 0xCAFE     # optional: explicit port + world seed

# Join (run on the other machine):
./scripts/join.sh <host-ip>       # port 9001
./scripts/join.sh 100.64.0.2 9001 # explicit port
```

**Windows** (`voxel.exe`; put the `.bat` files next to it):

```
host.bat              :: port 9001
join.bat              :: double-click, then type the host IP when prompted
join.bat 192.168.1.5  :: or pass it on the command line
```

Under the hood these just set two environment variables, which you can also set
by hand:

| Variable        | Meaning                                             |
|-----------------|-----------------------------------------------------|
| `VOXEL_HOST=:9001`            | Host on TCP port 9001                 |
| `VOXEL_CONNECT=1.2.3.4:9001`  | Join the host at that IP:port         |
| `VOXEL_SEED=0x...`            | (Host only) choose the world; clients inherit it |

Neither host nor connect set → ordinary single-player (unchanged).

## Reaching each other

The game just opens a TCP connection to an `IP:port`. Getting that connection to
*reach the host* depends on where the players are:

1. **Same house / LAN.** Easiest. The host finds their local IPv4
   (`ip addr` on Linux, `ipconfig` on Windows — e.g. `192.168.1.5`) and shares
   it; players `join` to it. Near-zero latency.

2. **Different houses — a mesh VPN (recommended).** Install
   [Tailscale](https://tailscale.com) or [ZeroTier](https://zerotier.com) on
   both machines and join the same network; each device gets a stable private IP
   (e.g. `100.x.y.z`). `host` as usual, and players `join` to the host's tunnel
   IP. **No router configuration**, and it behaves like a LAN.

3. **Different houses — port forwarding.** On the host's router, forward one TCP
   port (default **9001**) to the host machine, and allow it through the host's
   firewall. Players `join` to the host's **public** IP. More fiddly and
   ISP-dependent, but no third-party software.

Over the internet there is some latency and jitter; remote players are rendered
with a short interpolation delay so their movement stays smooth, and block edits
are reliable (they apply as soon as they arrive).

## Notes & current limits

- **Up to 4** players (host + 3). A 4th join is refused while full.
- Block edits sync **live for chunks both players currently have loaded**. A
  later update adds host→client chunk sync so edits to areas a peer hasn't
  visited yet are reconciled when they get there.
- The fluid/heat simulation is not networked in 0.3.
- This is a trusted-friends feature: there is no anti-cheat (movement is
  client-side). Host only with people you trust, and prefer a tunnel or a single
  forwarded port over exposing your machine broadly.
