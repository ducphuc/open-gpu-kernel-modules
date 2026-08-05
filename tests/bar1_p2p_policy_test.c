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

#include "../src/nvidia/src/kernel/gpu/bus/arch/turing/bar1_p2p_policy.h"

int main(void)
{
    const unsigned long long clientFbSize = 16ULL << 30;
    const unsigned long long partialStaticSize = 15ULL << 30;

    assert(!KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(0, clientFbSize,
                                                  partialStaticSize));
    assert(!KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(1, 0,
                                                  partialStaticSize));
    assert(!KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(1, clientFbSize, 0));

    assert(KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(1, clientFbSize,
                                                 partialStaticSize));
    assert(KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(1, clientFbSize,
                                                 clientFbSize));
    assert(KBUS_USE_DISPLAY_AWARE_STATIC_BAR1(1, clientFbSize,
                                                 clientFbSize + (1ULL << 30)));

    return 0;
}
