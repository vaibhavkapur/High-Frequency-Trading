#!/usr/bin/env bash
set -euo pipefail

MCAST_PORT="${1:-30001}"
TCP_PORT="${2:-30002}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  IFACE="lo0"
else
  IFACE="lo"
fi

echo "Capturing MDF1 multicast (UDP port $MCAST_PORT) and MDR1 recovery (TCP port $TCP_PORT) on $IFACE..."
echo "Press Ctrl-C to stop."
exec tcpdump -i "$IFACE" -nn -XX "udp port $MCAST_PORT or tcp port $TCP_PORT"
