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

#ifndef GUAC_BENCHMARKS_BENCH_STATS_H
#define GUAC_BENCHMARKS_BENCH_STATS_H

/*
 * Small, dependency-free grab bag shared by every benchmark under this
 * directory: a monotonic clock, median/MAD summary stats, and --runs
 * argument parsing. Every benchmark so far (and every one still to come)
 * needed its own copy of this, so it lives here once instead. Nothing in
 * here knows anything about what's actually being timed — that part stays
 * in each benchmark, since it's genuinely different from one benchmark to
 * the next.
 *
 * Everything below is "static inline" on purpose: this header gets
 * included into exactly one .c file per benchmark binary, and a benchmark
 * is free to use only some of these helpers. Plain "static" would make
 * -Wunused-function complain (and this tree builds benchmarks with
 * -Werror) about whichever ones it skips.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * Hints to the compiler that a function should never be inlined. Applied to
 * the actual code paths under comparison in a benchmark, so that inlining
 * and constant-folding can't quietly make one side of the comparison
 * cheaper than it would be at its real call site.
 */
#if defined(__GNUC__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

/**
 * The default number of timed samples a benchmark collects for each case,
 * unless the user overrides it with --runs.
 */
#define BENCH_DEFAULT_RUNS 15

/**
 * The fewest samples a benchmark will accept via --runs. Below this, the
 * median/MAD numbers stop meaning much.
 */
#define BENCH_MIN_RUNS 5

/**
 * Returns the current time in nanoseconds, as read from a monotonic clock
 * (so NTP adjustments, DST changes, etc. can't distort a measurement).
 * Exits the process on failure, since a benchmark with a broken clock has
 * nothing to report anyway.
 *
 * @return
 *     The current time, in nanoseconds, according to CLOCK_MONOTONIC.
 */
static inline uint64_t bench_now_ns(void) {

    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time)) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (uint64_t) time.tv_sec * UINT64_C(1000000000)
            + (uint64_t) time.tv_nsec;

}

/**
 * qsort() comparator for an array of doubles, ascending.
 */
static inline int bench_compare_double(const void* a, const void* b) {
    double first = *(const double*) a;
    double second = *(const double*) b;
    return (first > second) - (first < second);
}

/**
 * Sorts the given samples in place and returns their median. Using the
 * median (rather than the mean) keeps one stray slow sample — a context
 * switch, a page fault, whatever the OS felt like doing that millisecond —
 * from dragging the reported number around.
 *
 * @param values
 *     The samples to sort and summarize. Reordered in place.
 *
 * @param count
 *     The number of samples in values.
 *
 * @return
 *     The median of values.
 */
static inline double bench_median(double* values, int count) {
    qsort(values, (size_t) count, sizeof(*values), bench_compare_double);
    if (count % 2)
        return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

/**
 * Returns the median absolute deviation of the given samples around the
 * given center, as a robust stand-in for a standard deviation — one that
 * doesn't get thrown off by the same occasional slow outlier that medians
 * are already immune to.
 *
 * @param values
 *     The samples to summarize. NOT reordered (unlike bench_median()).
 *
 * @param count
 *     The number of samples in values.
 *
 * @param center
 *     The value deviations should be measured from, ordinarily the result
 *     of calling bench_median() on the same samples.
 *
 * @return
 *     The median absolute deviation of values around center.
 */
static inline double bench_median_absolute_deviation(const double* values,
        int count, double center) {

    double* deviations = malloc((size_t) count * sizeof(*deviations));
    double result;
    int i;

    if (!deviations) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    for (i = 0; i < count; i++) {
        double difference = values[i] - center;
        deviations[i] = difference < 0 ? -difference : difference;
    }

    result = bench_median(deviations, count);
    free(deviations);
    return result;

}

/**
 * Parses the optional "--runs N" argument accepted by every benchmark in
 * this directory, falling back to default_runs if the argument was
 * omitted entirely. Prints a usage message and exits on anything else —
 * malformed input, or a count outside [min_runs, 1000].
 *
 * @param argc
 *     The argument count passed to main().
 *
 * @param argv
 *     The argument vector passed to main().
 *
 * @param default_runs
 *     The number of runs to use if --runs was not given.
 *
 * @param min_runs
 *     The smallest run count this benchmark is willing to accept.
 *
 * @return
 *     The number of runs the benchmark should perform.
 */
static inline int bench_parse_runs(int argc, char** argv, int default_runs,
        int min_runs) {

    char* end;
    long runs;

    if (argc == 1)
        return default_runs;

    if (argc != 3 || strcmp(argv[1], "--runs")) {
        fprintf(stderr, "Usage: %s [--runs N]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    errno = 0;
    runs = strtol(argv[2], &end, 10);
    if (errno || *end || runs < min_runs || runs > 1000) {
        fprintf(stderr, "Runs must be between %d and 1000.\n", min_runs);
        exit(EXIT_FAILURE);
    }

    return (int) runs;

}

#endif
