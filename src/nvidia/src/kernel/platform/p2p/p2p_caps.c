/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "core/system.h"
#include "gpu_mgr/gpu_mgr.h"
#include "kernel/gpu/mig_mgr/kernel_mig_manager.h"
#include "kernel/gpu/nvlink/kernel_nvlink.h"
#include "kernel/gpu/bif/kernel_bif.h"
#include "gpu/subdevice/subdevice.h"
#include "gpu/gpu.h"
#include "virtualization/hypervisor/hypervisor.h"
#include "vgpu/rpc.h"
#include "vgpu/vgpu_events.h"
#include "platform/chipset/chipset.h"
#include "platform/p2p/p2p_caps.h"
#include "pcie_p2p_policy.h"
#include "nvrm_registry.h"
#include "nvlimits.h"
#include "nvdevid.h"

ct_assert(NV2080_GET_P2P_CAPS_UUID_LEN == NV_GPU_UUID_LEN);
ct_assert(PCIE_P2P_POLICY_ENABLE_PCIE_DEFAULT == NV_REG_STR_ENABLE_PCIE_P2P_DEFAULT);
ct_assert(PCIE_P2P_POLICY_ENABLE_DMABUF_DEFAULT == NV_REG_STR_ENABLE_DMABUF_P2P_DEFAULT);
ct_assert(PCIE_P2P_POLICY_STATUS_OK == NV0000_P2P_CAPS_STATUS_OK);
ct_assert(PCIE_P2P_POLICY_STATUS_CHIPSET_NOT_SUPPORTED == NV0000_P2P_CAPS_STATUS_CHIPSET_NOT_SUPPORTED);
ct_assert(PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED == NV0000_P2P_CAPS_STATUS_GPU_NOT_SUPPORTED);
ct_assert(PCIE_P2P_POLICY_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED == NV0000_P2P_CAPS_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED);
ct_assert(PCIE_P2P_POLICY_STATUS_DISABLED == NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY);
ct_assert(PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED == NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED);

static NvBool
_kp2pCapsIsPcieP2PEnabled(OBJGPU *pGpu)
{
    NvU32 data = NV_REG_STR_ENABLE_PCIE_P2P_DEFAULT;

    (void)osReadRegistryDword(pGpu, NV_REG_STR_ENABLE_PCIE_P2P, &data);
    return data != 0;
}

static void
_kp2pCapsLogPolicyOnce
(
    NvU32            gpuMask,
    OBJGPU          *pGpu,
    NvBool           bPcieP2PEnabled,
    NvBool           bBar1Eligible,
    NvU8             rawMailboxWriteCapStatus,
    NvU8             rawMailboxReadCapStatus,
    NvU8             effectiveWriteCapStatus,
    NvU8             effectiveReadCapStatus,
    P2P_CONNECTIVITY connectivity
)
{
    static volatile NvU32 bLogged = 0;
    NvU32 dmabufP2PEnabled = NV_REG_STR_ENABLE_DMABUF_P2P_DEFAULT;

    if (gpumgrGetSubDeviceCount(gpuMask) < 2)
        return;

    if (!portAtomicCompareAndSwapU32(&bLogged, 1, 0))
        return;

    (void)osReadRegistryDword(
        pGpu, NV_REG_STR_ENABLE_DMABUF_P2P, &dmabufP2PEnabled);
    portDbgPrintf(
        "NVRM: PCIe P2P policy: EnablePcieP2P=%u EnableDmaBufP2P=%u "
        "rawMailboxWrite=%u rawMailboxRead=%u bar1Eligible=%u "
        "effectiveWrite=%u effectiveRead=%u transport=%u\n",
        bPcieP2PEnabled, dmabufP2PEnabled != 0,
        rawMailboxWriteCapStatus, rawMailboxReadCapStatus, bBar1Eligible,
        effectiveWriteCapStatus, effectiveReadCapStatus, connectivity);
}

/**
 * @brief Determines if the GPUs are P2P compatible
 *
 * @param[in] pGpu0
 * @param[in] pGpu1
 *
 * @return NV_TRUE if the GPUs are P2P compatible
 */
static NvBool
areGpusP2PCompatible(OBJGPU *pGpu0, OBJGPU *pGpu1)
{
    // Mark GPUs of different arch or impl incapable of P2P over pcie
    if ((gpuGetChipArch(pGpu0) != gpuGetChipArch(pGpu1)) ||
        (gpuGetChipImpl(pGpu0) != gpuGetChipImpl(pGpu1)))
    {
        return NV_FALSE;
    }

    // Mark GPUs of different notebook implementation incapable of P2P over pcie
    if (IsMobile(pGpu0) != IsMobile(pGpu1))
    {
        return NV_FALSE;
    }

    return NV_TRUE;
}

NV_STATUS
p2pGetCaps
(
    NvU32 gpuMask,
    NvBool *pP2PWriteCapable,
    NvBool *pP2PReadCapable,
    NvBool *pP2PAtomicsCapable,
    P2P_CONNECTIVITY *pConnectivity
)
{
    NvU8 p2PWriteCapsStatus;
    NvU8 p2PReadCapsStatus;
    NvU8 p2PAtomicsCapsStatus;
    NV_STATUS status;
    P2P_CONNECTIVITY connectivity;

    if ((pP2PWriteCapable == NULL) || (pP2PReadCapable == NULL) ||
        (pP2PAtomicsCapable == NULL))
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = p2pGetCapsStatus(gpuMask, &p2PWriteCapsStatus,
                              &p2PReadCapsStatus, &p2PAtomicsCapsStatus,
                              &connectivity
                              );
    if (status != NV_OK)
    {
        return status;
    }

    //
    // The classes like NV50_P2P, NV50_THIRD_PARTY_P2P depends on direct P2P
    // connectivity, hence the check.
    //
    if (!((connectivity == P2P_CONNECTIVITY_PCIE_PROPRIETARY) ||
          (connectivity == P2P_CONNECTIVITY_PCIE_BAR1) ||
          (connectivity == P2P_CONNECTIVITY_NVLINK) ||
          (connectivity == P2P_CONNECTIVITY_C2C)))
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    *pP2PWriteCapable = (p2PWriteCapsStatus == NV0000_P2P_CAPS_STATUS_OK);
    *pP2PReadCapable = (p2PReadCapsStatus == NV0000_P2P_CAPS_STATUS_OK);
    *pP2PAtomicsCapable = (p2PAtomicsCapsStatus == NV0000_P2P_CAPS_STATUS_OK);

    if (pConnectivity != NULL)
    {
        *pConnectivity = connectivity;
    }

    return status;
}

static NV_STATUS
_kp2pCapsGetStatusIndirectOverNvLink
(
    NvU32 gpuMask,
    NvU8 *pP2PWriteCapStatus,
    NvU8 *pP2PReadCapStatus
)
{
    OBJGPU *pGpu = NULL;
    NvU32 gpuInstance  = 0;
    OBJGPU *pFirstGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    NvBool bIndirectPeers = NV_FALSE;
    KernelBif *pKernelBif = GPU_GET_KERNEL_BIF(pFirstGpu);

    if ((pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_DEFAULT) &&
        (pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_NVLINK))
    {
        *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        return NV_OK;
    }

    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        bIndirectPeers = gpumgrCheckIndirectPeer(pFirstGpu, pGpu);
        if (!bIndirectPeers)
        {
            break;
        }
    }

    if (bIndirectPeers)
    {
        *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_OK;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    }
    else
    {
        *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    }

    return NV_OK;
}

static NV_STATUS
_gpumgrGetP2PCapsStatusOverNvLink
(
    NvU32 gpuMask,
    NvU8 *pP2PWriteCapStatus,
    NvU8 *pP2PReadCapStatus
)
{
    OBJGPU *pGpu = NULL;
    NvU32 gpuInstance  = 0;
    NV_STATUS status;
    OBJGPU *pFirstGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    RMTIMEOUT timeout;
    NvU32 linkTrainingTimeout = 10000000;
    KernelBif *pKernelBif = NULL;
    KernelNvlink *pKernelNvlink = NULL;

    NV_ASSERT_OR_RETURN(pFirstGpu != NULL, NV_ERR_INVALID_ARGUMENT);
    pKernelNvlink = GPU_GET_KERNEL_NVLINK(pFirstGpu);
    pKernelBif = GPU_GET_KERNEL_BIF(pFirstGpu);

    if ((pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_DEFAULT) &&
        (pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_NVLINK))
    {
        *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        return NV_OK;
    }

    //
    // Re-initialize to check loop back configuration if only single GPU in
    // requested mask.
    //
    gpuInstance = (gpumgrGetSubDeviceCount(gpuMask) > 1) ? gpuInstance : 0;

    // Check NvLink P2P connectivity
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        //
        // If ALI is enabled then poll to make sure that the links have
        // finished training on the two given gpus. If timeout occurs then
        // log an error, but continue on as there could be another gpu pair
        // that could have connectivity
        //

        if ((pKernelNvlink != NULL) &&
            knvlinkDiscoverPostRxDetLinks_HAL(pFirstGpu, pKernelNvlink, pGpu) == NV_OK)
        {
            // Check to make sure that the links are active

            gpuSetTimeout(pGpu, linkTrainingTimeout, &timeout, IS_SILICON(pGpu) ?
                (GPU_TIMEOUT_FLAGS_BYPASS_THREAD_STATE | GPU_TIMEOUT_FLAGS_DEFAULT) : 0);
            do
            {
                status = gpuCheckTimeout(pGpu, &timeout);

                if (knvlinkCheckTrainingIsComplete(pFirstGpu, pGpu, pKernelNvlink) == NV_OK)
                {
                    break;
                }

                if (status == NV_ERR_TIMEOUT)
                {
                    NV_PRINTF(LEVEL_ERROR,
                            "Links failed to train for the given gpu pairs!\n");
                    knvlinkLogAliDebugMessages(pFirstGpu, pKernelNvlink, NV_TRUE);
                    return status;
                }
            }
            while(status != NV_ERR_TIMEOUT);
        }

        // Ensure that we can create a NvLink P2P object between the two object
        if ((pKernelNvlink != NULL) &&
            knvlinkIsNvlinkP2pSupported(pFirstGpu, pKernelNvlink, pGpu))
        {
            // Ensure training completes on legacy nvlink devices
            status = knvlinkTrainP2pLinksToActive(pFirstGpu, pGpu, pKernelNvlink);
            NV_ASSERT(status == NV_OK);

            if (status != NV_OK)
            {
                *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
                *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
                return NV_OK;
            }
        }
        else
        {
            *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            return NV_OK;
        }
    }

    *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_OK;
    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    return NV_OK;
}

static NV_STATUS
_kgetP2PCapsStatusOverC2C
(
    NvU32 gpuMask,
    NvU8 *pP2PWriteCapStatus,
    NvU8 *pP2PReadCapStatus
)
{
    OBJGPU     *pGpu        = NULL;
    KernelBif  *pKernelBif  = NULL;
    NvU32      gpuInstance  = 0;
    OBJGPU     *pFirstGpu   = gpumgrGetNextGpu(gpuMask, &gpuInstance);

    NV_ASSERT_OR_RETURN(pFirstGpu != NULL, NV_ERR_INVALID_ARGUMENT);
    pKernelBif = GPU_GET_KERNEL_BIF(pFirstGpu);

    if ((pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_DEFAULT) &&
        (pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_C2C))
    {
        *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        return NV_OK;
    }

    //
    // Re-initialize to check loop back configuration if only single GPU in
    // requested mask.
    //
    gpuInstance = (gpumgrGetSubDeviceCount(gpuMask) > 1) ? gpuInstance : 0;

    // Check C2C P2P connectivity
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        // Ensure that we can create a C2C P2P object between the two object

        if (!kbifIsC2CP2PSupported_HAL(pFirstGpu, pKernelBif, pGpu))
        {

            *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            return NV_OK;
        }
    }
    *pP2PReadCapStatus  = NV0000_P2P_CAPS_STATUS_OK;
    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    return NV_OK;
}

// Returns true if overrides are enabled for PCI-E.
static NvBool
_kp2pCapsCheckStatusOverridesForPcie
(
    NvU32 gpuMask,
    NvU8 *pP2PWriteCapStatus,
    NvU8 *pP2PReadCapStatus,
    NvU8 *pP2PAtomicsCapStatus
)
{
    KernelBif *pKernelBif  = NULL;
    NvU32      gpuInstance = 0;
    OBJGPU    *pGpu        = NULL;

    // Check overrides for all GPUs in the mask.
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        pKernelBif = GPU_GET_KERNEL_BIF(pGpu);
        if (pKernelBif->p2pOverride != BIF_P2P_NOT_OVERRIDEN)
        {
            switch(DRF_VAL(_REG_STR, _CL_FORCE_P2P, _READ, pKernelBif->p2pOverride))
            {
                case NV_REG_STR_CL_FORCE_P2P_READ_DISABLE:
                    *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY;
                    break;
                case NV_REG_STR_CL_FORCE_P2P_READ_ENABLE:
                    *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_OK;
                    break;
                default:
                    break;
            }

            switch(DRF_VAL(_REG_STR, _CL_FORCE_P2P, _WRITE, pKernelBif->p2pOverride))
            {
                case NV_REG_STR_CL_FORCE_P2P_WRITE_DISABLE:
                    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY;
                    break;
                case NV_REG_STR_CL_FORCE_P2P_WRITE_ENABLE:
                    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;
                    break;
                default:
                    break;
            }

            switch(DRF_VAL(_REG_STR, _CL_FORCE_P2P, _ATOMICS, pKernelBif->p2pOverride))
            {
                case NV_REG_STR_CL_FORCE_P2P_ATOMICS_DISABLE:
                    *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY;
                    break;
                case NV_REG_STR_CL_FORCE_P2P_ATOMICS_ENABLE:
                    *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_OK;
                    break;
                default:
                    break;
            }

            return NV_TRUE;
        }

    }

    return NV_FALSE;
}

/**
 * @brief Discover PCIe host topology independently of the selected transport
 *
 * @param[in]  gpuMask                      GPU pair mask
 * @param[out] pP2PWriteCapStatus           Pointer to get the P2P write capability
 * @param[out] pP2PReadCapStatus            Pointer to get the P2P read capability
 * @param[out] pbCommonPciSwitch            To return if GPUs are on a common PCIE switch
 * @param[out] pbHypervisorOverride          Whether hypervisor policy accepted the pair
 *
 * @returns NV_OK with topology-specific capability statuses
 */
static NV_STATUS
_kp2pCapsGetTopologyStatusOverPcie
(
    NvU32   gpuMask,
    NvU8   *pP2PWriteCapStatus,
    NvU8   *pP2PReadCapStatus,
    NvBool *pbCommonPciSwitch,
    NvBool *pbHypervisorOverride
)
{
    OBJSYS *pSys = SYS_GET_INSTANCE();
    OBJCL *pCl = SYS_GET_CL(pSys);
    OBJHYPERVISOR *pHypervisor = SYS_GET_HYPERVISOR(pSys);
    OBJGPU *pGpu = NULL;
    OBJGPU *pFirstGpu = NULL;
    NvU32 gpuInstance = 0;
    NvU32 iohDomainRef = 0xFFFFFFFF;
    NvU8 iohBusRef = 0xFF;
    NvU8 pciSwitchBus = 0;
    NvU8 pciSwitchBusRef = 0xFF;
    NvBool bCommonPciSwitchFound = NV_TRUE;

    *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    *pbHypervisorOverride = NV_FALSE;

    // Hypervisor policy is already a complete host-topology decision.
    if (pHypervisor &&
        pHypervisor->bDetected &&
        hypervisorPcieP2pDetection(pHypervisor, gpuMask))
    {
        *pbCommonPciSwitch = NV_FALSE;
        *pbHypervisorOverride = NV_TRUE;
        return NV_OK;
    }

    if (!pCl->ChipsetInitialized)
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pbCommonPciSwitch = NV_FALSE;
        return NV_OK;
    }

    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        NvU16 deviceID;

        if (pGpu->gpuClData.rootPort.addr.valid &&
            (pGpu->gpuClData.rootPort.VendorID == PCI_VENDOR_ID_INTEL))
        {
            deviceID = pGpu->gpuClData.rootPort.DeviceID;

            if (((deviceID >= DEVICE_ID_INTEL_3408_ROOT_PORT) &&
                 (deviceID <= DEVICE_ID_INTEL_3411_ROOT_PORT)) ||
                ((deviceID >= DEVICE_ID_INTEL_3C02_ROOT_PORT) &&
                 (deviceID <= DEVICE_ID_INTEL_3C0B_ROOT_PORT)) ||
                ((deviceID >= DEVICE_ID_INTEL_0E02_ROOT_PORT) &&
                 (deviceID <= DEVICE_ID_INTEL_0E0B_ROOT_PORT)) ||
                ((deviceID >= DEVICE_ID_INTEL_2F01_ROOT_PORT) &&
                 (deviceID <= DEVICE_ID_INTEL_2F0B_ROOT_PORT)) ||
                ((deviceID >= DEVICE_ID_INTEL_6F01_ROOT_PORT) &&
                 (deviceID <= DEVICE_ID_INTEL_6F0B_ROOT_PORT)) ||
                (deviceID == DEVICE_ID_INTEL_3420_ROOT_PORT) ||
                (deviceID == DEVICE_ID_INTEL_3421_ROOT_PORT))
            {
                if (iohDomainRef == 0xFFFFFFFF)
                {
                    iohDomainRef = pGpu->gpuClData.rootPort.addr.domain;
                    iohBusRef = pGpu->gpuClData.rootPort.addr.bus;
                }
                else if ((iohDomainRef != pGpu->gpuClData.rootPort.addr.domain) ||
                         (iohBusRef != pGpu->gpuClData.rootPort.addr.bus))
                {
                    *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED;
                    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED;
                    *pbCommonPciSwitch = NV_FALSE;
                    return NV_OK;
                }
            }
        }

        if (pFirstGpu == NULL)
        {
            pFirstGpu = pGpu;
            continue;
        }

        if (!areGpusP2PCompatible(pFirstGpu, pGpu))
        {
            *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pbCommonPciSwitch = NV_FALSE;
            return NV_OK;
        }

        clFindCommonDownstreamBR(pFirstGpu, pGpu, pCl, &pciSwitchBus);
        if (pciSwitchBusRef == 0xFF)
            pciSwitchBusRef = pciSwitchBus;

        if ((pciSwitchBus == 0xFF) || (pciSwitchBusRef != pciSwitchBus))
            bCommonPciSwitchFound = NV_FALSE;
    }

    if ((!pCl->bPciePeerReadCapable || !pCl->bPciePeerWriteCapable) &&
        !bCommonPciSwitchFound)
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_CHIPSET_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_CHIPSET_NOT_SUPPORTED;
    }

    *pbCommonPciSwitch = bCommonPciSwitchFound;
    return NV_OK;
}

/**
 * @brief Check GPU Pcie mailbox P2P capability
 *
 * The host/topology result is supplied by the transport-neutral discovery
 * helper so a cached mailbox result cannot bypass discovery needed by BAR1.
 */
static NV_STATUS
_kp2pCapsGetStatusOverPcie
(
    NvU32   gpuMask,
    NvU8   *pP2PWriteCapStatus,
    NvU8   *pP2PReadCapStatus,
    NvU8    topologyWriteCapStatus,
    NvU8    topologyReadCapStatus,
    NvBool  bHypervisorOverride
)
{
    OBJGPU *pGpu = NULL;
    OBJGPU *pFirstGpu = NULL;
    NvU32 gpuInstance = 0;
    KernelBif *pKernelBif = NULL;
    NvU8 gpuP2PReadCapsStatus = NV0000_P2P_CAPS_STATUS_OK;
    NvU8 gpuP2PWriteCapsStatus = NV0000_P2P_CAPS_STATUS_OK;
    NvU8 cachedReadCapsStatus;
    NvU8 cachedWriteCapsStatus;
    NvU32 lockedGpuMask = 0;
    NV_STATUS status = NV_OK;
    NvU8 unused; // atomics are never supported for mailbox PCIe.

    // Check if any overrides are enabled.
    if (_kp2pCapsCheckStatusOverridesForPcie(gpuMask, pP2PWriteCapStatus,
                                            pP2PReadCapStatus, &unused))
    {
        return NV_OK;
    }

    *pP2PWriteCapStatus = topologyWriteCapStatus;
    *pP2PReadCapStatus = topologyReadCapStatus;

    if (bHypervisorOverride)
        goto done;

    pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    pFirstGpu = pGpu;
    NV_ASSERT_OR_RETURN(pFirstGpu != NULL, NV_ERR_INVALID_ARGUMENT);

    if (IS_GSP_CLIENT(pGpu))
    {
        if (gpumgrGetPcieP2PCapsFromCache(gpuMask, &cachedWriteCapsStatus, &cachedReadCapsStatus))
        {
            if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK)
                *pP2PWriteCapStatus = cachedWriteCapsStatus;
            if (*pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
                *pP2PReadCapStatus = cachedReadCapsStatus;
            return NV_OK;
        }
    }

    // Check if GPUs have the HW P2P implementation

    if (IS_GSP_CLIENT(pFirstGpu))
    {
        // Lock GPUs
        lockedGpuMask = gpuMask;
        status = rmGpuGroupLockAcquire(0, GPU_LOCK_GRP_MASK,
            GPU_LOCK_FLAGS_SAFE_LOCK_UPGRADE, RM_LOCK_MODULES_P2P, &lockedGpuMask);

        // If we get NOTHING_TO_DO, we already have the needed locks, so don't free them
        if (status == NV_WARN_NOTHING_TO_DO)
            lockedGpuMask = 0;
        else if (status != NV_OK)
        {
            lockedGpuMask = 0;
            goto done;
        }
    }

    gpuInstance = 0;
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        // GPU specific P2P caps
        pKernelBif = GPU_GET_KERNEL_BIF(pGpu);
        if (pKernelBif->getProperty(pKernelBif, PDB_PROP_KBIF_P2P_WRITES_DISABLED))
            gpuP2PWriteCapsStatus = NV0000_P2P_CAPS_STATUS_GPU_NOT_SUPPORTED;
        if (pKernelBif->getProperty(pKernelBif, PDB_PROP_KBIF_P2P_READS_DISABLED))
            gpuP2PReadCapsStatus = NV0000_P2P_CAPS_STATUS_GPU_NOT_SUPPORTED;

        //
        // Reconcile the system and GPU specific P2P information
        // The system P2P status takes precedence
        // Do not override status from not OK to OK
        //
        if (*pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pP2PReadCapStatus = (gpuGetPcieP2PReadCaps(pGpu) == NV0000_P2P_CAPS_STATUS_OK) ?
                                    gpuP2PReadCapsStatus : gpuGetPcieP2PReadCaps(pGpu);
        }

        if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pP2PWriteCapStatus =  (gpuGetPcieP2PWriteCaps(pGpu) == NV0000_P2P_CAPS_STATUS_OK ?
                                    gpuP2PWriteCapsStatus : gpuGetPcieP2PWriteCaps(pGpu));
        }

        // No need to continue if P2P is not supported
        if ((*pP2PReadCapStatus != NV0000_P2P_CAPS_STATUS_OK) &&
            (*pP2PWriteCapStatus != NV0000_P2P_CAPS_STATUS_OK))
        {
            break;
        }
    }

done:
    if (lockedGpuMask != 0)
    {
        rmGpuGroupLockRelease(lockedGpuMask, GPUS_LOCK_FLAGS_NONE);
    }

    if (status != NV_OK)
    {
        if (*pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        }
        if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        }
    }

    //
    // Not fatal if failing, effect would be perf degradation as we would not hit the cache.
    // So just assert.
    //
    gpuInstance = 0;
    pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    if (IS_GSP_CLIENT(pGpu))
    {
       NV_ASSERT_OK(gpumgrStorePcieP2PCapsCache(gpuMask, *pP2PWriteCapStatus, *pP2PReadCapStatus));
    }
    return status;
}

/**
 * @brief Check the host system BAR1 P2P capability
 *
 * @param[in]  pGpu                         OBJGPU pointer
 * @param[out] pP2PWriteCapStatus           Pointer to get the P2P write capability
 * @param[out] pP2PReadCapStatus            Pointer to get the P2P read capability
 * @param[out] bCommonPciSwitchFound        To indicate if GPUs are on a common PCIE switch
 *
 * @returns NV_OK
 */
static NV_STATUS
_p2pCapsGetHostSystemStatusOverPcieBar1
(
    OBJGPU  *pGpu,
    NvU8    *pP2PWriteCapStatus,
    NvU8    *pP2PReadCapStatus,
    NvBool   bCommonPciSwitchFound
)
{
    OBJSYS *pSys = SYS_GET_INSTANCE();

    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_OK;

    if (bCommonPciSwitchFound ||
        (pSys->cpuInfo.type == NV0000_CTRL_SYSTEM_CPU_TYPE_RYZEN) ||
        (pSys->cpuInfo.type == NV0000_CTRL_SYSTEM_CPU_TYPE_XEON_SPR))
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_OK;
    }
    else
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_CHIPSET_NOT_SUPPORTED;
        NV_PRINTF(LEVEL_INFO, "Unrecognized CPU. Read Cap is disabled\n");
    }

    return NV_OK;
}

/**
 * @brief Cross check the PCIe routing for each GPU described by the mask
 * to verify whether the PCIe topology supports BAR1 atomics.
 *
 * @param[in]  pGpu                           OBJGPU pointer
 * @param[out] pP2PAtomicsCapStatus           Pointer to get the P2P atomics capability
 *
 * @returns NV_OK, if GPUs support atomics
 *          NV_ERR_NOT_SUPPORTED, if atomics are not supported
 */
static NV_STATUS
_p2pCapsGetPcieToplogySupportForBar1Atomics
(
    NvU32 gpuMask,
    NvU8 *pP2PAtomicsCapStatus
)
{
    // TODO: Bug 4648955 RM needs to perform discovery of PCIe topology support for atomics.
    return NV_ERR_NOT_SUPPORTED;
}

/**
 * @brief Check GPU Pcie BAR1 P2P capability
 *
 * @param[in]  pGpu                         OBJGPU pointer
 * @param[out] pP2PWriteCapStatus           Pointer to get the P2P write capability
 * @param[out] pP2PReadCapStatus            Pointer to get the P2P read capability
 * @param[out] bCommonPciSwitchFound        To indicate if GPUs are on a common PCIE switch
 *
 * @returns NV_OK, if GPUs at least support read or write
 *          NV_ERR_NOT_SUPPORTED, if GPUs does not support read and write
 */
static NV_STATUS
_kp2pCapsGetStatusOverPcieBar1
(
    NvU32   gpuMask,
    NvU8   *pP2PWriteCapStatus,
    NvU8   *pP2PReadCapStatus,
    NvU8   *pP2PAtomicsCapStatus,
    NvU8    topologyWriteCapStatus,
    NvU8    topologyReadCapStatus,
    NvBool  bCommonPciSwitchFound,
    NvBool  bBar1Eligible
)
{
    NvU32      gpuInstance = 0;
    OBJGPU    *pFirstGpu   = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    KernelBif *pKernelBif  = GPU_GET_KERNEL_BIF(pFirstGpu);
    NvU8 writeCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    NvU8 readCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    NvU8 atomicsCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;

    *pP2PWriteCapStatus = topologyWriteCapStatus;
    *pP2PReadCapStatus = topologyReadCapStatus;
    *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;

    if (((pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_DEFAULT) &&
         (pKernelBif->forceP2PType != NV_REG_STR_RM_FORCE_P2P_TYPE_PCIEP2P))
        ||
        ((pKernelBif->pcieP2PType != NV_REG_STR_RM_PCIEP2P_TYPE_BAR1) &&
         (pKernelBif->pcieP2PType != NV_REG_STR_RM_PCIEP2P_TYPE_AUTO)))
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        return NV_ERR_NOT_SUPPORTED;
    }

    if (!bBar1Eligible)
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_GPU_NOT_SUPPORTED;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_GPU_NOT_SUPPORTED;
        return NV_ERR_NOT_SUPPORTED;
    }

    // Check if any overrides are enabled.
    if (_kp2pCapsCheckStatusOverridesForPcie(gpuMask, pP2PWriteCapStatus,
                                             pP2PReadCapStatus,
                                             pP2PAtomicsCapStatus))
    {
        return NV_OK;
    }

    if ((*pP2PReadCapStatus != NV0000_P2P_CAPS_STATUS_OK) ||
        (*pP2PWriteCapStatus != NV0000_P2P_CAPS_STATUS_OK))
    {
        return NV_ERR_NOT_SUPPORTED;
    }

    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
                          _p2pCapsGetHostSystemStatusOverPcieBar1(pFirstGpu, &writeCapStatus,
                            &readCapStatus, bCommonPciSwitchFound));

    if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        *pP2PWriteCapStatus = writeCapStatus;
    if (*pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        *pP2PReadCapStatus = readCapStatus;

    if ((*pP2PReadCapStatus != NV0000_P2P_CAPS_STATUS_OK) ||
        (*pP2PWriteCapStatus != NV0000_P2P_CAPS_STATUS_OK))
    {
        // return not supported if it does not support both operations
        return NV_ERR_NOT_SUPPORTED;
    }

    // If p2p traffic is supported, check the PCIe topology for atomics capability
    _p2pCapsGetPcieToplogySupportForBar1Atomics(gpuMask, &atomicsCapStatus);

    *pP2PAtomicsCapStatus = atomicsCapStatus;

    return NV_OK;
}

NV_STATUS
p2pGetCapsStatus
(
    NvU32 gpuMask,
    NvU8 *pP2PWriteCapStatus,
    NvU8 *pP2PReadCapStatus,
    NvU8 *pP2PAtomicsCapStatus,
    P2P_CONNECTIVITY *pConnectivity
)
{
    OBJSYS       *pSys          = SYS_GET_INSTANCE();
    KernelNvlink *pKernelNvlink = NULL;
    OBJGPU       *pGpu          = NULL;
    NvU32         gpuInstance   = 0;
    NvBool        bCommonSwitchFound = NV_FALSE;
    NvBool        bHypervisorPcieOverride = NV_FALSE;
    NvBool        bPcieP2PEnabled = NV_TRUE;
    NvBool        bBar1Eligible = NV_TRUE;
    NvU8          topologyWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    NvU8          topologyReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;

    if ((pP2PWriteCapStatus == NULL) ||
        (pP2PReadCapStatus == NULL)  ||
        (pConnectivity == NULL)
        )
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    // Default values
    *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
    *pConnectivity = P2P_CONNECTIVITY_UNKNOWN;

    // MIG-Nvlink-P2P can be incompatible, so check compatibility for all GPUs
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        KernelMIGManager *pKernelMIGManager = GPU_GET_KERNEL_MIG_MANAGER(pGpu);
        NvBool bSmcNvLinkP2PSupported = ((pKernelMIGManager != NULL) &&
                                         kmigmgrIsMIGNvlinkP2PSupported(pGpu, pKernelMIGManager));

        // If any of the GPU has MIG enabled, return with no P2P support
        if (!bSmcNvLinkP2PSupported)
        {
            NV_PRINTF(LEVEL_NOTICE,
                  "P2P is marked unsupported with MIG for GPU instance = 0x%x\n",
                  gpuInstance);
            return NV_OK;
        }
    }

    gpuInstance = 0;

    // Check C2C P2P connectivity
    if (_kgetP2PCapsStatusOverC2C(gpuMask, pP2PWriteCapStatus,
                                 pP2PReadCapStatus) == NV_OK)
    {
        if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK &&
            *pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pConnectivity = P2P_CONNECTIVITY_C2C;
            // todo: move this, and other instances to cap check functions.
            *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_OK;
            return NV_OK;
        }
    }

    // Check NvLink P2P connectivity.
    if (_gpumgrGetP2PCapsStatusOverNvLink(gpuMask, pP2PWriteCapStatus,
                                          pP2PReadCapStatus) == NV_OK)
    {
        if (*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK &&
            *pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK)
        {
            *pConnectivity = P2P_CONNECTIVITY_NVLINK;
            *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_OK;
            return NV_OK;
        }
    }

    //
    // On NVSwitch systems, if the NVLink P2P path fails, don't fall back to
    // other P2P paths. To ensure that, check if any GPU in the mask has NVLink
    // support. If supported, enforce NVSwitch/NVLink connectivity by returning
    // NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED.
    //
    gpuInstance = 0;
    while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
    {
        pKernelNvlink = GPU_GET_KERNEL_NVLINK(pGpu);
        NVLINK_BIT_VECTOR * pBitVector = knvlinkGetDiscoveredLinkMask(pGpu, pKernelNvlink);
        if ( (pKernelNvlink != NULL)                &&
             (pBitVector != NULL)                   &&
             (!bitVectorTestAllCleared(pBitVector)) &&
             (pSys->getProperty(pSys, PDB_PROP_SYS_NVSWITCH_IS_PRESENT) ||
                knvlinkIsNvswitchProxyPresent(pGpu, pKernelNvlink) ||
                GPU_IS_NVSWITCH_DETECTED(pGpu)))
        {
            *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
            return NV_OK;
        }
    }

    // We didn't find direct P2P, so check for indirect P2P.
    if (_kp2pCapsGetStatusIndirectOverNvLink(gpuMask, pP2PWriteCapStatus,
                                            pP2PReadCapStatus) == NV_OK)
    {
        if ((*pP2PWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK) &&
            (*pP2PReadCapStatus == NV0000_P2P_CAPS_STATUS_OK))
        {
            *pConnectivity = P2P_CONNECTIVITY_NVLINK_INDIRECT;
            *pP2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_OK;
            return NV_OK;
        }
    }

    //
    // Check PCIE P2P connectivity.
    //
    // We can control P2P connectivity for PCI-E peers using regkeys, hence
    // if either read or write is supported, return success. See
    // _kp2pCapsCheckStatusOverridesForPcie for details.
    //
    gpuInstance = 0;
    pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
    NV_ASSERT_OR_RETURN(pGpu != NULL, NV_ERR_INVALID_ARGUMENT);

    bBar1Eligible = GPU_GET_KERNEL_BUS(pGpu)->getProperty(
        GPU_GET_KERNEL_BUS(pGpu), PDB_PROP_KBUS_SUPPORT_BAR1_P2P_BY_DEFAULT);
    bPcieP2PEnabled = _kp2pCapsIsPcieP2PEnabled(pGpu);
    if (!bPcieP2PEnabled)
    {
        *pP2PReadCapStatus = NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY;
        *pP2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_DISABLED_BY_REGKEY;
        _kp2pCapsLogPolicyOnce(
            gpuMask, pGpu, NV_FALSE, bBar1Eligible,
            pGpu->pcieP2PWriteCaps, pGpu->pcieP2PReadCaps,
            *pP2PWriteCapStatus, *pP2PReadCapStatus, *pConnectivity);
        return NV_OK;
    }

    if (_kp2pCapsGetTopologyStatusOverPcie(gpuMask, &topologyWriteCapStatus,
                                           &topologyReadCapStatus, &bCommonSwitchFound,
                                           &bHypervisorPcieOverride) == NV_OK)
    {
        NvU8 mailboxWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        NvU8 mailboxReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        NvU8 bar1P2PWriteCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        NvU8 bar1P2PReadCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        NvU8 bar1P2PAtomicsCapStatus = NV0000_P2P_CAPS_STATUS_NOT_SUPPORTED;
        OBJGPU *pFirstPcieGpu;
        KernelBif *pFirstKernelBif;

        gpuInstance = 0;
        pFirstPcieGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance);
        NV_ASSERT_OR_RETURN(pFirstPcieGpu != NULL, NV_ERR_INVALID_ARGUMENT);
        pFirstKernelBif = GPU_GET_KERNEL_BIF(pFirstPcieGpu);
        bBar1Eligible = gpumgrGetSubDeviceCount(gpuMask) > 1;

        while ((pGpu = gpumgrGetNextGpu(gpuMask, &gpuInstance)) != NULL)
        {
            if (!kbusIsPcieBar1P2PMappingSupported_HAL(
                    pFirstPcieGpu, GPU_GET_KERNEL_BUS(pFirstPcieGpu),
                    pGpu, GPU_GET_KERNEL_BUS(pGpu)))
            {
                bBar1Eligible = NV_FALSE;
                break;
            }
        }

        // Always preserve the raw mailbox result for transport-specific users
        // and diagnostics, even when BAR1 is the effective transport.
        _kp2pCapsGetStatusOverPcie(gpuMask, &mailboxWriteCapStatus,
                                   &mailboxReadCapStatus, topologyWriteCapStatus,
                                   topologyReadCapStatus, bHypervisorPcieOverride);

        if (pFirstKernelBif->pcieP2PType == NV_REG_STR_RM_PCIEP2P_TYPE_MAILBOX)
        {
            *pP2PWriteCapStatus = mailboxWriteCapStatus;
            *pP2PReadCapStatus = mailboxReadCapStatus;
            if ((mailboxWriteCapStatus == NV0000_P2P_CAPS_STATUS_OK) ||
                (mailboxReadCapStatus == NV0000_P2P_CAPS_STATUS_OK))
            {
                *pConnectivity = P2P_CONNECTIVITY_PCIE_PROPRIETARY;
            }
            _kp2pCapsLogPolicyOnce(
                gpuMask, pFirstPcieGpu, bPcieP2PEnabled, bBar1Eligible,
                mailboxWriteCapStatus, mailboxReadCapStatus,
                *pP2PWriteCapStatus, *pP2PReadCapStatus, *pConnectivity);
            return NV_OK;
        }

        // BAR1 and AUTO are independent of mailbox success and never fall
        // back to mailbox when BAR1 fails.
        if (_kp2pCapsGetStatusOverPcieBar1(gpuMask, &bar1P2PWriteCapStatus,
                                           &bar1P2PReadCapStatus,
                                           &bar1P2PAtomicsCapStatus,
                                           topologyWriteCapStatus,
                                           topologyReadCapStatus,
                                           bCommonSwitchFound,
                                           bBar1Eligible) == NV_OK)
        {
            *pConnectivity = P2P_CONNECTIVITY_PCIE_BAR1;
        }

        *pP2PWriteCapStatus = bar1P2PWriteCapStatus;
        *pP2PReadCapStatus = bar1P2PReadCapStatus;
        *pP2PAtomicsCapStatus = bar1P2PAtomicsCapStatus;
        _kp2pCapsLogPolicyOnce(
            gpuMask, pFirstPcieGpu, bPcieP2PEnabled, bBar1Eligible,
            mailboxWriteCapStatus, mailboxReadCapStatus,
            *pP2PWriteCapStatus, *pP2PReadCapStatus, *pConnectivity);
        return NV_OK;
    }

    return NV_OK;
}

NV_STATUS
subdeviceCtrlCmdGetP2pCaps_VF
(
    Subdevice *pSubdevice,
    NV2080_CTRL_GET_P2P_CAPS_PARAMS *pParams
)
{
    NvU32 i;
    NV_STATUS status = NV_OK;
    CALL_CONTEXT *pCallContext = resservGetTlsCallContext();
    RS_RES_CONTROL_PARAMS_INTERNAL *pControlParams = pCallContext->pControlParams;
    NV2080_CTRL_GET_P2P_CAPS_PARAMS *pShimParams;
    OBJGPU *pGpu = gpumgrGetGpuFromSubDeviceInst(pSubdevice->deviceInst,
                                                 pSubdevice->subDeviceInst);

    NV_CHECK_OR_RETURN(LEVEL_ERROR,
                       pParams->bAllCaps ||
                       (pParams->peerGpuCount <= NV0000_CTRL_SYSTEM_MAX_ATTACHED_GPUS),
                       NV_ERR_INVALID_ARGUMENT);
    NV_CHECK_OR_RETURN(LEVEL_ERROR, (pParams->bUseUuid == NV_FALSE), NV_ERR_NOT_SUPPORTED);

    // TODO: GPUSWSEC-1433 remove this check to enable this control for baremetal
    if (!IS_VGPU_GSP_PLUGIN_OFFLOAD_ENABLED(pGpu))
        return NV_ERR_NOT_SUPPORTED;

    pShimParams = portMemAllocNonPaged(sizeof *pShimParams);

    NV_CHECK_OR_RETURN(LEVEL_INFO, pShimParams != NULL, NV_ERR_NO_MEMORY);

    portMemCopy(pShimParams, sizeof *pShimParams, pParams, sizeof *pParams);

    if (!pShimParams->bAllCaps)
    {
        // Must translate Guest GpuIds to Guest UUIDs
        for (i = 0; i < pShimParams->peerGpuCount; i++)
        {
            NV2080_CTRL_GPU_P2P_PEER_CAPS_PEER_INFO *pParamsPeerInfo = &pShimParams->peerGpuCaps[i];
            OBJGPU *pRemoteGpu = gpumgrGetGpuFromId(pParamsPeerInfo->gpuId);

            NV_CHECK_OR_ELSE(LEVEL_INFO, pRemoteGpu != NULL,
                             status = NV_ERR_INVALID_ARGUMENT; goto done);

            portMemCopy(pParamsPeerInfo->gpuUuid,
                        sizeof(pParamsPeerInfo->gpuUuid),
                        pRemoteGpu->gpuUuid.uuid,
                        sizeof(pRemoteGpu->gpuUuid.uuid));
        }
    }

    pShimParams->bUseUuid = 1;

    NV_RM_RPC_CONTROL(pGpu,
                      pControlParams->hClient,
                      pControlParams->hObject,
                      pControlParams->cmd,
                      pShimParams,
                      sizeof *pShimParams,
                      status);
    if (status != NV_OK)
    {
        goto done;
    }

    // If bAllCaps, transfer additional output gpuIds and peerGpuCount
    if (pParams->bAllCaps)
    {
        pParams->peerGpuCount = pShimParams->peerGpuCount;

        for (i = 0; i < pParams->peerGpuCount; ++i)
        {
            // Use UUID to compute corresponding Guest GPU ID
            OBJGPU *pRemoteGpu = gpumgrGetGpuFromUuid(pShimParams->peerGpuCaps[i].gpuUuid,
                                                      DRF_DEF(2080_GPU_CMD, _GPU_GET_GID_FLAGS, _TYPE, _SHA1) |
                                                      DRF_DEF(2080_GPU_CMD, _GPU_GET_GID_FLAGS, _FORMAT, _BINARY));

            NV_CHECK_OR_ELSE(LEVEL_INFO, pRemoteGpu != NULL,
                             status = NV_ERR_GPU_UUID_NOT_FOUND; goto done);

            pParams->peerGpuCaps[i].gpuId = pRemoteGpu->gpuId;
        }
    }

    // Transfer output values from shimParams to user params.
    for (i = 0; i < pParams->peerGpuCount; ++i)
    {
        pParams->peerGpuCaps[i].p2pCaps = pShimParams->peerGpuCaps[i].p2pCaps;
        pParams->peerGpuCaps[i].p2pOptimalReadCEs = pShimParams->peerGpuCaps[i].p2pOptimalReadCEs;
        pParams->peerGpuCaps[i].p2pOptimalWriteCEs = pShimParams->peerGpuCaps[i].p2pOptimalWriteCEs;
        portMemCopy(pParams->peerGpuCaps[i].p2pCapsStatus,
                    sizeof(pParams->peerGpuCaps[i].p2pCapsStatus),
                    pShimParams->peerGpuCaps[i].p2pCapsStatus,
                    sizeof(pShimParams->peerGpuCaps[i].p2pCapsStatus));
        pParams->peerGpuCaps[i].busPeerId = pShimParams->peerGpuCaps[i].busPeerId;
    }

done:
    portMemFree(pShimParams);

    return status;
}
