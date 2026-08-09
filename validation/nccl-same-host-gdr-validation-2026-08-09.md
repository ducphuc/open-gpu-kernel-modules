# Same-host DMA-BUF GDRDMA container validation — 2026-08-09

## Scope and result

This record adds containerized NCCL evidence for the same-host DMA-BUF
GPUDirect RDMA path using SR-IOV VFs moved into isolated container network
namespaces. The source under test was the GPUDirect-enabled fork represented by
this repository, together with matching 610.57.04 kernel modules, matching
userspace, and a manually applied host `libcuda` replacement that exposed the
required DMA-BUF capability to CUDA userspace.

The validated positive path is:

- two RTX 5060 Ti GPUs on the same host;
- two ConnectX-6 Lx functions exported as VFs and rebound with non-zero GUIDs;
- each VF moved into a separate container netns;
- RoCE v2 addressing on the VF interfaces;
- NCCL `NET/IB/.../GDRDMA` over the two VF-backed HCAs;
- successful two-rank `all_reduce_perf_mpi` completion.

Result summary:

- Non-coherent DMA-BUF GPUDirect RDMA path: PASS
- NCCL `NET/IB/.../GDRDMA` communication: PASS
- Same-host VF-netns container workflow: PASS
- Feature-gate safety: not re-executed in this container validation record
- Stress and physical-wire calibration: deferred

## Platform and topology

- Host: same x86-64 validation host used for the 610.57.04 release work.
- Kernel: `7.0.0-29-generic`.
- Driver/userspace family: NVIDIA 610.57.04, CUDA 13.3.
- GPUs: two NVIDIA GeForce RTX 5060 Ti at PCI `41:00.0` and `42:00.0`.
- NIC: Mellanox ConnectX-6 Lx dual-port 25GbE RoCE.
- PF netdevs: `enp98s0f0np0`, `enp98s0f1np1`.
- VF netdevs created for this validation: `enp98s0f0v0`, `enp98s0f1v0`.
- VF RDMA HCAs: `mlx5_2`, `mlx5_3`.
- VF RoCE IPs inside container namespaces: `10.201.0.1/24`, `10.201.0.2/24`.
- NCCL GID index: `3`.

The VFs initially appeared with zero node GUIDs and were therefore not accepted
as a valid RDMA validation path. The host-side preparation explicitly wrote
non-zero GUIDs for both VFs and rebound the corresponding PCI functions before
continuing.

## Host-side VF preparation

The validated path required one VF per RoCE port:

```text
/sys/class/net/enp98s0f0np0/device/sriov_numvfs = 1
/sys/class/net/enp98s0f1np1/device/sriov_numvfs = 1
```

The resulting VF netdev and verbs mapping was:

```text
enp98s0f0v0 -> PCI 0000:62:00.2 -> mlx5_2
enp98s0f1v0 -> PCI 0000:62:01.2 -> mlx5_3
```

The VFs were assigned non-zero node GUIDs and rebound. After rebind:

```text
mlx5_2 node_guid = 6e44:1d03:0007:c407
mlx5_3 node_guid = 5613:3b03:00b2:930a
```

Both VF ports reached `PORT_ACTIVE`.

## Container namespace workflow

The working workflow does not rely on Docker bridge networking for the RDMA
path. Instead:

1. start two containers with `network_mode: none`;
2. move `enp98s0f0v0` into the launcher namespace;
3. move `enp98s0f1v0` into the worker namespace;
4. assign RoCE IPs;
5. run MPI/NCCL over those VF interfaces.

Validated container identities:

```text
nccl-vf-launcher: GPU 0, HCA mlx5_2, VF IP 10.201.0.1
nccl-vf-worker:   GPU 1, HCA mlx5_3, VF IP 10.201.0.2
```

The container namespace injection and bring-up were validated by:

- bidirectional `ping` between `10.201.0.1` and `10.201.0.2`;
- plain non-CUDA `ib_write_bw` between the two containers;
- successful NCCL `all_reduce_perf_mpi` over `NET/IB`.

## Plain RDMA validation inside the container namespaces

Before using NCCL, the exact same VF-netns container topology successfully ran
plain RDMA without CUDA:

```text
ib_write_bw -d mlx5_2 -i 1 -x 3 -F --report_gbits 10.201.0.2
```

Observed container-to-container RDMA result:

```text
#bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]
65536      5000             23.59              23.59
```

The server-side log matched the same throughput. This demonstrates that the
containerized VF namespace model itself is RDMA-correct before adding CUDA or
NCCL.

## NCCL environment used for the positive GDRDMA run

The successful NCCL run used the following effective settings:

```text
NCCL_DEBUG=INFO
NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH,P2P,SHM
NCCL_NET=IB
NCCL_NET_PLUGIN=none
NCCL_COLLNET_ENABLE=0
NCCL_P2P_DISABLE=1
NCCL_SHM_DISABLE=1
NCCL_IB_DISABLE=0
NCCL_IB_HCA=mlx5_2,mlx5_3
NCCL_IB_GID_INDEX=3
NCCL_DMABUF_ENABLE=1
NCCL_NET_GDR_LEVEL=SYS
NCCL_CROSS_NIC=1
NCCL_SOCKET_IFNAME=^docker,lo
```

MPI bootstrap used:

```text
mpirun --allow-run-as-root --mca pml ob1 --mca btl self,tcp --mca coll ^ucc
```

The earlier `btl_tcp_if_include` pinning to a namespace-specific interface was
removed because it caused the worker rank to reject the launcher-only interface
name and abort the communicator.

## NCCL positive evidence

The successful run established a single two-rank communicator:

```text
Rank 0 ... device 0 [0000:41:00]
Rank 1 ... device 1 [0000:42:00]
```

The RDMA backend selected the VF HCAs and VF netdevs:

```text
NET/IB : Using [0]mlx5_2:1/RoCE [RO]; OOB enp98s0f0v0:10.201.0.1<0>
NET/IB : Using [0]mlx5_3:1/RoCE [RO]; OOB enp98s0f1v0:10.201.0.2<0>
```

The decisive GPUDirect RDMA evidence from the successful reruns was:

```text
DMA-BUF is available on GPU device 0
GPU Direct RDMA Enabled for HCA 0 'mlx5_2'
GPU Direct RDMA Enabled for HCA 0 'mlx5_3'
Channel ... via NET/IB/0/GDRDMA
Connected all rings, use ring PXN 0 GDR 1
Collective test concluded: all_reduce_perf_mpi
```

This is sufficient to classify the transport as actual NCCL `NET/IB/.../GDRDMA`
rather than plain host-staged IB.

## NCCL throughput observed

The successful GDRDMA reruns converged to a large-message plateau around
`2.82-2.87 GB/s` on the 25GbE RoCE path.

Representative completed run summary:

```text
1073741824 bytes -> 2.84-2.87 GB/s busbw
Avg bus bandwidth -> 1.43879 GB/s
```

The average bus bandwidth is pulled down by the small-message part of the
sweep and should not be interpreted as the sustained large-message line rate.
The large-message plateau is the more relevant throughput indicator for this
validation.

## GPUDirect status before and after the final environment change

An earlier VF-netns NCCL run with a different environment completed over
RoCE but reported:

```text
GPU Direct RDMA Disabled for HCA 0 'mlx5_2'
GPU Direct RDMA Disabled for HCA 0 'mlx5_3'
Connected all rings, use ring PXN 0 GDR 0
```

After enabling the intended environment:

```text
NCCL_P2P_DISABLE=1
NCCL_SHM_DISABLE=1
NCCL_DMABUF_ENABLE=1
NCCL_NET_GDR_LEVEL=SYS
```

the same VF-netns workflow switched to:

```text
GPU Direct RDMA Enabled ...
... via NET/IB/0/GDRDMA
Connected all rings, use ring PXN 0 GDR 1
```

This differential result is strong evidence that the final run exercised the
same-host DMA-BUF GDRDMA path rather than a fallback path.

## RDMA activity and MAC counter evidence

The same-host NCCL GDRDMA run was bounded by before/after counter capture under
`validation/artifacts/nccl-same-host-gdr-2026-08-09/`.

### RDMA port counter deltas

Captured from `/sys/class/infiniband/<dev>/ports/1/counters/`:

```text
mlx5_2 port_rcv_data  25177513414
mlx5_2 port_xmit_data 25081211379
mlx5_3 port_rcv_data  25177511615
mlx5_3 port_xmit_data 25081213069
```

These large deltas are consistent with workload-scale RDMA activity during the
successful NCCL collective.

### IEEE 802.3 MAC counter deltas (`PPCNT grp=0`)

Captured through `mlxreg` on the two physical NIC functions:

```text
port0:
  a_frames_received_ok    +96301591
  a_frames_transmitted_ok +96301392
  a_octets_received_ok    +100710215876
  a_octets_transmitted_ok +100710186222

port1:
  a_frames_received_ok    +96301392
  a_frames_transmitted_ok +96301591
  a_octets_received_ok    +100710186222
  a_octets_transmitted_ok +100710215876
```

This shows that the same-host GDRDMA NCCL run produced not only large RDMA
counter movement but also large Ethernet MAC counter movement on both ports.

### Interpretation

For this host and this VF-netns workflow, the successful same-host NCCL GDRDMA
path does not exhibit negligible MAC deltas. Instead, the bounded run produced
substantial RDMA and substantial MAC counter increases together.

Therefore, this record supports the following narrower conclusion:

- same-host NCCL `NET/IB/.../GDRDMA` succeeded;
- workload-scale RDMA activity was observed; and
- workload-scale Ethernet MAC counter activity was also observed on both ports.

This record does **not** use the current counter set to claim adapter-internal
forwarding. The forwarding inference remains dependent on the separate physical-
wire calibration and the full differential analysis described in
`nccl-same-host-gdr-reproduction-plan.md`.

## Harness added for reproducibility

Host-specific harness files were added to support the container validation:

```text
scripts/setup-vf-netns.sh
scripts/run-nccl-vf-netns.sh
```

The original container workflow assets were created under
`$HOME/qwen-vllm-bench`. Reproducible copies are now maintained in this
repository at:

```text
validation/harness/nccl-vf-netns/docker-compose.nccl.vf-netns.yml
validation/harness/nccl-vf-netns/vf-netns-image/Dockerfile
```

The harness implements:

- container build/start;
- VF netdev injection into namespaces;
- IP assignment;
- plain RDMA validation;
- NCCL `all_reduce_perf_mpi` execution;
- teardown.

The committed harness is the canonical copy for reproducing this validation.

## Result matrix

| Validation | Evidence | Result |
|---|---|---|
| Correct candidate module loaded | local 610.57.04 validation basis | PASS |
| Experimental feature enabled | prior GPUDirect validation basis + successful GDRDMA run | PASS |
| BAR1/topology prerequisites | prior validated host basis | PASS |
| Patched/private `libcuda` userspace capability path present during positive validation | local operator-applied host setup | PASS for local validation workflow |
| CUDA DMA-BUF capability available | `DMA-BUF is available on GPU device 0` | PASS |
| DMA-BUF verbs registration path | successful GDRDMA connector lines | PASS |
| No legacy MR fallback for positive NCCL path | `NET/IB/.../GDRDMA` connectors | PASS |
| RDMA transfer | `ib_write_bw` in VF-netns containers | PASS |
| MPI two-rank communicator | rank 0 + rank 1 present | PASS |
| Separate GPU assignment | rank 0 -> GPU 0, rank 1 -> GPU 1 | PASS |
| NCCL IB transport | `Using network IB` | PASS |
| NCCL GDRDMA connector | `via NET/IB/0/GDRDMA` | PASS |
| Workload-scale RDMA activity | successful `ib_write_bw` + NCCL GDRDMA transfer | PASS |
| Negligible external MAC traffic | contradicted by the bounded same-host run; large `PPCNT grp=0` deltas were observed | FAIL FOR THIS RUN |
| Physical MAC-counter calibration | not archived in this record | DEFERRED |
| Internal forwarding inference | not supported by the current bounded counter evidence alone | DEFERRED |
| Feature disabled safely | not re-executed in this record | DEFERRED |
| Concurrent registration stress | not re-executed in this record | DEFERRED |
| Clean teardown | repeated container teardown and VF recovery used successfully | PASS |
| Kernel health | no validation-time crash path observed during successful run | PASS for bounded container run |
| Post-test CUDA health | NCCL repeated successfully | PASS |

## Conclusion

This local validation record demonstrates that the same-host DMA-BUF GPUDirect
RDMA path can be exercised successfully through NCCL `NET/IB/.../GDRDMA` using
SR-IOV VFs moved into separate container namespaces on the validated RTX 5060 Ti
+ ConnectX-6 Lx host.

The decisive facts are:

- containerized VF-to-VF RDMA worked;
- NCCL established a single two-rank communicator;
- NCCL used `mlx5_2` and `mlx5_3` over RoCE;
- the successful runs explicitly reported `GPU Direct RDMA Enabled`;
- channel connectors used `NET/IB/0/GDRDMA`;
- the collective completed successfully with repeatable throughput.

This record therefore upgrades the same-host containerized NCCL result from
plain RoCE success to a confirmed same-host DMA-BUF GDRDMA success.
