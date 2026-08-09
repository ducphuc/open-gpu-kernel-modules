# VF netns NCCL harness usage — 2026-08-09

This note describes how to use the host-specific VF netns harness added during the
same-host DMA-BUF GDRDMA validation.

## Scope

The harness is intended for the validated single-host configuration with:

- two RTX 5060 Ti GPUs;
- a dual-port ConnectX-6 Lx 25GbE RoCE NIC;
- one SR-IOV VF created on each physical NIC port;
- VFs moved into separate container namespaces.

It is not a generic deployment system. It is a reproducible local validation
workflow.

The harness assets are now vendored into this repository under a repo-local
path so the workflow no longer depends on unpublished files outside the tree.

## Files

- `scripts/setup-vf-netns.sh`
- `scripts/run-nccl-vf-netns.sh`

The compose and image files used by the harness are vendored in this repository:

- `validation/harness/nccl-vf-netns/docker-compose.nccl.vf-netns.yml`
- `validation/harness/nccl-vf-netns/vf-netns-image/Dockerfile`

The runner script uses that compose file through the `COMPOSE_FILE`
environment variable default embedded in the script.

Both scripts are intended to be executable directly from a clean checkout.

## Prerequisites

1. The intended 610.57.04 kernel modules and userspace must already be
   installed and loaded.
2. The host-side CUDA/userspace path required for DMA-BUF GPUDirect RDMA must
   already be in place.
3. SR-IOV must be enabled on both physical NIC ports.
4. One VF per port must exist.
5. The VF node GUIDs must be non-zero.
6. `mst` and `mlxreg` should be available when collecting hardware counters.
7. The operator must have working `sudo` for namespace and NIC operations.

## Expected validated VF mapping

The validated run used:

```text
enp98s0f0v0 -> mlx5_2 -> 10.201.0.1/24
enp98s0f1v0 -> mlx5_3 -> 10.201.0.2/24
```

These defaults are encoded into the scripts but can be overridden with
environment variables. Supported overrides include at least:

```text
COMPOSE_FILE
LAUNCHER
WORKER
LAUNCHER_VF
WORKER_VF
LAUNCHER_IP
WORKER_IP
LAUNCHER_HCA
WORKER_HCA
GID_INDEX
SSH_PORT
READINESS_TIMEOUT
```

## Basic commands

### Start containers and inject VFs

```bash
cd /path/to/open-gpu-kernel-modules
./scripts/run-nccl-vf-netns.sh up
```

This builds/starts the VF-netns containers and then calls
`setup-vf-netns.sh` to:

- find the container PIDs;
- create `/var/run/netns/*` links;
- move the host VFs into the container namespaces;
- bring the VF links up; and
- assign the validated RoCE IPs.

The runner now performs basic preflight checks before mutating the networking:

- compose file exists;
- setup script is executable;
- required host commands are present; and
- both containers are actually running after `compose up -d`.

### Validate plain RDMA between container namespaces

```bash
./scripts/run-nccl-vf-netns.sh validate
```

This performs:

- container-to-container `ping`;
- non-CUDA `ib_write_bw` between `mlx5_2` and `mlx5_3`.

### Run NCCL `all_reduce_perf_mpi`

```bash
env -u LD_LIBRARY_PATH ./scripts/run-nccl-vf-netns.sh nccl
```

The validated environment for the successful GDRDMA run includes:

```text
NCCL_DEBUG=INFO
NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH,P2P,SHM
NCCL_NET=IB
NCCL_NET_PLUGIN=none
NCCL_COLLNET_ENABLE=0
NCCL_P2P_DISABLE=1
NCCL_SHM_DISABLE=1
NCCL_IB_DISABLE=0
NCCL_IB_HCA=mlx5_2 / mlx5_3
NCCL_IB_GID_INDEX=3
NCCL_DMABUF_ENABLE=1
NCCL_NET_GDR_LEVEL=SYS
NCCL_CROSS_NIC=1
NCCL_SOCKET_IFNAME=^docker,lo
```

The runner launches `mpirun` manually from the launcher container and uses the
VF IPs as the two MPI host endpoints.

### Tear down the validation environment

```bash
./scripts/run-nccl-vf-netns.sh down
```

This removes the validation containers and their compose-managed volumes.

The teardown path now propagates real Docker/Compose failures instead of always
returning success.

## Recovering VFs back to the host namespace

If a validation run fails after VFs are moved into old container namespaces,
recover them before the next attempt. The recovery used during validation was:

```bash
for ns in vf-launcher vf-worker nccl-vf-launcher nccl-vf-worker; do
  if sudo ip netns list | grep -q "^${ns}\b"; then
    for dev in enp98s0f0v0 enp98s0f1v0; do
      if sudo ip -n "$ns" link show "$dev" >/dev/null 2>&1; then
        sudo ip -n "$ns" link set "$dev" netns 1 || true
      fi
    done
  fi
done
```

After recovery, verify the host sees both VF netdevs again with `ip link show`.

## Counter capture

For the same-host GDRDMA validation record, before/after counter capture was
performed around a successful NCCL run.

Artifacts were written under:

```text
validation/artifacts/nccl-same-host-gdr-2026-08-09/
```

The captured set includes:

- `counter-deltas.txt`
- `raw/rdma-counters-before.txt`
- `raw/rdma-counters-after.txt`
- `raw/ppcnt-port0-before.txt`
- `raw/ppcnt-port0-after.txt`
- `raw/ppcnt-port1-before.txt`
- `raw/ppcnt-port1-after.txt`
- `raw/nccl-mpi.log`

The raw evidence is retained because the validation plan requires the underlying
files, not just the derived summary.

## Readiness timeout behavior

The `nccl` command now uses a bounded readiness wait. If the worker TCP port or
SSH service does not become reachable before `READINESS_TIMEOUT`, the runner:

- prints the failed stage;
- prints the worker address and port;
- dumps `compose ps -a`; and
- prints a tail of the worker logs.

The command exits nonzero and does not silently continue into MPI bootstrap.

## Validation scope note

This harness and the associated validation record target the validated local
hardware configuration. Vendoring the assets makes that workflow reproducible;
it does not make the harness a generic deployment system.
