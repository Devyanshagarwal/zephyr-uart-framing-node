/*
 * Copyright (c) 2026 Devyansh Agarwal
 * SPDX-License-Identifier: Apache-2.0
 *
 * Demo application: a producer thread synthesizes framed byte streams
 * (well-formed, then a corrupted frame, then a well-formed one after
 * garbage), a "RX" thread feeds bytes into the framer one at a time
 * (modelling an ISR pushing bytes as they arrive), and a consumer
 * thread pulls decoded frames off the queue and logs them.
 *
 * On real hardware, the producer thread is replaced by a UART ISR
 * calling framer_feed_byte() on each received byte. The framer API
 * does not change.
 */

#include "framer.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define STACK_SIZE 2048
#define PROD_PRIO   5
#define CONS_PRIO   4

static K_THREAD_STACK_DEFINE(prod_stack, STACK_SIZE);
static K_THREAD_STACK_DEFINE(cons_stack, STACK_SIZE);
static struct k_thread prod_thread_data;
static struct k_thread cons_thread_data;

/* Byte queue that models the ISR-to-thread hand-off. In real firmware
 * the UART ISR would push into this and the RX task would drain it. */
K_MSGQ_DEFINE(rx_bytes, sizeof(uint8_t), 512, 1);

/*
 * Build a framed packet into `out` and return its length.
 *   [SOF][LEN][PAYLOAD][CRC16_HI][CRC16_LO]
 */
static size_t build_frame(const uint8_t *payload, uint8_t len, uint8_t *out)
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

static void push_bytes(const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        (void)k_msgq_put(&rx_bytes, &bytes[i], K_FOREVER);
    }
}

static void producer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    uint8_t buf[FRAMER_MAX_PAYLOAD + 8];

    /* Frame A: well-formed */
    const uint8_t payload_a[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t na = build_frame(payload_a, sizeof(payload_a), buf);
    push_bytes(buf, na);
    k_sleep(K_MSEC(50));

    /* Frame B: garbage prefix + well-formed frame (resync test) */
    const uint8_t garbage[] = { 0x00, 0x11, 0x22, 0x33 };
    push_bytes(garbage, sizeof(garbage));
    const uint8_t payload_b[] = { 'H', 'E', 'L', 'L', 'O' };
    size_t nb = build_frame(payload_b, sizeof(payload_b), buf);
    push_bytes(buf, nb);
    k_sleep(K_MSEC(50));

    /* Frame C: corrupt CRC on purpose */
    const uint8_t payload_c[] = { 0x01, 0x02, 0x03 };
    size_t nc = build_frame(payload_c, sizeof(payload_c), buf);
    buf[nc - 1] ^= 0xFF; /* flip CRC LSB */
    push_bytes(buf, nc);
    k_sleep(K_MSEC(50));

    /* Frame D: well-formed, proves recovery after CRC error */
    const uint8_t payload_d[] = { 'W', 'O', 'R', 'L', 'D', '!' };
    size_t nd = build_frame(payload_d, sizeof(payload_d), buf);
    push_bytes(buf, nd);
}

/* This thread models the RX-side dispatcher that would run in
 * a real UART ISR: pull one byte, hand it to the framer, repeat. */
static void consumer_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    /* Byte-shovel loop */
    k_tid_t self = k_current_get();
    ARG_UNUSED(self);

    /* Two sub-loops: byte feeder + frame collector, cooperatively via yielding */
    while (1) {
        uint8_t byte;
        int rc = k_msgq_get(&rx_bytes, &byte, K_MSEC(10));
        if (rc == 0) {
            framer_feed_byte(byte);
        }

        struct framer_frame f;
        while (framer_get_frame(&f, K_NO_WAIT) == 0) {
            LOG_INF("RX frame len=%u first=0x%02x last=0x%02x",
                    f.len, f.data[0], f.data[f.len - 1]);
        }

        struct framer_stats st;
        framer_get_stats(&st);
        static uint32_t last_ok;
        if (st.frames_ok != last_ok) {
            last_ok = st.frames_ok;
            LOG_INF("stats: ok=%u crc_err=%u resyncs=%u dropped=%u",
                    st.frames_ok, st.frames_crc_err,
                    st.resyncs, st.bytes_dropped);
        }
    }
}

int main(void)
{
    LOG_INF("zephyr-uart-framing-node starting");
    framer_init();

    k_thread_create(&cons_thread_data, cons_stack, STACK_SIZE,
                    consumer_thread, NULL, NULL, NULL,
                    CONS_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&cons_thread_data, "framer_consumer");

    k_thread_create(&prod_thread_data, prod_stack, STACK_SIZE,
                    producer_thread, NULL, NULL, NULL,
                    PROD_PRIO, 0, K_MSEC(100));
    k_thread_name_set(&prod_thread_data, "demo_producer");

    return 0;
}
