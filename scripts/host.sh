#!/bin/sh
# Host a multiplayer game (Linux).  Usage: ./scripts/host.sh [port] [seed]
#   port  defaults to 9001
#   seed  optional 0x... or decimal world seed (clients adopt it automatically)
# Players join with:  ./scripts/join.sh <your-ip> [port]
PORT="${1:-9001}"
BIN="${VOXEL_BIN:-./build/voxel}"
[ -n "$2" ] && export VOXEL_SEED="$2"
export VOXEL_HOST=":$PORT"
echo "Hosting on TCP port $PORT. Share your address with players:"
echo "  - same LAN : your LAN IP            (run:  ip addr | grep 'inet ')"
echo "  - internet : your Tailscale/ZeroTier IP, or your public IP if you"
echo "               port-forwarded TCP $PORT to this machine"
exec "$BIN"
