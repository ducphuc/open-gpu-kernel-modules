#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
COMPOSE_FILE=${COMPOSE_FILE:-$ROOT_DIR/validation/harness/nccl-vf-netns/docker-compose.nccl.vf-netns.yml}
SETUP_SCRIPT="$ROOT_DIR/scripts/setup-vf-netns.sh"
LAUNCHER=${LAUNCHER:-nccl-vf-launcher}
WORKER=${WORKER:-nccl-vf-worker}
LAUNCHER_VF=${LAUNCHER_VF:-enp98s0f0v0}
WORKER_VF=${WORKER_VF:-enp98s0f1v0}
LAUNCHER_IP=${LAUNCHER_IP:-10.201.0.1/24}
WORKER_IP=${WORKER_IP:-10.201.0.2/24}
LAUNCHER_ADDR=${LAUNCHER_IP%/*}
WORKER_ADDR=${WORKER_IP%/*}
LAUNCHER_HCA=${LAUNCHER_HCA:-mlx5_2}
WORKER_HCA=${WORKER_HCA:-mlx5_3}
GID_INDEX=${GID_INDEX:-3}
SSH_PORT=${SSH_PORT:-2222}
READINESS_TIMEOUT=${READINESS_TIMEOUT:-60}

die() {
  echo "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    die "missing required command: $1"
  }
}

validate_config() {
  [[ -n "$LAUNCHER_ADDR" ]] || die "LAUNCHER_IP must include a non-empty address"
  [[ -n "$WORKER_ADDR" ]] || die "WORKER_IP must include a non-empty address"
  [[ "$READINESS_TIMEOUT" =~ ^[0-9]+$ ]] || die "READINESS_TIMEOUT must be a positive integer"
  [[ "$SSH_PORT" =~ ^[0-9]+$ ]] || die "SSH_PORT must be an integer between 1 and 65535"
  [[ "$GID_INDEX" =~ ^[0-9]+$ ]] || die "GID_INDEX must be a non-negative integer"

  READINESS_TIMEOUT=$((10#$READINESS_TIMEOUT))
  SSH_PORT=$((10#$SSH_PORT))
  (( READINESS_TIMEOUT > 0 )) || die "READINESS_TIMEOUT must be a positive integer"
  (( SSH_PORT >= 1 && SSH_PORT <= 65535 )) || die "SSH_PORT must be an integer between 1 and 65535"
}

wait_for_ready() {
  local stage=$1
  shift
  local deadline=$((SECONDS + READINESS_TIMEOUT))
  until "$@"; do
    if (( SECONDS >= deadline )); then
      echo "timeout waiting for $stage" >&2
      echo "worker=$WORKER addr=$WORKER_ADDR port=$SSH_PORT" >&2
      compose ps -a >&2 || true
      docker logs "$WORKER" 2>&1 | tail -n 80 >&2 || true
      exit 1
    fi
    sleep 1
  done
}

usage() {
  cat <<'EOF'
Usage:
  run-nccl-vf-netns.sh up        Build/start containers and inject VFs
  run-nccl-vf-netns.sh validate  Run ping + non-CUDA RDMA perftest
  run-nccl-vf-netns.sh nccl      Run NCCL all_reduce_perf_mpi
  run-nccl-vf-netns.sh down      Stop containers and remove compose resources
EOF
}

compose() {
  LAUNCHER="$LAUNCHER" \
  WORKER="$WORKER" \
  LAUNCHER_HCA="$LAUNCHER_HCA" \
  WORKER_HCA="$WORKER_HCA" \
  GID_INDEX="$GID_INDEX" \
  SSH_PORT="$SSH_PORT" \
    docker compose -f "$COMPOSE_FILE" "$@"
}

up() {
  validate_config
  [[ -f "$COMPOSE_FILE" ]] || { echo "missing compose file: $COMPOSE_FILE" >&2; exit 1; }
  [[ -x "$SETUP_SCRIPT" ]] || { echo "setup script is not executable: $SETUP_SCRIPT" >&2; exit 1; }
  need_cmd docker
  need_cmd sudo
  need_cmd ip
  compose build
  compose up -d
  docker inspect -f '{{.State.Running}}' "$LAUNCHER" | grep -qx true
  docker inspect -f '{{.State.Running}}' "$WORKER" | grep -qx true
  "$SETUP_SCRIPT" "$LAUNCHER" "$WORKER" "$LAUNCHER_VF" "$WORKER_VF" "$LAUNCHER_IP" "$WORKER_IP"
}

validate() {
  validate_config
  docker exec \
    --env WORKER_ADDR="$WORKER_ADDR" \
    "$LAUNCHER" bash -lc 'ping -c 2 "$WORKER_ADDR"'
  docker exec \
    --env WORKER_HCA="$WORKER_HCA" \
    --env GID_INDEX="$GID_INDEX" \
    "$WORKER" bash -lc '
      rm -f /tmp/ibw_server.log
      nohup ib_write_bw -d "$WORKER_HCA" -i 1 -x "$GID_INDEX" -F --report_gbits \
        > /tmp/ibw_server.log 2>&1 </dev/null &
    '
  sleep 2
  docker exec \
    --env LAUNCHER_HCA="$LAUNCHER_HCA" \
    --env GID_INDEX="$GID_INDEX" \
    --env WORKER_ADDR="$WORKER_ADDR" \
    "$LAUNCHER" bash -lc '
      ib_write_bw -d "$LAUNCHER_HCA" -i 1 -x "$GID_INDEX" -F --report_gbits "$WORKER_ADDR"
    '
  docker exec "$WORKER" bash -lc 'tail -n 120 /tmp/ibw_server.log'
}

run_nccl() {
  validate_config
  wait_for_ready "worker tcp port" \
    docker exec \
      --env WORKER_ADDR="$WORKER_ADDR" \
      --env SSH_PORT="$SSH_PORT" \
      "$LAUNCHER" bash -lc 'nc -z "$WORKER_ADDR" "$SSH_PORT" >/dev/null 2>&1'
  wait_for_ready "worker ssh" \
    docker exec \
      --env WORKER_ADDR="$WORKER_ADDR" \
      --env SSH_PORT="$SSH_PORT" \
      "$LAUNCHER" bash -lc '
        ssh -i /ssh-share/id_ed25519 -p "$SSH_PORT" \
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
          "root@$WORKER_ADDR" true >/dev/null 2>&1
      '
  docker exec \
    --env SSH_PORT="$SSH_PORT" \
    --env LAUNCHER_ADDR="$LAUNCHER_ADDR" \
    --env WORKER_ADDR="$WORKER_ADDR" \
    --env LAUNCHER_HCA="$LAUNCHER_HCA" \
    --env WORKER_HCA="$WORKER_HCA" \
    --env GID_INDEX="$GID_INDEX" \
    "$LAUNCHER" bash -lc '
    set -euo pipefail
    mpirun \
      --allow-run-as-root \
      --mca pml ob1 \
      --mca btl self,tcp \
      --mca coll ^ucc \
      -mca plm_rsh_args "-p $SSH_PORT -i /ssh-share/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
      -np 1 -H "$LAUNCHER_ADDR" \
        -x NCCL_DEBUG \
        -x NCCL_DEBUG_SUBSYS \
        -x NCCL_NET \
        -x NCCL_NET_PLUGIN \
        -x NCCL_COLLNET_ENABLE \
        -x NCCL_P2P_DISABLE \
        -x NCCL_SHM_DISABLE \
        -x NCCL_IB_DISABLE \
        -x NCCL_IB_HCA="$LAUNCHER_HCA" \
        -x NCCL_IB_GID_INDEX="$GID_INDEX" \
        -x NCCL_NET_GDR_LEVEL \
        -x NCCL_DMABUF_ENABLE \
        -x NCCL_CROSS_NIC \
        -x NCCL_SOCKET_IFNAME=^docker,lo \
        -x LD_LIBRARY_PATH \
        /usr/local/bin/all_reduce_perf_mpi -b 8 -e 1G -f 2 -g 1 -w 2 --iters 10 -c 10 \
      : -np 1 -H "$WORKER_ADDR" \
        -x NCCL_DEBUG \
        -x NCCL_DEBUG_SUBSYS \
        -x NCCL_NET \
        -x NCCL_NET_PLUGIN \
        -x NCCL_COLLNET_ENABLE \
        -x NCCL_P2P_DISABLE \
        -x NCCL_SHM_DISABLE \
        -x NCCL_IB_DISABLE \
        -x NCCL_IB_HCA="$WORKER_HCA" \
        -x NCCL_IB_GID_INDEX="$GID_INDEX" \
        -x NCCL_NET_GDR_LEVEL \
        -x NCCL_DMABUF_ENABLE \
        -x NCCL_CROSS_NIC \
        -x NCCL_SOCKET_IFNAME=^docker,lo \
        -x LD_LIBRARY_PATH \
        /usr/local/bin/all_reduce_perf_mpi -b 8 -e 1G -f 2 -g 1 -w 2 --iters 10 -c 10
  '
}

down() {
  [[ -f "$COMPOSE_FILE" ]] || { echo "missing compose file: $COMPOSE_FILE" >&2; exit 1; }
  compose down -v
}

cmd=${1:-}
case "$cmd" in
  up) up ;;
  validate) validate ;;
  nccl) run_nccl ;;
  down) down ;;
  *) usage; exit 1 ;;
esac
