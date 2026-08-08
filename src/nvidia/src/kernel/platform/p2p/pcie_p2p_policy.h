/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Duc P. Tran
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

#ifndef PCIE_P2P_POLICY_H
#define PCIE_P2P_POLICY_H

#define PCIE_P2P_POLICY_ENABLE_PCIE_DEFAULT   1U
#define PCIE_P2P_POLICY_ENABLE_DMABUF_DEFAULT 1U

typedef enum
{
    PCIE_P2P_POLICY_REQUEST_MAILBOX = 0,
    PCIE_P2P_POLICY_REQUEST_BAR1,
    PCIE_P2P_POLICY_REQUEST_AUTO,
} PCIE_P2P_POLICY_REQUEST;

typedef enum
{
    PCIE_P2P_POLICY_CONNECTIVITY_NONE = 0,
    PCIE_P2P_POLICY_CONNECTIVITY_MAILBOX,
    PCIE_P2P_POLICY_CONNECTIVITY_BAR1,
} PCIE_P2P_POLICY_CONNECTIVITY;

typedef enum
{
    PCIE_P2P_POLICY_STATUS_OK = 0,
    PCIE_P2P_POLICY_STATUS_CHIPSET_NOT_SUPPORTED = 1,
    PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED = 2,
    PCIE_P2P_POLICY_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED = 3,
    PCIE_P2P_POLICY_STATUS_DISABLED = 4,
    PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED = 5,
} PCIE_P2P_POLICY_STATUS;

typedef struct
{
    unsigned int enablePcieP2P;
    PCIE_P2P_POLICY_REQUEST request;
    unsigned int loopback;
    unsigned int pairCompatible;
    PCIE_P2P_POLICY_STATUS topologyReadStatus;
    PCIE_P2P_POLICY_STATUS topologyWriteStatus;
    unsigned int bar1Eligible;
    unsigned int staticBar1Available;
    unsigned int mailboxMappingActive;
    PCIE_P2P_POLICY_STATUS rawMailboxReadStatus;
    PCIE_P2P_POLICY_STATUS rawMailboxWriteStatus;
    PCIE_P2P_POLICY_STATUS bar1AtomicsStatus;
} PCIE_P2P_POLICY_INPUTS;

typedef struct
{
    PCIE_P2P_POLICY_CONNECTIVITY connectivity;
    PCIE_P2P_POLICY_STATUS effectiveReadStatus;
    PCIE_P2P_POLICY_STATUS effectiveWriteStatus;
    PCIE_P2P_POLICY_STATUS effectiveAtomicsStatus;
    PCIE_P2P_POLICY_STATUS rawMailboxReadStatus;
    PCIE_P2P_POLICY_STATUS rawMailboxWriteStatus;
} PCIE_P2P_POLICY_RESULT;

static inline PCIE_P2P_POLICY_RESULT
pcieP2pPolicySelect(const PCIE_P2P_POLICY_INPUTS *pInputs)
{
    PCIE_P2P_POLICY_RESULT result = {
        PCIE_P2P_POLICY_CONNECTIVITY_NONE,
        PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED,
        PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED,
        PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED,
        pInputs->rawMailboxReadStatus,
        pInputs->rawMailboxWriteStatus,
    };

    if (!pInputs->enablePcieP2P)
    {
        result.effectiveReadStatus = PCIE_P2P_POLICY_STATUS_DISABLED;
        result.effectiveWriteStatus = PCIE_P2P_POLICY_STATUS_DISABLED;
        return result;
    }

    if (pInputs->loopback || !pInputs->pairCompatible)
        return result;

    result.effectiveReadStatus = pInputs->topologyReadStatus;
    result.effectiveWriteStatus = pInputs->topologyWriteStatus;
    if ((result.effectiveReadStatus != PCIE_P2P_POLICY_STATUS_OK) ||
        (result.effectiveWriteStatus != PCIE_P2P_POLICY_STATUS_OK))
    {
        return result;
    }

    if (pInputs->request == PCIE_P2P_POLICY_REQUEST_MAILBOX)
    {
        result.effectiveReadStatus = pInputs->rawMailboxReadStatus;
        result.effectiveWriteStatus = pInputs->rawMailboxWriteStatus;
        if ((result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_OK) ||
            (result.effectiveWriteStatus == PCIE_P2P_POLICY_STATUS_OK))
        {
            result.connectivity = PCIE_P2P_POLICY_CONNECTIVITY_MAILBOX;
        }
        return result;
    }

    if (!pInputs->bar1Eligible)
    {
        result.effectiveReadStatus = PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED;
        result.effectiveWriteStatus = PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED;
        return result;
    }

    if (!pInputs->staticBar1Available || pInputs->mailboxMappingActive)
    {
        result.effectiveReadStatus = PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED;
        result.effectiveWriteStatus = PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED;
        return result;
    }

    result.connectivity = PCIE_P2P_POLICY_CONNECTIVITY_BAR1;
    result.effectiveAtomicsStatus = pInputs->bar1AtomicsStatus;
    return result;
}

static inline unsigned int
pcieP2pPolicyDmaBufEnabled(unsigned int enableDmaBufP2P)
{
    return enableDmaBufP2P != 0U;
}

#endif // PCIE_P2P_POLICY_H
