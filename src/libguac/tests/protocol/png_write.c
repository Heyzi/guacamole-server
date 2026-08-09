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

#include <CUnit/CUnit.h>
#include <guacamole/parser.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/stream.h>

#include "encode-png.h"

#include <cairo/cairo.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Deliberately not equal and not a power of two, so a row/column swap or
 * off-by-one in row addressing produces a detectable mismatch. */
#define TEST_IMAGE_WIDTH 17
#define TEST_IMAGE_HEIGHT 13

/* Bounded well under 256 colors, so the palette path (not the >256-color
 * Cairo fallback) is what gets tested. */
static uint32_t test_pixel(int x, int y) {
    unsigned char value = (x * 3 + y * 11) % 200;
    return ((uint32_t) value << 16) | ((uint32_t) value << 8) | value;
}

/**
 * State for png_read_closure(): the reassembled PNG bytes and read offset.
 */
typedef struct png_read_state {
    const unsigned char* data;
    size_t length;
    size_t offset;
} png_read_state;

/**
 * cairo_read_func_t reading from a png_read_state.
 */
static cairo_status_t png_read_closure(void* closure, unsigned char* out,
        unsigned int length) {

    png_read_state* state = (png_read_state*) closure;

    if (state->offset + length > state->length)
        return CAIRO_STATUS_READ_ERROR;

    memcpy(out, state->data + state->offset, length);
    state->offset += length;

    return CAIRO_STATUS_SUCCESS;

}

/**
 * Writes a deterministic RGB24 test image to fd as guac_png_write() "blob"
 * instructions. Closes fd.
 */
static void write_png(int fd) {

    guac_socket* socket = guac_socket_open(fd);
    if (socket == NULL) {
        close(fd);
        return;
    }

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
            TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);

    int stride = cairo_image_surface_get_stride(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);

    for (int y = 0; y < TEST_IMAGE_HEIGHT; y++) {
        uint32_t* row = (uint32_t*) (data + y * stride);
        for (int x = 0; x < TEST_IMAGE_WIDTH; x++)
            row[x] = test_pixel(x, y);
    }

    cairo_surface_mark_dirty(surface);

    guac_stream stream;
    stream.index = 0;

    guac_png_write(socket, &stream, surface);

    cairo_surface_destroy(surface);
    guac_socket_free(socket);

}

/**
 * Reads the "blob" instructions written by write_png() from fd, decodes the
 * reassembled PNG, and asserts every pixel matches test_pixel(). Closes fd.
 */
static void read_and_verify_png(int fd) {

    guac_socket* socket = guac_socket_open(fd);
    CU_ASSERT_PTR_NOT_NULL_FATAL(socket);

    guac_parser* parser = guac_parser_alloc();
    CU_ASSERT_PTR_NOT_NULL_FATAL(parser);

    /* Reassemble the PNG from each "blob" instruction */
    unsigned char png_data[65536];
    size_t png_length = 0;

    while (guac_parser_read(parser, socket, 1000000) == 0
            && strcmp(parser->opcode, "blob") == 0) {

        CU_ASSERT_EQUAL_FATAL(parser->argc, 2);

        int decoded_length = guac_protocol_decode_base64(parser->argv[1]);
        CU_ASSERT_TRUE_FATAL(png_length + decoded_length <= sizeof(png_data));

        memcpy(png_data + png_length, parser->argv[1], decoded_length);
        png_length += decoded_length;

    }

    guac_parser_free(parser);
    guac_socket_free(socket);

    png_read_state state = { .data = png_data, .length = png_length };
    cairo_surface_t* decoded = cairo_image_surface_create_from_png_stream(
            png_read_closure, &state);

    CU_ASSERT_EQUAL_FATAL(cairo_surface_status(decoded), CAIRO_STATUS_SUCCESS);
    CU_ASSERT_EQUAL(cairo_image_surface_get_width(decoded), TEST_IMAGE_WIDTH);
    CU_ASSERT_EQUAL(cairo_image_surface_get_height(decoded), TEST_IMAGE_HEIGHT);

    int stride = cairo_image_surface_get_stride(decoded);
    unsigned char* data = cairo_image_surface_get_data(decoded);

    int mismatch = 0;
    for (int y = 0; y < TEST_IMAGE_HEIGHT && !mismatch; y++) {
        uint32_t* row = (uint32_t*) (data + y * stride);
        for (int x = 0; x < TEST_IMAGE_WIDTH; x++) {
            if ((row[x] & 0xFFFFFF) != test_pixel(x, y)) {
                mismatch = 1;
                break;
            }
        }
    }

    CU_ASSERT_EQUAL(mismatch, 0);

    cairo_surface_destroy(decoded);

}

/**
 * Verifies guac_png_write() round-trips an RGB24 surface losslessly,
 * exercising its row-by-row Cairo-to-PNG-row-buffer copy.
 */
void test_protocol__png_write() {

    int fd[2];

    CU_ASSERT_EQUAL_FATAL(pipe(fd), 0);

    int read_fd = fd[0];
    int write_fd = fd[1];

    int childpid;
    CU_ASSERT_NOT_EQUAL_FATAL((childpid = fork()), -1);

    if (childpid == 0) {
        close(read_fd);
        write_png(write_fd);
        exit(0);
    }

    close(write_fd);
    read_and_verify_png(read_fd);

}
