/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Compares the two ways display-flush.c can bring last_frame back in sync
 * with pending_frame once a layer's geometry stops changing: copy the whole
 * buffer every time (what the code used to do), or copy only the dirty rect
 * (what it does now). Every case below is named after whatever in a real
 * Guacamole session would actually produce that dirty rect, rather than
 * being picked to look good on a chart.
 */

#include "bench-stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPP 4

typedef struct rect {
    int left, top, right, bottom;
} rect;

typedef struct benchmark_case {

    /** Label shown in the results table. */
    const char* name;

    /** Section heading printed above this case, if it starts a new group. */
    const char* section;

    /** Dimensions of the (simulated) layer buffer, in pixels. */
    int buffer_width;
    int buffer_height;

    /**
     * Extra bytes appended to each row beyond buffer_width * BPP. Real
     * layers can end up with a buffer_stride wider than buffer_width * 4 —
     * a layer that shrank without being reallocated keeps the old, larger
     * buffer around. Neither copy strategy should ever touch this padding,
     * and it shouldn't change either one's cost.
     */
    int stride_padding;

    /** The single bounding rect to copy from pending to last. */
    rect dirty;

} benchmark_case;

typedef uint64_t (*copy_fn)(unsigned char*, const unsigned char*, size_t,
        int, int, const rect*);

static volatile uint64_t checksum_sink;

/**
 * Hashes the bytes of the given buffer that fall within the given rect, at
 * the given row stride. Used to check that copy_full() and copy_dirty()
 * below leave the exact same data behind within the dirty rect, no matter
 * what either one does to the rest of the buffer.
 */
static uint64_t checksum_region(const unsigned char* buffer, size_t stride,
        const rect* dirty) {

    uint64_t checksum = 0;
    int row_bytes = (dirty->right - dirty->left) * BPP;

    for (int y = dirty->top; y < dirty->bottom; y++) {
        const unsigned char* row = buffer + (size_t) y * stride
                + (size_t) dirty->left * BPP;

        for (int i = 0; i < row_bytes; i++)
            checksum = checksum * 31 + row[i];
    }

    return checksum;
}

/**
 * How the "geometry unchanged" branch of frame_complete() used to behave:
 * copy the entire buffer from pending to last on any change at all, no
 * matter how small the actual dirty rect was. Cost is always
 * buffer_width * buffer_height, regardless of what dirty describes.
 */
static BENCH_NOINLINE uint64_t copy_full(unsigned char* last,
        const unsigned char* pending, size_t stride, int buffer_width,
        int buffer_height, const rect* dirty) {

    unsigned char* last_row = last;
    const unsigned char* pending_row = pending;
    size_t row_length = (size_t) buffer_width * BPP;

    for (int y = 0; y < buffer_height; y++) {
        memcpy(last_row, pending_row, row_length);
        last_row += stride;
        pending_row += stride;
    }

    return checksum_region(last, stride, dirty);
}

/**
 * How it behaves now: only the dirty rect gets copied. Cost tracks
 * dirty_width * dirty_height, plus one memcpy() call per dirty row — so a
 * tall, narrow rect (lots of short rows) pays more call overhead than a
 * short, wide one covering the same area.
 */
static BENCH_NOINLINE uint64_t copy_dirty(unsigned char* last,
        const unsigned char* pending, size_t stride, int buffer_width,
        int buffer_height, const rect* dirty) {

    (void) buffer_width;
    (void) buffer_height;

    unsigned char* last_row = last + (size_t) dirty->top * stride
            + (size_t) dirty->left * BPP;
    const unsigned char* pending_row = pending + (size_t) dirty->top * stride
            + (size_t) dirty->left * BPP;
    size_t row_length = (size_t) (dirty->right - dirty->left) * BPP;

    for (int y = dirty->top; y < dirty->bottom; y++) {
        memcpy(last_row, pending_row, row_length);
        last_row += stride;
        pending_row += stride;
    }

    return checksum_region(last, stride, dirty);
}

typedef struct measurement {
    uint64_t elapsed_ns;
    uint64_t checksum;
} measurement;

static measurement measure(copy_fn copy, unsigned char* last,
        const unsigned char* pending, size_t stride, int buffer_width,
        int buffer_height, const rect* dirty, size_t iterations) {

    measurement result = { 0, 0 };
    uint64_t start = bench_now_ns();
    size_t iteration;

    for (iteration = 0; iteration < iterations; iteration++) {
        uint64_t checksum = copy(last, pending, stride, buffer_width,
                buffer_height, dirty);
        result.checksum ^= checksum + UINT64_C(0x9E3779B97F4A7C15)
                + (result.checksum << 6) + (result.checksum >> 2);
    }

    result.elapsed_ns = bench_now_ns() - start;
    checksum_sink ^= result.checksum;
    return result;
}

/**
 * Picks an iteration count large enough that one call to measure() (using
 * copy_full(), always the slower of the two) takes at least ~100ms, so the
 * clock's own resolution and any one-off scheduling hiccup stay a small
 * fraction of what's being measured.
 */
static size_t calibrate(unsigned char* last, const unsigned char* pending,
        size_t stride, int buffer_width, int buffer_height,
        const rect* dirty) {

    size_t iterations = 1;

    for (;;) {
        measurement result = measure(copy_full, last, pending, stride,
                buffer_width, buffer_height, dirty, iterations);

        if (result.elapsed_ns >= UINT64_C(100000000))
            return iterations;

        if (!result.elapsed_ns)
            iterations *= 10;
        else {
            uint64_t factor = UINT64_C(100000000) / result.elapsed_ns + 1;
            if (factor > 10)
                factor = 10;
            iterations *= (size_t) factor;
        }
    }
}

static void run_case(const benchmark_case* benchmark, int runs) {

    size_t stride = (size_t) benchmark->buffer_width * BPP
            + (size_t) benchmark->stride_padding;
    size_t size = stride * (size_t) benchmark->buffer_height;

    /* Pending and last start out with different, fixed fill bytes rather
     * than anything derived from buffer size or position. That way the two
     * buffers can never coincidentally start out equal, which would let a
     * copy that does nothing at all still pass the checksum check below. */
    unsigned char* pending = malloc(size);
    unsigned char* last = malloc(size);
    double* old_samples = malloc((size_t) runs * sizeof(*old_samples));
    double* new_samples = malloc((size_t) runs * sizeof(*new_samples));
    double old_median, new_median, old_mad, new_mad;
    double dirty_area, buffer_area;
    size_t iterations;
    int run;

    if (!pending || !last || !old_samples || !new_samples) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    memset(pending, 0xAB, size);
    memset(last, 0x55, size);

    iterations = calibrate(last, pending, stride, benchmark->buffer_width,
            benchmark->buffer_height, &benchmark->dirty);

    for (run = 0; run < runs; run++) {
        measurement old_result;
        measurement new_result;

        /* Alternate which strategy goes first each run, so a steady drift
         * in the machine's mood (thermal throttling, frequency scaling
         * kicking in partway through) lands on both sides about equally
         * instead of quietly favoring whichever ran second every time. */
        if (run % 2) {
            new_result = measure(copy_dirty, last, pending, stride,
                    benchmark->buffer_width, benchmark->buffer_height,
                    &benchmark->dirty, iterations);
            old_result = measure(copy_full, last, pending, stride,
                    benchmark->buffer_width, benchmark->buffer_height,
                    &benchmark->dirty, iterations);
        }
        else {
            old_result = measure(copy_full, last, pending, stride,
                    benchmark->buffer_width, benchmark->buffer_height,
                    &benchmark->dirty, iterations);
            new_result = measure(copy_dirty, last, pending, stride,
                    benchmark->buffer_width, benchmark->buffer_height,
                    &benchmark->dirty, iterations);
        }

        if (old_result.checksum != new_result.checksum) {
            fprintf(stderr, "%s: checksum mismatch\n", benchmark->name);
            exit(EXIT_FAILURE);
        }

        old_samples[run] = old_result.elapsed_ns / (double) iterations;
        new_samples[run] = new_result.elapsed_ns / (double) iterations;
    }

    old_median = bench_median(old_samples, runs);
    new_median = bench_median(new_samples, runs);
    old_mad = bench_median_absolute_deviation(old_samples, runs, old_median);
    new_mad = bench_median_absolute_deviation(new_samples, runs, new_median);

    dirty_area = (double) (benchmark->dirty.right - benchmark->dirty.left)
            * (double) (benchmark->dirty.bottom - benchmark->dirty.top);
    buffer_area = (double) benchmark->buffer_width
            * (double) benchmark->buffer_height;

    printf("%-16s %5.1f%% %12.1f +/- %-9.1f %12.2f +/- %-8.2f %+9.1fx\n",
            benchmark->name, 100.0 * dirty_area / buffer_area,
            old_median, old_mad, new_median, new_mad,
            old_median / new_median);

    free(new_samples);
    free(old_samples);
    free(last);
    free(pending);
}

int main(int argc, char** argv) {
    static const benchmark_case cases[] = {

        /* Each rect below is named and sized after whatever would actually
         * produce it in a live Guacamole session, on a 1920x1080 desktop —
         * not picked as an abstract geometric probe. They run smallest and
         * most frequent first (a blinking or just-typed terminal glyph,
         * which can happen many times a second) through to the largest and
         * rarest (a full-screen redraw from switching applications, or
         * resizing the client window). */
        { "glyph cell (blink/keystroke)", "Real interaction scenarios (1920x1080 desktop)",
          1920, 1080, 0, {  700,  500,  710,  520 } },
        { "scrollbar/panel drag (16px)", NULL, 1920, 1080, 0, {  950,    0,  966, 1080 } },
        { "button/icon redraw",   NULL, 1920, 1080, 0, {  400,  300,  520,  336 } },
        { "notification popup",  NULL, 1920, 1080, 0, {  600,  200,  920,  320 } },
        { "terminal line append", NULL, 1920, 1080, 0, {    0,  530, 1920,  550 } },
        { "video/thumbnail tile", NULL, 1920, 1080, 0, {  200,  200,  840,  560 } },
        { "window drag (bbox of move)", NULL, 1920, 1080, 0, {  100,  100,  800,  600 } },
        { "page/document scroll", NULL, 1920, 1080, 0, {    0,   90, 1920,  990 } },
        { "full-screen redraw",  NULL, 1920, 1080, 0, {    0,    0, 1920, 1080 } },

        /* Same terminal-line-append rect, but the buffer is allocated wider
         * than it's currently being used for — see the stride_padding doc
         * comment above. This should land on effectively the same numbers
         * as the unpadded terminal line case above; if it doesn't, that's a
         * sign padding bytes are being touched (or walked over) somewhere
         * they shouldn't be. */
        { "terminal line, +512B stride", "Padded stride (buffer wider than currently used)",
          1920, 1080, 512, {    0,  530, 1920,  550 } },

        /* The same terminal-line-append update — a common, frequent update
         * in any SSH/telnet session — replayed across the spread of
         * resolutions people actually connect at, from a small SD window up
         * through a 4K display. copy_full's cost tracks the whole buffer,
         * so it keeps climbing here; copy_dirty's cost only tracks the
         * dirty rect, which doesn't change, so the win widens with
         * resolution. */
        { "terminal line @ 640x480 (SD)",   "Same update across client resolutions",
          640,  480,  0, {    0,  230,  640,  250 } },
        { "terminal line @ 1920x1080 (FHD)", NULL, 1920, 1080, 0, {    0,  530, 1920,  550 } },
        { "terminal line @ 3440x1440 (ultrawide)", NULL, 3440, 1440, 0, {    0,  715, 3440,  735 } },
        { "terminal line @ 3840x2160 (4K)",  NULL, 3840, 2160, 0, {    0, 1070, 3840, 1090 } }

    };
    int runs = bench_parse_runs(argc, argv, BENCH_DEFAULT_RUNS, BENCH_MIN_RUNS);
    size_t i;

    printf("runs=%d, sample>=100ms, speedup=old/new\n", runs);
    printf("%-16s %6s %23s %22s %10s\n",
            "dirty rect", "area%", "old ns/call +/- MAD", "new ns/call +/- MAD",
            "speedup");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].section != NULL)
            printf("-- %s --\n", cases[i].section);
        run_case(&cases[i], runs);
    }

    return checksum_sink == UINT64_MAX ? EXIT_FAILURE : EXIT_SUCCESS;
}
