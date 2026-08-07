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

#include "../src/nvidia/arch/nvalloc/unix/include/dmabuf_gdr_policy.h"

int main(void)
{
    uint64_t windowStart = UINT64_C(0x20000000);
    uint64_t windowSize = UINT64_C(0x3dfe00000);
    uint64_t windowEnd = windowStart + windowSize;
    uint64_t emptySize = 0;

    assert(DMABUF_GDR_NONCOHERENT_ALLOWED(1, 0, 1, 1, 0, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(0, 0, 1, 1, 0, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(1, 1, 1, 1, 0, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(1, 0, 0, 1, 0, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(1, 0, 1, 0, 0, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(1, 0, 1, 1, 1, 0));
    assert(!DMABUF_GDR_NONCOHERENT_ALLOWED(1, 0, 1, 1, 0, 1));

    /* forcePcie=1, coherent=1, forceSpa=1 -> SPA */
    assert(DMABUF_GDR_USE_GRDMA_SPA(1, 1, 1));
    /* forcePcie=1, coherent=1, forceSpa=0 -> normal FB base */
    assert(!DMABUF_GDR_USE_GRDMA_SPA(1, 1, 0));
    /* forcePcie=1, coherent=0, forceSpa=1 -> normal FB base (regression case) */
    assert(!DMABUF_GDR_USE_GRDMA_SPA(1, 0, 1));
    /* forcePcie=1, coherent=0, forceSpa=0 -> normal FB base */
    assert(!DMABUF_GDR_USE_GRDMA_SPA(1, 0, 0));
    /* forcePcie=0 never selects SPA regardless of coherence/forceSpa */
    assert(!DMABUF_GDR_USE_GRDMA_SPA(0, 1, 1));
    assert(!DMABUF_GDR_USE_GRDMA_SPA(0, 0, 1));

    assert(DMABUF_GDR_RANGE_CONTAINED(windowStart, UINT64_C(0x1000),
                                        windowStart, windowSize));
    assert(DMABUF_GDR_RANGE_CONTAINED(windowEnd - UINT64_C(0x1000),
                                        UINT64_C(0x1000), windowStart, windowSize));
    assert(!DMABUF_GDR_RANGE_CONTAINED(windowEnd - UINT64_C(0x1000),
                                         UINT64_C(0x2000), windowStart, windowSize));
    assert(!DMABUF_GDR_RANGE_CONTAINED(windowEnd, UINT64_C(0x1000),
                                         windowStart, windowSize));
    assert(!DMABUF_GDR_RANGE_CONTAINED(windowStart - UINT64_C(0x1000),
                                         UINT64_C(0x1000), windowStart, windowSize));
    assert(!DMABUF_GDR_RANGE_CONTAINED(windowStart, emptySize,
                                         windowStart, windowSize));
    assert(!DMABUF_GDR_RANGE_CONTAINED(UINT64_MAX - UINT64_C(0x1000),
                                         UINT64_C(0x2000), windowStart, windowSize));

    return 0;
}
