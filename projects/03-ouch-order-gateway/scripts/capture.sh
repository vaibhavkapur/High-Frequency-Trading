#!/usr/bin/env bash
# Captures the simulator's loopback traffic to a pcap for Wireshark.
# Needs root for tcpdump. Start `ouch_exchange_sim --port 9100` first, then run
# this, then run a client against the same port.
#
# Usage: sudo scripts/capture.sh [port] [output.pcap]
set -euo pipefail
PORT="${1:-9100}"
OUT="${2:-ouch_capture.pcap}"
IFACE=lo
[[ "$(uname -s)" == "Darwin" ]] && IFACE=lo0
echo "capturing tcp port $PORT on $IFACE to $OUT (Ctrl-C to stop)"
exec tcpdump -i "$IFACE" -w "$OUT" "tcp port $PORT"
