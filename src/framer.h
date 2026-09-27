/*
 * Copyright (c) 2026 Devyansh Agarwal
 * SPDX-License-Identifier: Apache-2.0
 *
 * Byte-stream framing parser for embedded RX paths.
 *
 * Frame format:
 *   [SOF=0x7E] [LEN] [PAYLOAD ...LEN bytes...] [CRC16_HI] [CRC16_LO]
 *
 * CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xor-out)
 * computed over LEN and PAYLOAD.
 *
 * Designed for an ISR-to-thread hand-off pattern: bytes arrive in an
 * interrupt, are pushed into an internal ring buffer, and a consumer
 * thread pulls decoded frames out via framer_get_frame().
 */

#ifndef FRAMER_H_
#define FRAMER_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#define FRAMER_SOF          0x7E
#define FRAMER_MAX_PAYLOAD  255U

/* Diagnostic counters exposed for shell / test inspection. */
struct framer_stats {
    uint32_t frames_ok;
    uint32_t frames_crc_err;
    uint32_t bytes_dropped;   /* bytes discarded during resync */
    uint32_t resyncs;         /* number of times parser resynced on garbage */
};

/* One decoded frame handed off to the consumer thread. */
struct framer_frame {
    uint8_t len;
    uint8_t data[FRAMER_MAX_PAYLOAD];
};

/*
 * framer_init - reset parser state and clear stats.
 * Call once before feeding bytes.
 */
void framer_init(void);

/*
 * framer_feed_byte - push one byte into the parser.
 *
 * On a completed valid frame, the frame is pushed onto the internal
 * message queue and can be retrieved with framer_get_frame().
 *
 * Safe to call from thread context. If called from an ISR, ensure the
 * downstream message queue submit is ISR-safe (k_msgq_put is).
 *
 * Returns 0 on success, negative errno on internal error.
 */
int framer_feed_byte(uint8_t b);

/*
 * framer_get_frame - block up to `timeout` for the next decoded frame.
 *
 * Returns 0 on success and fills *out; -EAGAIN on timeout.
 */
int framer_get_frame(struct framer_frame *out, k_timeout_t timeout);

/*
 * framer_get_stats - snapshot diagnostic counters.
 */
void framer_get_stats(struct framer_stats *out);

/*
 * framer_crc16_ccitt - exposed for producer-side frame construction and tests.
 */
uint16_t framer_crc16_ccitt(const uint8_t *data, size_t len);

#endif /* FRAMER_H_ */
