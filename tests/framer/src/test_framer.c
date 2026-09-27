/*
 * Copyright (c) 2026 Devyansh Agarwal
 * SPDX-License-Identifier: Apache-2.0
 *
 * ztest suite for framer.c — mirrors the plain-assert coverage in the
 * upstream C11 ringbuf-parser project (wrap-around, partial-frame delivery,
 * garbage resync, CRC-error paths).
 */

#include "framer.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <string.h>

/* Build a well-formed frame into out[]; returns bytes written. */
static size_t make_frame(const uint8_t *payload, uint8_t len, uint8_t *out)
{
    uint8_t crc_input[1 + FRAMER_MAX_PAYLOAD];
    crc_input[0] = len;
    memcpy(&crc_input[1], payload, len);
    uint16_t crc = framer_crc16_ccitt(crc_input, 1U + len);

    size_t i = 0;
    out[i++] = FRAMER_SOF;
    out[i++] = len;
    memcpy(&out[i], payload, len);
    i += len;
    out[i++] = (uint8_t)(crc >> 8);
    out[i++] = (uint8_t)(crc & 0xFF);
    return i;
}

static void feed(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int rc = framer_feed_byte(buf[i]);
        zassert_equal(rc, 0, "feed rc=%d at i=%zu", rc, i);
    }
}

/* -------------------- tests -------------------- */

ZTEST(framer, test_wellformed_single_frame)
{
    framer_init();

    const uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t frame[16];
    size_t n = make_frame(payload, sizeof(payload), frame);

    feed(frame, n);

    struct framer_frame f;
    int rc = framer_get_frame(&f, K_MSEC(100));
    zassert_equal(rc, 0, "expected a decoded frame, rc=%d", rc);
    zassert_equal(f.len, sizeof(payload), "wrong len");
    zassert_mem_equal(f.data, payload, sizeof(payload), "payload mismatch");

    struct framer_stats st;
    framer_get_stats(&st);
    zassert_equal(st.frames_ok, 1U, "frames_ok mismatch");
    zassert_equal(st.frames_crc_err, 0U, "unexpected crc_err");
}

ZTEST(framer, test_partial_frame_delivery)
{
    framer_init();

    const uint8_t payload[] = { 'p', 'a', 'r', 't', 'i', 'a', 'l' };
    uint8_t frame[16];
    size_t n = make_frame(payload, sizeof(payload), frame);

    /* Feed one byte at a time, checking that the frame only appears
     * after the full sequence has been consumed. */
    for (size_t i = 0; i < n - 1; i++) {
        framer_feed_byte(frame[i]);
        struct framer_frame f;
        int rc = framer_get_frame(&f, K_NO_WAIT);
        zassert_equal(rc, -EAGAIN, "frame emitted too early at i=%zu (rc=%d)", i, rc);
    }
    framer_feed_byte(frame[n - 1]);

    struct framer_frame f;
    int rc = framer_get_frame(&f, K_MSEC(100));
    zassert_equal(rc, 0, "expected frame after final byte, rc=%d", rc);
    zassert_equal(f.len, sizeof(payload), "wrong len");
}

ZTEST(framer, test_garbage_resync)
{
    framer_init();

    const uint8_t garbage[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    const uint8_t payload[]  = { 'g', 'o', 'o', 'd' };
    uint8_t frame[16];
    size_t n = make_frame(payload, sizeof(payload), frame);

    feed(garbage, sizeof(garbage));
    feed(frame, n);

    struct framer_frame f;
    int rc = framer_get_frame(&f, K_MSEC(100));
    zassert_equal(rc, 0, "expected frame after garbage, rc=%d", rc);
    zassert_equal(f.len, sizeof(payload), "wrong len");
    zassert_mem_equal(f.data, payload, sizeof(payload), "payload mismatch");

    struct framer_stats st;
    framer_get_stats(&st);
    zassert_equal(st.frames_ok, 1U, "frames_ok mismatch");
    zassert_true(st.bytes_dropped >= sizeof(garbage),
                 "expected dropped >= %zu got %u",
                 sizeof(garbage), st.bytes_dropped);
}

ZTEST(framer, test_crc_error_path)
{
    framer_init();

    const uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
    uint8_t frame[16];
    size_t n = make_frame(payload, sizeof(payload), frame);

    /* Corrupt the CRC by flipping the last byte's low bits */
    frame[n - 1] ^= 0xFF;
    feed(frame, n);

    struct framer_frame f;
    int rc = framer_get_frame(&f, K_MSEC(50));
    zassert_equal(rc, -EAGAIN, "no frame should be emitted, rc=%d", rc);

    struct framer_stats st;
    framer_get_stats(&st);
    zassert_equal(st.frames_ok, 0U, "unexpected frames_ok");
    zassert_equal(st.frames_crc_err, 1U, "expected 1 crc err, got %u",
                  st.frames_crc_err);

    /* And that the parser recovered: a well-formed frame after it should decode. */
    n = make_frame(payload, sizeof(payload), frame);
    feed(frame, n);
    rc = framer_get_frame(&f, K_MSEC(100));
    zassert_equal(rc, 0, "parser did not recover after CRC error, rc=%d", rc);
}

ZTEST(framer, test_back_to_back_frames)
{
    framer_init();

    uint8_t frame[16];
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t payload[] = { i, (uint8_t)(i + 1), (uint8_t)(i + 2) };
        size_t n = make_frame(payload, sizeof(payload), frame);
        feed(frame, n);

        struct framer_frame f;
        int rc = framer_get_frame(&f, K_MSEC(50));
        zassert_equal(rc, 0, "frame %u not decoded, rc=%d", i, rc);
        zassert_equal(f.data[0], i, "frame %u payload[0] mismatch", i);
    }
}

ZTEST(framer, test_crc16_ccitt_known_vector)
{
    /* Standard test vector: CRC-16/CCITT-FALSE of "123456789" is 0x29B1 */
    const uint8_t v[] = "123456789";
    uint16_t crc = framer_crc16_ccitt(v, 9);
    zassert_equal(crc, 0x29B1, "CRC vector mismatch: 0x%04x", crc);
}

ZTEST_SUITE(framer, NULL, NULL, NULL, NULL, NULL);
