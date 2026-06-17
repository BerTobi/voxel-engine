#!/bin/sh
# Join a multiplayer game (Linux).  Usage: ./scripts/join.sh <host-ip> [port]
#   host-ip : the host's LAN IP, tunnel IP (Tailscale/ZeroTier), or public IP
#   port    : defaults to 9001 (must match the host)
# You generate the SAME planet locally from the host's seed - nothing is downloaded.
if [ -z "$1" ]; then
    echo "usage: $0 <host-ip> [port]"
    exit 1
fi
PORT="${2:-9001}"
BIN="${VOXEL_BIN:-./build/voxel}"
export VOXEL_CONNECT="$1:$PORT"
echo "Connecting to $1:$PORT ..."
exec "$BIN"
