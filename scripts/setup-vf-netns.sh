#!/usr/bin/env bash
set -euo pipefail

LAUNCHER_CTN=${1:-nccl-vf-launcher}
WORKER_CTN=${2:-nccl-vf-worker}
LAUNCHER_VF=${3:-enp98s0f0v0}
WORKER_VF=${4:-enp98s0f1v0}
LAUNCHER_IP=${5:-10.201.0.1/24}
WORKER_IP=${6:-10.201.0.2/24}

launcher_pid=$(docker inspect -f '{{.State.Pid}}' "$LAUNCHER_CTN")
worker_pid=$(docker inspect -f '{{.State.Pid}}' "$WORKER_CTN")

if [[ "$launcher_pid" == "0" || "$worker_pid" == "0" ]]; then
  echo "containers must be running" >&2
  exit 1
fi

sudo mkdir -p /var/run/netns
sudo ln -sf "/proc/$launcher_pid/ns/net" "/var/run/netns/$LAUNCHER_CTN"
sudo ln -sf "/proc/$worker_pid/ns/net" "/var/run/netns/$WORKER_CTN"

for vf in "$LAUNCHER_VF" "$WORKER_VF"; do
  if ! ip link show "$vf" >/dev/null 2>&1; then
    echo "host VF not found: $vf" >&2
    exit 1
  fi
done

sudo ip link set "$LAUNCHER_VF" netns "$LAUNCHER_CTN"
sudo ip link set "$WORKER_VF" netns "$WORKER_CTN"

sudo ip -n "$LAUNCHER_CTN" link set lo up
sudo ip -n "$WORKER_CTN" link set lo up
sudo ip -n "$LAUNCHER_CTN" link set "$LAUNCHER_VF" up
sudo ip -n "$WORKER_CTN" link set "$WORKER_VF" up
sudo ip -n "$LAUNCHER_CTN" addr add "$LAUNCHER_IP" dev "$LAUNCHER_VF"
sudo ip -n "$WORKER_CTN" addr add "$WORKER_IP" dev "$WORKER_VF"

echo "launcher netns:"
sudo ip -n "$LAUNCHER_CTN" addr show "$LAUNCHER_VF"
echo "worker netns:"
sudo ip -n "$WORKER_CTN" addr show "$WORKER_VF"
