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
#include <stdint.h>

#include "../kernel-open/nvidia/dmabuf-gdr-topology-policy.h"

int main(void)
{
    uint64_t barStart = UINT64_C(0x26000000000);
    uint64_t barSize = UINT64_C(0x400000000);
    uint64_t exactMask = barStart + barSize - 1;
    uint64_t wideMask = UINT64_C(0xffffffffffffffff);

    assert(DMABUF_GDR_BAR_ADDRESSABLE(barStart, barSize, exactMask));
    assert(DMABUF_GDR_BAR_ADDRESSABLE(barStart, barSize, wideMask));
    assert(!DMABUF_GDR_BAR_ADDRESSABLE(barStart, barSize, exactMask - 1));
    assert(!DMABUF_GDR_BAR_ADDRESSABLE(barStart, UINT64_C(0), wideMask));
    assert(!DMABUF_GDR_BAR_ADDRESSABLE(UINT64_MAX - UINT64_C(0x1000),
                                         UINT64_C(0x2000), UINT64_MAX));

    assert(DMABUF_GDR_TOPOLOGY_ALLOWED(1, 1, 4,
                                         barStart, barSize, wideMask));
    assert(!DMABUF_GDR_TOPOLOGY_ALLOWED(0, 1, 4,
                                          barStart, barSize, wideMask));
    assert(!DMABUF_GDR_TOPOLOGY_ALLOWED(1, 0, 4,
                                          barStart, barSize, wideMask));
    assert(!DMABUF_GDR_TOPOLOGY_ALLOWED(1, 1, -1,
                                          barStart, barSize, wideMask));
    assert(!DMABUF_GDR_TOPOLOGY_ALLOWED(1, 1, 4,
                                          barStart, barSize, exactMask - 1));

    return 0;
}
