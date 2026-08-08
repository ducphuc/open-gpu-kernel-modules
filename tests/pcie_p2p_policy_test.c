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

#include <assert.h>

#include "../src/nvidia/src/kernel/platform/p2p/pcie_p2p_policy.h"

static PCIE_P2P_POLICY_INPUTS defaultInputs(void)
{
    PCIE_P2P_POLICY_INPUTS inputs = {
        PCIE_P2P_POLICY_ENABLE_PCIE_DEFAULT,
        PCIE_P2P_POLICY_REQUEST_AUTO,
        0,
        1,
        PCIE_P2P_POLICY_STATUS_OK,
        PCIE_P2P_POLICY_STATUS_OK,
        1,
        1,
        0,
        PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED,
        PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED,
        PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED,
    };

    return inputs;
}

static void assertBar1Selected(PCIE_P2P_POLICY_RESULT result)
{
    assert(result.connectivity == PCIE_P2P_POLICY_CONNECTIVITY_BAR1);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_OK);
    assert(result.effectiveWriteStatus == PCIE_P2P_POLICY_STATUS_OK);
    assert(result.effectiveAtomicsStatus == PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED);
    assert(result.rawMailboxReadStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);
    assert(result.rawMailboxWriteStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);
}

int main(void)
{
    PCIE_P2P_POLICY_INPUTS inputs = defaultInputs();
    PCIE_P2P_POLICY_RESULT result;

    assert(PCIE_P2P_POLICY_ENABLE_PCIE_DEFAULT == 1U);
    assert(PCIE_P2P_POLICY_ENABLE_DMABUF_DEFAULT == 1U);
    assert(pcieP2pPolicyDmaBufEnabled(1));
    assert(!pcieP2pPolicyDmaBufEnabled(0));

    // Raw mailbox GNS does not gate the independent default BAR1 path.
    assertBar1Selected(pcieP2pPolicySelect(&inputs));

    // The two feature switches are independent.
    inputs.enablePcieP2P = 0;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_DISABLED);
    assert(result.effectiveWriteStatus == PCIE_P2P_POLICY_STATUS_DISABLED);
    assert(pcieP2pPolicyDmaBufEnabled(1));
    inputs.enablePcieP2P = 1;
    assertBar1Selected(pcieP2pPolicySelect(&inputs));
    assert(!pcieP2pPolicyDmaBufEnabled(0));

    // Explicit diagnostic mailbox preserves its raw result.
    inputs.request = PCIE_P2P_POLICY_REQUEST_MAILBOX;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);
    assert(result.effectiveWriteStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);
    inputs.rawMailboxReadStatus = PCIE_P2P_POLICY_STATUS_OK;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.connectivity == PCIE_P2P_POLICY_CONNECTIVITY_MAILBOX);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_OK);

    // Explicit BAR1 and AUTO share the same fail-closed BAR1 policy.
    inputs = defaultInputs();
    inputs.request = PCIE_P2P_POLICY_REQUEST_BAR1;
    assertBar1Selected(pcieP2pPolicySelect(&inputs));
    inputs.request = PCIE_P2P_POLICY_REQUEST_AUTO;
    inputs.bar1Eligible = 0;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);
    assert(result.rawMailboxReadStatus == PCIE_P2P_POLICY_STATUS_GPU_NOT_SUPPORTED);

    // A mixed/ineligible pair and loopback both fail closed.
    inputs = defaultInputs();
    inputs.pairCompatible = 0;
    assert(pcieP2pPolicySelect(&inputs).connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);
    inputs = defaultInputs();
    inputs.loopback = 1;
    assert(pcieP2pPolicySelect(&inputs).connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);

    // Static BAR1 and mailbox-conflict predicates are mandatory.
    inputs = defaultInputs();
    inputs.staticBar1Available = 0;
    assert(pcieP2pPolicySelect(&inputs).connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);
    inputs = defaultInputs();
    inputs.mailboxMappingActive = 1;
    assert(pcieP2pPolicySelect(&inputs).connectivity == PCIE_P2P_POLICY_CONNECTIVITY_NONE);

    // Precise topology and explicit disable statuses are preserved.
    inputs = defaultInputs();
    inputs.topologyReadStatus = PCIE_P2P_POLICY_STATUS_CHIPSET_NOT_SUPPORTED;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_CHIPSET_NOT_SUPPORTED);
    inputs = defaultInputs();
    inputs.topologyWriteStatus = PCIE_P2P_POLICY_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.effectiveWriteStatus == PCIE_P2P_POLICY_STATUS_IOH_TOPOLOGY_NOT_SUPPORTED);
    inputs = defaultInputs();
    inputs.topologyReadStatus = PCIE_P2P_POLICY_STATUS_DISABLED;
    result = pcieP2pPolicySelect(&inputs);
    assert(result.effectiveReadStatus == PCIE_P2P_POLICY_STATUS_DISABLED);

    // Atomics remain independently unsupported.
    inputs = defaultInputs();
    result = pcieP2pPolicySelect(&inputs);
    assert(result.effectiveAtomicsStatus == PCIE_P2P_POLICY_STATUS_NOT_SUPPORTED);

    return 0;
}
