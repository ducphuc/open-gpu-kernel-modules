# PCIe BAR1 and DMA-BUF P2P bounded validation — 2026-08-07

## Scope and provenance

- Branch: `fix/610.57.04-independent-bar1-p2p-caps`
- Driver/userspace: NVIDIA 610.57.04, CUDA 13.3
- Kernel: `7.0.0-29-generic`, x86-64
- GPUs: two NVIDIA GeForce RTX 5060 Ti (GB206), PCI `41:00.0` and `42:00.0`
- IOMMU: AMD IOMMU, pass-through mode
- IOVAS prerequisite: `83d6f3b8 RM: retain IOVA space until mappings are released`

This was an incremental, bounded validation of an authorized local driver
build. GPU commands and kernel-log reads ran through host namespaces. Every
candidate case used an exit trap and restored the exact official module set:

```text
nvidia         E3EB66762FE230BFDEFB25D
nvidia_uvm     625DCD62A2DB1AC8DCCA0FF
nvidia_modeset 5294D15459B74F85F379205
nvidia_drm     4E0736D3CFF454957152517
```

Both GPUs were queryable after every restoration. No pull request or remote
update was made.

## Static validation

- `make -C tests clean check`: PASS
- `make -j$(nproc) modules`: PASS
- `git diff --check`: PASS
- `modinfo -p kernel-open/nvidia.ko` exposes `NVreg_EnablePcieP2P` and
  `NVreg_EnableDmaBufP2P`; the old experimental name is absent.
- The full build emits the repository's existing objtool naked-return warnings
  but no compile, link, or modpost error.

## Default transport result

With both public switches at their default value of 1:

```text
GPU0 -> GPU1 read:    OK
GPU1 -> GPU0 read:    OK
GPU0 -> GPU1 write:   OK
GPU1 -> GPU0 write:   OK
GPU0 <-> GPU1 atomic: NS
```

The one-time policy record proves raw/effective separation:

```text
NVRM: PCIe P2P policy: EnablePcieP2P=1 EnableDmaBufP2P=1 rawMailboxWrite=2 rawMailboxRead=2 bar1Eligible=1 effectiveWrite=0 effectiveRead=0 transport=2
```

Here, raw status `2` is `GPU_NOT_SUPPORTED`, effective status `0` is `OK`, and
connectivity `2` is `P2P_CONNECTIVITY_PCIE_BAR1`. Atomics remain independently
unsupported.

An explicit `RMPcieP2PType=MAILBOX` query returned `GNS` with effective status
2 and no selected transport. An explicit `RMPcieP2PType=BAR1` query returned
`OK` with the same raw mailbox status and BAR1 connectivity. Default `AUTO`
matched BAR1 and did not fall back to mailbox.

## Public-switch matrix

| PCIe P2P | DMA-BUF P2P | CUDA/RM capability | 2 MiB FORCE_PCIE export | Result |
|---:|---:|---|---|---|
| 1 | 1 | read/write `OK` | exported `nv_dmabuf`; close/free lifetime case passed | PASS |
| 1 | 0 | read `OK` | CUDA 801, no FD created or leaked | PASS |
| 0 | 1 | read/write `DR` | exported `nv_dmabuf`; close/free lifetime case passed | PASS |
| 0 | 0 | read `DR` | CUDA 801, no FD created or leaked | PASS |

The DMA-BUF probe allocated 2 MiB, performed host-to-device and device-to-host
copy checks, requested the public `FORCE_PCIE` DMA-BUF handle, and verified FD
count restoration. In the positive case it verified the exporter name, size,
and close-on-exec flag, closed the DMA-BUF before freeing CUDA memory, repeated
the CUDA copy check, and returned to the original FD count. Both negative cases
failed at `cuMemGetHandleForAddressRange` with the expected
`CUDA_ERROR_NOT_SUPPORTED` result and still freed the CUDA allocation cleanly.

The positive and negative DMA-BUF calls used the existing private 610.57.04
capability-test library at `/home/ducphuc/libcuda-patched` (SHA-256
`646a198c255f33e5ddeae8ac9c13b255ae4932bfb3941cf817c7ba76a04f5d76`).
That userspace prerequisite is not part of this repository.

## Native CUDA peer copy

The bounded `p2pBandwidthLatencyTest` run reported mutual peer access and
completed successfully:

```text
Device=0 CAN Access Peer Device=1
Device=1 CAN Access Peer Device=0
P2P enabled unidirectional cross-GPU bandwidth: 14.09 GB/s
P2P enabled bidirectional cross-GPU bandwidth:   27.79-27.80 GB/s
P2P enabled GPU cross-GPU latency:               0.40 us
```

CUDA Samples are functional checks, not benchmark results; the figures only
distinguish the native peer path from the earlier fallback behavior.

## Kernel health

Each case was bounded by unique kernel markers. The isolated intervals were
searched for left-over IOVAS mappings, `pIOVAS != NULL`, Xids, RM assertions,
MMU/IOMMU faults, AER faults, kernel warnings/oopses, BUGs, panics, hung tasks,
and call traces. No such signature appeared in any completed capability,
peer-copy, DMA-BUF, or transport-selection case.

## Harness limitation

One preliminary attempt to load the locally built candidate DRM module failed
before any test because it referenced the unavailable
`drm_fbdev_ttm_driver_fbdev_probe` symbol. Direct `insmod` also omitted the
`video` dependency required by modeset. The official stack was restored and
verified immediately. All completed candidate tests were therefore headless
and loaded only `nvidia.ko` plus `nvidia-uvm.ko`; every case still ended by
restoring all four official modules with dependency-aware `modprobe`.

This is an environment/kernel-interface build limitation that remains to be
resolved before claiming candidate display-module load coverage. It occurred
before capability or mapping execution and does not change the P2P results.

## Deferred coverage

This bounded pass does not claim concurrent registration stress, repeated
abrupt termination, persistence-mode permutations, suspend/resume, hot unplug,
translated-IOMMU coverage, display-attached candidate-module coverage, or
additional GPU architectures. Those cases remain behind a separate review
gate. The repository issue template records the data needed to validate new
devices and topologies incrementally.
