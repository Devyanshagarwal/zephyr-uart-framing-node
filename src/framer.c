/*
 * Copyright (c) 2026 Devyansh Agarwal
 * SPDX-License-Identifier: Apache-2.0
 *
 * Byte-at-a-time framing state machine with CRC-16/CCITT-FALSE validation
 * and automatic resync on corruption. Zephyr port of ringbuf-parser.
 */

#include "framer.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(framer, CONFIG_FRAMER_LOG_LEVEL);

/* Queue of decoded frames handed off to the consumer thread. */
K_MSGQ_DEFINE(framer_msgq, sizeof(struct framer_frame), 4, 4);

enum parser_state {
    STATE_WAIT_SOF,
    STATE_READ_LEN,
    STATE_READ_PAYLOAD,
    STATE_READ_CRC_HI,
    STATE_READ_CRC_LO,
};

static struct {
    enum parser_state state;
    uint8_t  len;
    uint8_t  payload_idx;
    uint8_t  payload[FRAMER_MAX_PAYLOAD];
    uint16_t crc_recv;
    struct framer_stats stats;
} parser;

/* CRC-16/CCITT-FALSE (poly=0x1021, init=0xFFFF, no reflection, no xor-out). */
uint16_t framer_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static void reset_to_wait_sof(bool count_as_resync)
{
    if (count_as_resync && parser.state != STATE_WAIT_SOF) {
        parser.stats.resyncs++;
    }
    parser.state = STATE_WAIT_SOF;
    parser.len = 0;
    parser.payload_idx = 0;
    parser.crc_recv = 0;
}

void framer_init(void)
{
    memset(&parser, 0, sizeof(parser));
    parser.state = STATE_WAIT_SOF;
    k_msgq_purge(&framer_msgq);
}

int framer_feed_byte(uint8_t b)
{
    switch (parser.state) {
    case STATE_WAIT_SOF:
        if (b == FRAMER_SOF) {
            parser.state = STATE_READ_LEN;
        } else {
            parser.stats.bytes_dropped++;
        }
        break;

    case STATE_READ_LEN:
        parser.len = b;
        parser.payload_idx = 0;
        if (parser.len == 0) {
            /* zero-length payload: skip straight to CRC */
            parser.state = STATE_READ_CRC_HI;
        } else {
            parser.state = STATE_READ_PAYLOAD;
        }
        break;

    case STATE_READ_PAYLOAD:
        parser.payload[parser.payload_idx++] = b;
        if (parser.payload_idx >= parser.len) {
            parser.state = STATE_READ_CRC_HI;
        }
        break;

    case STATE_READ_CRC_HI:
        parser.crc_recv = ((uint16_t)b) << 8;
        parser.state = STATE_READ_CRC_LO;
        break;

    case STATE_READ_CRC_LO: {
        parser.crc_recv |= b;

        /* Build the buffer that was CRC'd: LEN || PAYLOAD */
        uint8_t crc_input[1 + FRAMER_MAX_PAYLOAD];
        crc_input[0] = parser.len;
        memcpy(&crc_input[1], parser.payload, parser.len);
        uint16_t crc_calc = framer_crc16_ccitt(crc_input, 1U + parser.len);

        if (crc_calc == parser.crc_recv) {
            struct framer_frame f = {
                .len = parser.len,
            };
            memcpy(f.data, parser.payload, parser.len);

            int rc = k_msgq_put(&framer_msgq, &f, K_NO_WAIT);
            if (rc == 0) {
                parser.stats.frames_ok++;
            } else {
                /* consumer isn't draining fast enough; drop and count */
                parser.stats.bytes_dropped += parser.len;
                LOG_WRN("msgq full; dropping frame len=%u", parser.len);
            }
        } else {
            parser.stats.frames_crc_err++;
            LOG_DBG("CRC mismatch: got 0x%04x expected 0x%04x",
                    parser.crc_recv, crc_calc);
        }

        reset_to_wait_sof(false);
        break;
    }

    default:
        reset_to_wait_sof(true);
        return -EINVAL;
    }

    return 0;
}

int framer_get_frame(struct framer_frame *out, k_timeout_t timeout)
{
    if (out == NULL) {
        return -EINVAL;
    }
    return k_msgq_get(&framer_msgq, out, timeout);
}

void framer_get_stats(struct framer_stats *out)
{
    if (out == NULL) {
        return;
    }
    unsigned int key = irq_lock();
    *out = parser.stats;
    irq_unlock(key);
}
