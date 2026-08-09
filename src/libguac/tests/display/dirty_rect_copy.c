/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "display-priv.h"

#include <CUnit/CUnit.h>
#include <guacamole/mem.h>
#include <guacamole/rect.h>

#include <string.h>

/**
 * Byte written throughout the "pending" buffer of each case prior to
 * copying. Any byte with this value after the copy was reached by the copy.
 */
#define PENDING_FILL 0xAB

/**
 * Byte written throughout the "last" buffer of each case prior to copying.
 * Any byte retaining this value after the copy was left untouched.
 */
#define LAST_FILL 0x55

/**
 * One test case: a pending/last buffer pair of a given size, plus the dirty
 * rect that display-flush.c's "geometry unchanged" branch of
 * PFW_LFW_guac_display_frame_complete() would use to copy between them.
 */
typedef struct dirty_copy_case {

    /**
     * Width/height shared by both the pending and last buffers, in pixels.
     */
    int width;
    int height;

    /**
     * Extra bytes appended to each row beyond width * 4, exercising a stride
     * that does not tightly pack pixel data.
     */
    size_t stride_padding;

    /**
     * The rectangle that should be copied from pending to last, equivalent
     * to pending_frame.dirty in the real code. Already assumed to be
     * constrained within the bounds of the buffer, as guaranteed by
     * guac_rect_constrain() within guac_display_plan_create() prior to
     * frame completion.
     */
    guac_rect dirty;

} dirty_copy_case;

/**
 * Runs one case: allocates a pending/last buffer pair, performs the same
 * bounded copy that display-flush.c's frame-complete branch performs, then
 * checks every single byte of the last buffer — copied where it should
 * have been, and left alone everywhere else, including the stride padding
 * that should never be touched at all.
 *
 * @param test
 *     The case to run.
 */
static void run_case(const dirty_copy_case* test) {

    size_t stride = guac_mem_ckd_mul_or_die(test->width, 4) + test->stride_padding;
    size_t size = guac_mem_ckd_mul_or_die(stride, test->height);

    guac_display_layer_state pending = { 0 };
    guac_display_layer_state last = { 0 };

    pending.buffer = guac_mem_alloc(size);
    last.buffer = guac_mem_alloc(size);
    pending.buffer_stride = stride;
    last.buffer_stride = stride;

    memset(pending.buffer, PENDING_FILL, size);
    memset(last.buffer, LAST_FILL, size);

    guac_rect dirty = test->dirty;

    const unsigned char* pending_frame = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(pending, dirty);
    unsigned char* last_frame = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(last, dirty);
    size_t row_length = guac_mem_ckd_mul_or_die(guac_rect_width(&dirty), 4);

    for (int y = dirty.top; y < dirty.bottom; y++) {
        memcpy(last_frame, pending_frame, row_length);
        last_frame += last.buffer_stride;
        pending_frame += pending.buffer_stride;
    }

    for (int y = 0; y < test->height; y++) {
        for (size_t col = 0; col < stride; col++) {

            int x = (int) (col / 4);
            int within_dirty = col < (size_t) test->width * 4
                    && y >= dirty.top && y < dirty.bottom
                    && x >= dirty.left && x < dirty.right;

            unsigned char expected = within_dirty ? PENDING_FILL : LAST_FILL;
            size_t offset = (size_t) y * stride + col;

            CU_ASSERT_EQUAL(expected, last.buffer[offset]);

        }
    }

    guac_mem_free(pending.buffer);
    guac_mem_free(last.buffer);

}

/**
 * Verifies that the bounded copy PFW_LFW_guac_display_frame_complete() uses
 * to update last_frame from pending_frame, when a layer's geometry hasn't
 * changed, copies pixels inside the dirty rect and leaves everything else
 * alone — including pixels just outside the rect and the stride padding —
 * across a range of dirty rect positions relative to the buffer bounds.
 */
void test_display__dirty_rect_copy() {

    static const dirty_copy_case cases[] = {

        /* Dirty rect entirely within the buffer interior */
        { 40, 30, 0, { .left = 10, .top = 8,  .right = 28, .bottom = 20 } },

        /* Dirty rect touching the right edge */
        { 40, 30, 0, { .left = 30, .top = 5,  .right = 40, .bottom = 15 } },

        /* Dirty rect touching the bottom edge */
        { 40, 30, 0, { .left = 5,  .top = 20, .right = 25, .bottom = 30 } },

        /* Dirty rect touching the bottom-right corner */
        { 40, 30, 0, { .left = 35, .top = 25, .right = 40, .bottom = 30 } },

        /* Single-pixel dirty rect */
        { 40, 30, 0, { .left = 17, .top = 9,  .right = 18, .bottom = 10 } },

        /* Full width, partial height */
        { 40, 30, 0, { .left = 0,  .top = 12, .right = 40, .bottom = 18 } },

        /* Buffer stride padded beyond width * 4 bytes */
        { 40, 30, 16, { .left = 5, .top = 5,  .right = 35, .bottom = 25 } },

        /* Dirty rect covering the entire layer */
        { 24, 20, 0, { .left = 0,  .top = 0,  .right = 24, .bottom = 20 } }

    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);

}
