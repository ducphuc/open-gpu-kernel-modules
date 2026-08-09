# GPUDirect support policy and validation

This document is the authoritative reference for the policy, controls,
runtime requirements, diagnostics, validation criteria, and reporting
requirements for the GPUDirect extensions in this fork.

The project provides two independent kernel-side feature families:

- **GPUDirect Peer-to-Peer (P2P) over PCIe:** the CUDA/RM GPU-to-GPU peer
  path, using BAR1 peer mappings. This is the capability commonly exercised
  through CUDA P2P, CUDA peer-to-peer, or CUDA peer-access APIs.
- **Non-coherent DMA-BUF P2P:** a generic DMA-BUF export/import mechanism that
  exposes qualifying GPU memory to eligible PCIe importers through static
  BAR1. GPUDirect RDMA is the validated use of this mechanism in this project.

`NVreg_EnableDmaBufP2P` controls the generic non-coherent DMA-BUF path; it is
not an RDMA-specific switch. CUDA/RM GPU-to-GPU GPUDirect P2P uses the
separate `NVreg_EnablePcieP2P` path. A GPU acting as a DMA-BUF importer through
some other software path is not prohibited by this policy, but such use is not
part of the current validation claims.

The project is focused on GeForce GPUs. The DMA-BUF implementation is
capability-based rather than restricted by GPU architecture, device ID, or
product class, so other non-coherent GPUs can use the same path when the
compiled NVIDIA HAL and runtime safety checks expose the required support.

The fork does not add device IDs, force generated properties, bypass topology
checks, or enable a mailbox transport that firmware reports as unsupported.

## Module parameters

The controls are independent and are applied at module load:

| `NVreg_EnablePcieP2P` | `NVreg_EnableDmaBufP2P` | Behavior |
|---:|---:|---|
| 1 | 1 | BAR1 CUDA/RM P2P and eligible non-coherent DMA-BUF P2P are enabled. This is the default. |
| 1 | 0 | BAR1 CUDA/RM P2P remains enabled; non-coherent DMA-BUF P2P rejects cleanly. |
| 0 | 1 | PCIe CUDA/RM P2P reports disabled; eligible non-coherent DMA-BUF P2P remains independently available. |
| 0 | 0 | Both feature families reject cleanly. |

`NVreg_EnablePcieP2P=0` produces
`NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY` for effective PCIe read and write
capability and prevents CUDA/RM PCIe peer-mapping construction. It does not
modify the raw mailbox capability stored for each GPU, and it does not disable
or remove the static BAR1 aperture used independently by the DMA-BUF
`FORCE_PCIE` path. Consequently, non-coherent DMA-BUF P2P can remain eligible
when `NVreg_EnablePcieP2P=0`, subject to its own runtime gates.

`NVreg_EnableDmaBufP2P=0` disables the capability-gated non-coherent
`FORCE_PCIE` DMA-BUF path. The existing coherent DMA-BUF path is unchanged.

`FORCE_PCIE` is the NVIDIA DMA-BUF export mapping type that requests the
PCIe/BAR1 physical representation rather than the default mapping.

The former `NVreg_ExperimentalDmaBufP2P` name is no longer a supported
parameter name and is not retained as an alias. Existing configurations should
use `NVreg_EnableDmaBufP2P`. This document does not claim a specific
unknown-parameter failure mode beyond the fact that the old name is no longer
recognized by this feature.

## GPUDirect P2P transport selection

The default `RMPcieP2PType=AUTO` policy means:

1. Discover transport-neutral GPU-pair and host PCIe topology.
2. Read and preserve the raw mailbox capability for diagnostics.
3. Evaluate BAR1 using the compiled HAL property and runtime safety checks.
4. Select `P2P_CONNECTIVITY_PCIE_BAR1` only when BAR1 passes.
5. Return the precise BAR1 failure when it does not pass; do not try mailbox.

`RMPcieP2PType=BAR1` follows the same BAR1 evaluation. An explicit
`RMPcieP2PType=MAILBOX` request returns the raw mailbox result and is intended
for diagnosis, not automatic fallback.

The raw and effective results can therefore legitimately differ. On the
validated RTX 5060 Ti (GB206) pair, firmware reports raw mailbox read/write as
`GPU_NOT_SUPPORTED`, while the independent effective BAR1 result is read/write
`OK`; PCIe atomics remain `NOT_SUPPORTED` until separately proven.

## Runtime gates

GPUDirect P2P over BAR1 requires compatible GPUs, a supported compiled HAL
path, acceptable host and IOH topology, a usable static BAR1 window, and no
active mailbox mapping conflict.

Non-coherent DMA-BUF P2P applies additional importer-specific requirements:

- the export must use `FORCE_PCIE`;
- the GPU must be non-coherent;
- MIG must be disabled;
- the exported range must be wholly contained in the usable static BAR1
  window;
- the importer DMA mask must cover the complete BAR mapping;
- the importer must use an identity IOMMU domain; and
- Linux PCI P2PDMA must report a non-negative peer distance.

Range arithmetic remains overflow-safe and rejects spanning or out-of-window
requests.

## Mapping lifetime and teardown

The IOVA-space lifetime fix in this branch is a prerequisite for clean UVM
teardown. DMA mappings retain their IOVA space until the last mapping is
released, including when UVM duplicate handles outlive GPU device teardown.

This is a lifetime-correctness requirement, not a runtime admission gate.

## CUDA userspace prerequisite for GPUDirect RDMA

The kernel-side non-coherent DMA-BUF support is necessary, but it is not
sufficient by itself on the validated GeForce configuration.

Stock CUDA userspace does not advertise the required DMA-BUF capability on
that configuration, so stock userspace does not exercise the non-coherent
DMA-BUF path enabled by this fork. Investigation or modification of CUDA
userspace is outside the scope of support.

Harry Chen (`Harry-Chen`) independently reported analogous userspace gating on
RTX 5090 while investigating GPUDirect RDMA support:

- https://github.com/aikitoria/open-gpu-kernel-modules/issues/20#issuecomment-4489627490
- https://harrychen.xyz/2026/05/20/enable-gpudirect-rdma-on-rtx-5090/

## Diagnostics

The first multi-GPU PCIe capability evaluation after module load emits one
kernel record beginning with `PCIe P2P policy:`. It includes both switch
values, raw mailbox read/write status, BAR1 eligibility, effective read/write
status, and the selected connectivity value.

Useful checks are:

    grep -E '^(EnablePcieP2P|EnableDmaBufP2P):' /proc/driver/nvidia/params
    nvidia-smi topo -m
    nvidia-smi topo -p2p r
    nvidia-smi topo -p2p w
    nvidia-smi topo -p2p a
    dmesg | grep 'PCIe P2P policy:'

After testing, inspect the same kernel-log interval for NVIDIA Xids, RM
assertions, MMU/IOMMU faults, PCIe AER faults, kernel warnings/oopses, hung
tasks, and IOVA-space teardown warnings.

## Validation requirements

A capability result alone is not sufficient validation. Validation should
exercise the corresponding data path and verify that the participating
devices remain usable afterward.

### GPUDirect P2P

Exercise CUDA peer-access APIs and peer-copy workloads, including tests such as
`simpleP2P` and `p2pBandwidthLatencyTest`. Record the tested allocation sizes,
directions, and workload range where applicable.

Validation should confirm:

- effective PCIe read/write capability;
- successful CUDA peer-access enablement;
- successful peer allocation and data transfer with verification;
- expected PCIe P2P bandwidth/latency behavior for the tested topology; and
- continued usability of both GPUs after the test.

The validated configurations in this project have exercised the GPUDirect P2P
path through CUDA peer-access APIs rather than relying only on capability
queries.

### GPUDirect RDMA

Validation of the non-coherent DMA-BUF path should include:

- successful GPU-memory DMA-BUF export;
- successful attachment or memory registration by the third-party importer;
- an actual RDMA workload using the registered GPU memory;
- evidence that the DMA-BUF path, rather than a legacy pointer-based fallback,
  was selected; and
- post-test inspection for Xids, RM assertions, MMU/IOMMU faults, PCIe AER
  faults, BAR1 failures, importer resets, kernel warnings/oopses, hung tasks,
  and IOVA-space teardown warnings.

The validated RTX 5060 Ti (GB206) GPUDirect RDMA configuration used mlx5 as
the DMA-BUF importer. Plain `ib_write_bw` established the VF RDMA path, and
NCCL GDRDMA exercised GPU memory through the DMA-BUF path.

#### Current 610.57.04 evidence

The current release-specific result used two RTX 5060 Ti GPUs, two
ConnectX-6 Lx SR-IOV VFs moved into separate container network namespaces,
RoCE v2, matching 610.57.04 kernel modules and userspace, and CUDA 13.3. NCCL
formed a two-rank communicator, reported DMA-BUF availability and GPUDirect
RDMA enablement for both mlx5 HCAs, used `NET/IB/0/GDRDMA` connectors, and
completed the collective successfully.

The positive result required a manually applied host `libcuda` replacement
that exposed the required DMA-BUF capability. It does not establish support
with stock CUDA userspace. The container run did not repeat feature-disabled
safety or concurrent registration stress, and physical-wire calibration
remains deferred. Because the bounded run showed workload-scale Ethernet MAC
counter increases, it is not evidence for adapter-internal forwarding.

The evidence, limitations, raw logs, and counter deltas are recorded in the
[same-host container validation](validation/nccl-same-host-gdr-validation-2026-08-09.md).
The reproducible workflow is described in the
[VF-netns harness usage guide](validation/nccl-vf-netns-harness-usage-2026-08-09.md).

Validation records that identify 610.43.03 provide historical hardware
evidence only. Results obtained with 610.43.03 must be repeated with matching
610.57.04 kernel modules, GSP firmware, and userspace components before the
corresponding capability and hardware configuration is considered validated
on 610.57.04.

## Issue reporting

Use the repository's GPUDirect issue template when reporting another device,
importer, or topology.

For all reports, include:

- the exact commit;
- NVIDIA driver and userspace versions;
- module `srcversion` values;
- GPU PCI IDs;
- IOMMU state;
- topology output;
- module parameters;
- the `PCIe P2P policy:` kernel record;
- the reproduction result; and
- the relevant kernel-log interval.

For non-coherent DMA-BUF or GPUDirect RDMA reports, also include:

- the importer device, PCI ID, and driver;
- the importer DMA mask when available;
- the importer IOMMU-domain mode;
- GPU-to-importer PCIe topology;
- the DMA-BUF or GPUDirect RDMA test command and relevant output; and
- evidence identifying the memory-registration path that was selected.

Remove serial numbers, hostnames, and unrelated private data before attaching
logs.
