/*
 * MH2P RCC-side RSTP peer
 *
 * The MMX application processor talks to the RCC (radio/car controller)
 * over a 1 Mbit/s UART carrying the "RSTP" multiplexing protocol.  On the
 * real unit the RCC M4 receives hard keys and touch-panel events over the
 * vehicle buses and forwards them on the RSTP keypanel channel, where the
 * original hidcontroller consumes them.  This chardev plays the RCC side of
 * that link so the unmodified MMX firmware receives native pointer and
 * keyboard input from any QEMU display.  Explicit vehicle clamp and main
 * power-state inputs are delivered to the original OOC on PWRMAN.
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/fifo8.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "chardev/char.h"
#include "ui/console.h"
#include "ui/input.h"
#include "system/reset.h"
#include "qom/object.h"
#include "trace.h"

#define RSTP_START          0x7e
#define RSTP_ESCAPE         0x7d
#define RSTP_ESCAPE_XOR     0x20

#define RSTP_FLAG_INIT      0x00
#define RSTP_FLAG_INIT_ACK  0x01
#define RSTP_FLAG_CHRST     0x02
#define RSTP_FLAG_DATA      0x10
#define RSTP_FLAG_ACK       0x11
#define RSTP_FLAG_NACK      0x12
#define RSTP_FLAG_SDATA     0x14
#define RSTP_FLAG_DATAN     0x20
#define RSTP_FLAG_DATANC    0x21

#define RSTP_MAX_DATA       32
#define RSTP_MAX_FRAME      (2 + 2 + RSTP_MAX_DATA + 2)
#define RSTP_SQN_WINDOW     16

#define RSTP_CHN_KEYPANEL   0x40
#define RSTP_CHN_PWRMAN     0x10
#define RSTP_CHN_WATCHDOG   0xff
#define RSTP_WATCHDOG_NS    (1000 * SCALE_MS)

#define RSTP_TX_DEPTH       64
#define RSTP_TX_WINDOW      8
#define RSTP_RETRANSMIT_NS  (200 * SCALE_MS)
#define RSTP_INIT_RETRY_NS  (100 * SCALE_MS)
#define RSTP_NACK_HOLDOFF_NS (20 * SCALE_MS)
#define RSTP_OUTBUF_SIZE    8192

/* Keypanel function IDs consumed by hidcontroller. */
#define KP_FID_KEY2             0x00
#define KP_FID_ENCODER2         0x01
#define KP_FID_INPUT_PANEL_RDY  0x0d
#define KP_FID_TOUCH_COORD      0x0f

/* org.dsi.ifc.keypanel.Constants */
#define KBD_FCC                 1
#define KBD_MFW                 4
#define KBD_TOUCHSCREEN_FRONT   13
#define KBD_VOLUME_CONTROL      17
#define KST_RELEASED            0
#define KST_PRESSED             1
#define KEY_VOL                 17

/* Original P2865 hidcontroller.json scales these to 1920x720. */
#define TOUCH_RAW_WIDTH         1024
#define TOUCH_RAW_HEIGHT        540
#define TOUCH_MOVE_INTERVAL_MS  16
/* A single RSTP frame holds a six-byte header and four six-byte updates. */
#define TOUCH_MAX_CONTACTS      ((RSTP_MAX_DATA - 6) / 6)

#define TYPE_CHARDEV_MH2P_RSTP "chardev-mh2p-rstp"
OBJECT_DECLARE_SIMPLE_TYPE(MH2PRstpChardev, CHARDEV_MH2P_RSTP)

typedef struct RstpTxEntry {
    uint8_t data[RSTP_MAX_DATA];
    uint8_t len;
    uint8_t sqn;
    bool sent;
} RstpTxEntry;

typedef struct RstpTxChannel {
    uint8_t chn;
    uint8_t next_sqn;
    unsigned head;
    unsigned count;
    int64_t sent_ns;
    RstpTxEntry queue[RSTP_TX_DEPTH];
} RstpTxChannel;

typedef struct MH2PKeyMap {
    QKeyCode qcode;
    uint8_t kbd;
    uint8_t key;
} MH2PKeyMap;

typedef struct RstpTouchSlot {
    bool active;
    bool released;
    int64_t tracking_id;
    int x;
    int y;
} RstpTouchSlot;

struct MH2PRstpChardev {
    Chardev parent;

    QemuInputHandlerState *hs;
    Fifo8 outbuf;
    QEMUTimer *timer;
    QEMUTimer *watchdog_timer;

    /* Receiver */
    uint8_t rx[RSTP_MAX_FRAME];
    unsigned rx_len;
    bool rx_active;
    bool rx_escape;
    uint8_t rx_last_sqn[256];

    /* Link state: INIT is repeated until the MMX answers with INIT_ACK. */
    bool link_up;
    int64_t init_sent_ns;
    bool panels_ready;

    RstpTxChannel keypanel;
    RstpTxChannel pwrman;
    bool pwrman_seen;
    int clamp_state;
    int power_state;

    /* runmode-mgr answers each watchdog counter with counter + 1. */
    uint32_t watchdog_counter;

    /* Pointer state */
    int abs_x;
    int abs_y;
    QemuConsole *abs_con;
    bool touch_down;
    bool touch_moved;
    int64_t touch_last_ms;
    int last_x;
    int last_y;
    int wheel;
    RstpTouchSlot contacts[TOUCH_MAX_CONTACTS];
    bool multitouch;
    bool multitouch_dirty;
    DECLARE_BITMAP(keys_down, Q_KEY_CODE__MAX);
};

static const MH2PKeyMap mh2p_rstp_keymap[] = {
    { Q_KEY_CODE_HOME,      KBD_FCC, 116 },  /* KEY_HOME */
    { Q_KEY_CODE_ESC,       KBD_FCC, 13 },   /* KEY_BACK */
    { Q_KEY_CODE_BACKSPACE, KBD_FCC, 13 },   /* KEY_BACK */
    { Q_KEY_CODE_F1,        KBD_FCC, 1 },    /* KEY_MED */
    { Q_KEY_CODE_F2,        KBD_FCC, 3 },    /* KEY_TEL */
    { Q_KEY_CODE_F3,        KBD_FCC, 4 },    /* KEY_NAV */
    { Q_KEY_CODE_F4,        KBD_FCC, 6 },    /* KEY_CAR */
    { Q_KEY_CODE_F5,        KBD_FCC, 7 },    /* KEY_SET */
    { Q_KEY_CODE_F6,        KBD_FCC, 78 },   /* KEY_MENU */
    { Q_KEY_CODE_F7,        KBD_FCC, 79 },   /* KEY_CLIMATE */
    { Q_KEY_CODE_F8,        KBD_FCC, 103 },  /* KEY_SOURCE */
    { Q_KEY_CODE_F9,        KBD_FCC, 104 },  /* KEY_MAP */
    { Q_KEY_CODE_F10,       KBD_FCC, 106 },  /* KEY_OPTION */
    { Q_KEY_CODE_LEFT,      KBD_FCC, 10 },   /* KEY_PREV */
    { Q_KEY_CODE_RIGHT,     KBD_FCC, 14 },   /* KEY_NEXT */
    { Q_KEY_CODE_UP,        KBD_MFW, 38 },   /* KEY_MFW_UP */
    { Q_KEY_CODE_DOWN,      KBD_MFW, 39 },   /* KEY_MFW_DOWN */
    { Q_KEY_CODE_PGUP,      KBD_MFW, 42 },   /* KEY_MFW_VOLUME_UP */
    { Q_KEY_CODE_PGDN,      KBD_MFW, 43 },   /* KEY_MFW_VOLUME_DOWN */
    { Q_KEY_CODE_EQUAL,     KBD_MFW, 42 },
    { Q_KEY_CODE_MINUS,     KBD_MFW, 43 },
    { Q_KEY_CODE_F11,       KBD_MFW, 57 },   /* KEY_MFW_MUTE */
    { Q_KEY_CODE_AUDIOMUTE, KBD_MFW, 57 },
};

static uint16_t rstp_crc16(const uint8_t *buf, unsigned len)
{
    uint16_t crc = 0xffff;

    for (unsigned i = 0; i < len; i++) {
        crc ^= buf[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void rstp_flush(MH2PRstpChardev *s)
{
    Chardev *chr = CHARDEV(s);

    while (!fifo8_is_empty(&s->outbuf)) {
        int avail = qemu_chr_be_can_write(chr);
        uint32_t len;
        const uint8_t *buf;

        if (avail <= 0) {
            trace_mh2p_rstp_uart_blocked(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                fifo8_num_used(&s->outbuf));
            return;
        }
        buf = fifo8_pop_bufptr(&s->outbuf,
                               MIN((uint32_t)avail,
                                   fifo8_num_used(&s->outbuf)), &len);
        trace_mh2p_rstp_uart_write(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  len, fifo8_num_used(&s->outbuf));
        qemu_chr_be_write(chr, buf, len);
    }
}

static void rstp_chr_accept_input(Chardev *chr)
{
    rstp_flush(CHARDEV_MH2P_RSTP(chr));
}

static void rstp_put_escaped(uint8_t *out, unsigned *pos, uint8_t byte)
{
    if (byte == RSTP_START || byte == RSTP_ESCAPE) {
        out[(*pos)++] = RSTP_ESCAPE;
        byte ^= RSTP_ESCAPE_XOR;
    }
    out[(*pos)++] = byte;
}

/* @body is the unescaped frame after the start byte, without CRC. */
static void rstp_send_raw(MH2PRstpChardev *s, const uint8_t *body,
                          unsigned len)
{
    uint8_t out[1 + 2 * (RSTP_MAX_FRAME + 2)];
    unsigned pos = 0;
    uint16_t crc = rstp_crc16(body, len);

    out[pos++] = RSTP_START;
    for (unsigned i = 0; i < len; i++) {
        rstp_put_escaped(out, &pos, body[i]);
    }
    rstp_put_escaped(out, &pos, crc & 0xff);
    rstp_put_escaped(out, &pos, crc >> 8);

    if (fifo8_num_free(&s->outbuf) < pos) {
        trace_mh2p_rstp_overflow(body[0], pos);
        return;
    }
    fifo8_push_all(&s->outbuf, out, pos);
    trace_mh2p_rstp_tx(body[0], len > 2 ? body[2] : 0,
                       len > 3 ? body[3] : 0, len);
    rstp_flush(s);
}

static void rstp_send_ack(MH2PRstpChardev *s, uint8_t flag, uint8_t chn,
                          uint8_t sqn)
{
    const uint8_t body[] = { flag, 2, chn, sqn };

    rstp_send_raw(s, body, sizeof(body));
}

static void rstp_send_data(MH2PRstpChardev *s, uint8_t chn,
                           const RstpTxEntry *e)
{
    uint8_t body[4 + RSTP_MAX_DATA];

    body[0] = RSTP_FLAG_DATA;
    body[1] = e->len + 2;
    body[2] = chn;
    body[3] = e->sqn;
    memcpy(body + 4, e->data, e->len);
    rstp_send_raw(s, body, e->len + 4);
}

static void rstp_arm_timer(MH2PRstpChardev *s)
{
    RstpTxChannel *channels[] = { &s->keypanel, &s->pwrman };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t deadline = INT64_MAX;

    if (!s->link_up) {
        deadline = s->init_sent_ns + RSTP_INIT_RETRY_NS;
    } else {
        for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
            RstpTxChannel *c = channels[i];

            if (c->count && c->queue[c->head].sent) {
                deadline = MIN(deadline, c->sent_ns + RSTP_RETRANSMIT_NS);
            }
        }
    }
    if (deadline == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, MAX(deadline, now + 1));
    }
}

static void rstp_tx_pump_channel(MH2PRstpChardev *s, RstpTxChannel *c)
{
    unsigned inflight = 0;

    if (!s->link_up) {
        rstp_arm_timer(s);
        return;
    }
    for (unsigned i = 0; i < c->count; i++) {
        RstpTxEntry *e = &c->queue[(c->head + i) % RSTP_TX_DEPTH];

        if (!e->sent) {
            if (inflight >= RSTP_TX_WINDOW) {
                break;
            }
            e->sqn = c->next_sqn++;
            e->sent = true;
            if (!inflight) {
                c->sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            rstp_send_data(s, c->chn, e);
        }
        inflight++;
    }
    rstp_arm_timer(s);
}

static void rstp_tx_pump(MH2PRstpChardev *s)
{
    rstp_tx_pump_channel(s, &s->keypanel);
    rstp_tx_pump_channel(s, &s->pwrman);
}

static void rstp_tx_retransmit(MH2PRstpChardev *s, RstpTxChannel *c)
{
    c->sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < c->count; i++) {
        RstpTxEntry *e = &c->queue[(c->head + i) % RSTP_TX_DEPTH];

        if (!e->sent) {
            break;
        }
        trace_mh2p_rstp_retransmit(c->chn, e->sqn);
        rstp_send_data(s, c->chn, e);
    }
    rstp_arm_timer(s);
}

static void rstp_tx_queue_channel(MH2PRstpChardev *s, RstpTxChannel *c,
                                 const uint8_t *data, unsigned len)
{
    RstpTxEntry *e;

    assert(len <= RSTP_MAX_DATA);
    if (c->count == RSTP_TX_DEPTH) {
        trace_mh2p_rstp_tx_full(c->chn, data[0]);
        return;
    }
    e = &c->queue[(c->head + c->count) % RSTP_TX_DEPTH];
    memcpy(e->data, data, len);
    e->len = len;
    e->sent = false;
    c->count++;
    if (c->chn == RSTP_CHN_KEYPANEL) {
        trace_mh2p_rstp_keypanel(data[0], len);
    }
    rstp_tx_pump_channel(s, c);
}

static void rstp_tx_queue(MH2PRstpChardev *s, const uint8_t *data, unsigned len)
{
    rstp_tx_queue_channel(s, &s->keypanel, data, len);
}

/* Drop queued frames up to and including @sqn. */
static void rstp_tx_acked(MH2PRstpChardev *s, RstpTxChannel *c, uint8_t sqn)
{
    unsigned acknowledged = 0;

    for (unsigned i = 0; i < c->count; i++) {
        RstpTxEntry *e = &c->queue[(c->head + i) % RSTP_TX_DEPTH];

        if (!e->sent) {
            break;
        }
        if (e->sqn == sqn) {
            acknowledged = i + 1;
            break;
        }
    }
    if (!acknowledged) {
        trace_mh2p_rstp_ack_ignored(c->chn, sqn);
        return;
    }
    c->head = (c->head + acknowledged) % RSTP_TX_DEPTH;
    c->count -= acknowledged;
    c->sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    rstp_tx_pump_channel(s, c);
}

static void rstp_tx_nacked(MH2PRstpChardev *s, RstpTxChannel *c,
                           uint8_t expected)
{
    bool known = false;

    rstp_tx_acked(s, c, expected - 1);
    for (unsigned i = 0; i < c->count; i++) {
        RstpTxEntry *e = &c->queue[(c->head + i) % RSTP_TX_DEPTH];

        if (e->sent && e->sqn == expected) {
            known = true;
        }
    }
    if (!known) {
        /* The MMX side restarted its sequence; renumber everything. */
        for (unsigned i = 0; i < c->count; i++) {
            c->queue[(c->head + i) % RSTP_TX_DEPTH].sent = false;
        }
        c->next_sqn = expected;
        rstp_tx_pump_channel(s, c);
        return;
    }
    /*
     * The MMX NACKs every frame it cannot take yet (for example while no
     * client reads the channel); leave repeats to the retransmit timer.
     */
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
        c->sent_ns + RSTP_NACK_HOLDOFF_NS) {
        rstp_tx_retransmit(s, c);
    }
}

static void rstp_reset_sequences(MH2PRstpChardev *s)
{
    RstpTxChannel *channels[] = { &s->keypanel, &s->pwrman };

    memset(s->rx_last_sqn, 0xff, sizeof(s->rx_last_sqn));
    for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
        RstpTxChannel *c = channels[i];

        c->next_sqn = 0;
        for (unsigned j = 0; j < c->count; j++) {
            c->queue[(c->head + j) % RSTP_TX_DEPTH].sent = false;
        }
    }
}

static void rstp_send_init(MH2PRstpChardev *s)
{
    /*
     * Supplier 'A' (Alpine, matching the RCC_APN firmware), normal run
     * mode, no splash override.  The receiver strips bit 7 of every byte.
     */
    const uint8_t body[] = { RSTP_FLAG_INIT, 'A' | 0x80, 0x80, 0x80, 0x80 };

    s->init_sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    /* Do not pile up INITs while the guest is not draining the UART. */
    if (fifo8_is_empty(&s->outbuf)) {
        rstp_send_raw(s, body, sizeof(body));
    }
    rstp_arm_timer(s);
}

static void rstp_send_panels_ready(MH2PRstpChardev *s)
{
    static const uint8_t kbds[] = {
        KBD_FCC, KBD_MFW, KBD_TOUCHSCREEN_FRONT, KBD_VOLUME_CONTROL,
    };

    s->panels_ready = true;
    for (unsigned i = 0; i < ARRAY_SIZE(kbds); i++) {
        const uint8_t rec[] = { KP_FID_INPUT_PANEL_RDY, kbds[i], 1 };

        rstp_tx_queue(s, rec, sizeof(rec));
    }
}

static void rstp_watchdog_cb(void *opaque)
{
    MH2PRstpChardev *s = opaque;
    uint32_t counter = cpu_to_le32(s->watchdog_counter);
    uint8_t body[4 + sizeof(counter)] = {
        RSTP_FLAG_DATAN, 2 + sizeof(counter), RSTP_CHN_WATCHDOG,
        0,
    };

    if (s->link_up) {
        memcpy(body + 4, &counter, sizeof(counter));
        trace_mh2p_rstp_watchdog_tx(s->watchdog_counter);
        rstp_send_raw(s, body, sizeof(body));
        s->watchdog_counter++;
    }
    timer_mod(s->watchdog_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RSTP_WATCHDOG_NS);
}

static void rstp_timer_cb(void *opaque)
{
    MH2PRstpChardev *s = opaque;
    RstpTxChannel *channels[] = { &s->keypanel, &s->pwrman };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->link_up) {
        if (now >= s->init_sent_ns + RSTP_INIT_RETRY_NS) {
            rstp_send_init(s);
        }
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
        RstpTxChannel *c = channels[i];

        if (c->count && c->queue[c->head].sent &&
            now >= c->sent_ns + RSTP_RETRANSMIT_NS) {
            rstp_tx_retransmit(s, c);
        }
    }
    rstp_arm_timer(s);
}

static void rstp_send_clamp_state(MH2PRstpChardev *s)
{
    /* OOC protocol version 1, event 2: bit 0 is S, bit 1 is KL15. */
    const uint8_t data[] = { 1, 2, s->clamp_state };

    if (s->clamp_state >= 0) {
        rstp_tx_queue_channel(s, &s->pwrman, data, sizeof(data));
    }
}

static void rstp_send_power_state(MH2PRstpChardev *s)
{
    /* Event 1, main MMX selector 1, original power-state enum, no reason. */
    const uint8_t data[] = { 1, 1, 1, s->power_state, 0 };

    if (s->power_state >= 0) {
        rstp_tx_queue_channel(s, &s->pwrman, data, sizeof(data));
    }
}

static void rstp_handle_pwrman(MH2PRstpChardev *s, const uint8_t *data,
                               unsigned len)
{
    /* Fixed argument lengths from the original OOC pmou_event_len_args. */
    static const uint8_t arg_len[] = {
        0, 3, 1, 1, 1, 1, 1, 1, 2, 4, 2, 1, 3, 3, 1, 1,
    };
    bool publish_clamp = !s->pwrman_seen;
    bool publish_power = !s->pwrman_seen;

    if (!len || (data[0] & 0x70)) {
        trace_mh2p_rstp_drop("bad pwrman header", len ? data[0] : 0, len);
        return;
    }
    for (unsigned pos = 1; pos < len; ) {
        uint8_t event = data[pos++];

        if (event >= ARRAY_SIZE(arg_len) || len - pos < arg_len[event]) {
            trace_mh2p_rstp_drop("bad pwrman event", event, len);
            return;
        }
        if (event == 14 && data[pos] == 2) {
            publish_clamp = true;
        }
        if (event == 14 && data[pos] == 1) {
            publish_power = true;
        }
        pos += arg_len[event];
    }
    s->pwrman_seen = true;
    if (publish_clamp) {
        rstp_send_clamp_state(s);
    }
    if (publish_power) {
        rstp_send_power_state(s);
    }
}

static void rstp_handle_payload(MH2PRstpChardev *s, uint8_t chn,
                                const uint8_t *data, unsigned len)
{
    trace_mh2p_rstp_rx_data(chn, len, len ? data[0] : 0);
    if (chn == RSTP_CHN_PWRMAN) {
        rstp_handle_pwrman(s, data, len);
        return;
    }
    if (chn == RSTP_CHN_WATCHDOG && len == 4) {
        uint32_t reply = ldl_le_p(data);

        trace_mh2p_rstp_watchdog_rx(reply);
        /* Delayed replies must not rewind the next outgoing counter. */
        if (reply == s->watchdog_counter) {
            s->watchdog_counter++;
        } else {
            trace_mh2p_rstp_watchdog_reply_ignored(
                reply, s->watchdog_counter);
        }
        return;
    }
    if (chn == RSTP_CHN_KEYPANEL && !s->panels_ready) {
        rstp_send_panels_ready(s);
    }
}

static void rstp_handle_reliable(MH2PRstpChardev *s, uint8_t chn,
                                 uint8_t sqn, const uint8_t *data,
                                 unsigned len)
{
    uint8_t last = s->rx_last_sqn[chn];
    uint8_t ahead = sqn - last;

    if (ahead == 1) {
        s->rx_last_sqn[chn] = sqn;
        rstp_send_ack(s, RSTP_FLAG_ACK, chn, sqn);
        rstp_handle_payload(s, chn, data, len);
    } else if ((uint8_t)(last - sqn) < RSTP_SQN_WINDOW) {
        rstp_send_ack(s, RSTP_FLAG_ACK, chn, last);
    } else if (ahead <= RSTP_SQN_WINDOW + 1) {
        rstp_send_ack(s, RSTP_FLAG_NACK, chn, last + 1);
    } else {
        /* Far out of window: the MMX restarted, resynchronise. */
        trace_mh2p_rstp_resync(chn, last, sqn);
        s->rx_last_sqn[chn] = sqn;
        rstp_send_ack(s, RSTP_FLAG_ACK, chn, sqn);
        rstp_handle_payload(s, chn, data, len);
    }
}

static void rstp_handle_frame(MH2PRstpChardev *s, const uint8_t *f,
                              unsigned len)
{
    uint8_t flag = f[0];

    switch (flag) {
    case RSTP_FLAG_INIT:
        /* The MMX multiplexer restarted: its clients will start over. */
        trace_mh2p_rstp_rx(flag, 0, 0, len);
        rstp_reset_sequences(s);
        s->panels_ready = false;
        s->pwrman_seen = false;
        s->link_up = true;
        rstp_send_raw(s, (const uint8_t[]){ RSTP_FLAG_INIT_ACK }, 1);
        rstp_tx_pump(s);
        return;
    case RSTP_FLAG_INIT_ACK:
        trace_mh2p_rstp_rx(flag, 0, 0, len);
        if (!s->link_up) {
            rstp_reset_sequences(s);
            s->link_up = true;
            rstp_tx_pump(s);
        }
        return;
    case RSTP_FLAG_CHRST:
        trace_mh2p_rstp_rx(flag, f[1] ? f[2] : 0, 0, len);
        for (unsigned i = 0; i < f[1]; i++) {
            s->rx_last_sqn[f[2 + i]] = 0xff;
        }
        return;
    case RSTP_FLAG_ACK:
    case RSTP_FLAG_NACK:
        for (unsigned i = 0; i + 1 < f[1]; i += 2) {
            uint8_t chn = f[2 + i], sqn = f[3 + i];
            RstpTxChannel *c = chn == RSTP_CHN_KEYPANEL ? &s->keypanel :
                               chn == RSTP_CHN_PWRMAN ? &s->pwrman : NULL;

            trace_mh2p_rstp_rx(flag, chn, sqn, len);
            if (!c) {
                continue;
            }
            if (flag == RSTP_FLAG_ACK) {
                rstp_tx_acked(s, c, sqn);
            } else {
                rstp_tx_nacked(s, c, sqn);
            }
        }
        return;
    case RSTP_FLAG_DATA:
    case RSTP_FLAG_SDATA:
        trace_mh2p_rstp_rx(flag, f[2], f[3], len);
        rstp_handle_reliable(s, f[2], f[3], f + 4, f[1] - 2);
        return;
    case RSTP_FLAG_DATAN:
    case RSTP_FLAG_DATANC:
        trace_mh2p_rstp_rx(flag, f[2], f[3], len);
        rstp_handle_payload(s, f[2], f + 4, f[1] - 2);
        return;
    default:
        trace_mh2p_rstp_drop("unknown flag", flag, len);
    }
}

/* Expected unescaped length after the start byte, or 0 if not yet known. */
static unsigned rstp_expected_len(const uint8_t *f, unsigned have)
{
    switch (f[0]) {
    case RSTP_FLAG_INIT:
        return 1 + 4 + 2;
    case RSTP_FLAG_INIT_ACK:
        return 1 + 2;
    case RSTP_FLAG_DATANC:
        return have < 2 ? 0 : 2 + f[1];
    default:
        return have < 2 ? 0 : 2 + f[1] + 2;
    }
}

static bool rstp_valid_header(const uint8_t *f, unsigned have)
{
    switch (f[0]) {
    case RSTP_FLAG_INIT:
    case RSTP_FLAG_INIT_ACK:
        return true;
    case RSTP_FLAG_CHRST:
    case RSTP_FLAG_ACK:
    case RSTP_FLAG_NACK:
        return have < 2 || f[1] <= RSTP_MAX_DATA;
    case RSTP_FLAG_DATA:
    case RSTP_FLAG_SDATA:
    case RSTP_FLAG_DATAN:
    case RSTP_FLAG_DATANC:
        return have < 2 || (f[1] >= 2 && f[1] <= RSTP_MAX_DATA + 2);
    default:
        return false;
    }
}

static void rstp_rx_byte(MH2PRstpChardev *s, uint8_t byte)
{
    unsigned need;

    if (byte == RSTP_START) {
        if (s->rx_active && s->rx_len) {
            trace_mh2p_rstp_drop("truncated", s->rx[0], s->rx_len);
        }
        s->rx_active = true;
        s->rx_escape = false;
        s->rx_len = 0;
        return;
    }
    if (!s->rx_active) {
        return;
    }
    if (byte == RSTP_ESCAPE) {
        s->rx_escape = true;
        return;
    }
    if (s->rx_escape) {
        byte ^= RSTP_ESCAPE_XOR;
        s->rx_escape = false;
    }
    s->rx[s->rx_len++] = byte;
    if (!rstp_valid_header(s->rx, s->rx_len)) {
        trace_mh2p_rstp_drop("bad header", s->rx[0], s->rx_len);
        s->rx_active = false;
        return;
    }
    need = rstp_expected_len(s->rx, s->rx_len);
    if (!need || s->rx_len < need) {
        return;
    }
    s->rx_active = false;
    if (s->rx[0] != RSTP_FLAG_DATANC) {
        uint16_t crc = s->rx[need - 2] | s->rx[need - 1] << 8;

        if (crc != rstp_crc16(s->rx, need - 2)) {
            trace_mh2p_rstp_drop("bad crc", s->rx[0], need);
            return;
        }
    }
    rstp_handle_frame(s, s->rx, need);
}

static int rstp_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(chr);

    for (int i = 0; i < len; i++) {
        rstp_rx_byte(s, buf[i]);
    }
    return len;
}

static void rstp_queue_touch(MH2PRstpChardev *s, uint8_t finger_count)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t dt = s->touch_last_ms ? now - s->touch_last_ms : 0;
    uint16_t x = qemu_input_scale_axis(s->abs_x, INPUT_EVENT_ABS_MIN,
                                       INPUT_EVENT_ABS_MAX,
                                       0, TOUCH_RAW_WIDTH - 1);
    uint16_t y = qemu_input_scale_axis(s->abs_y, INPUT_EVENT_ABS_MIN,
                                       INPUT_EVENT_ABS_MAX,
                                       0, TOUCH_RAW_HEIGHT - 1);
    const uint8_t rec[] = {
        KP_FID_TOUCH_COORD, KBD_TOUCHSCREEN_FRONT, MIN(dt, 255),
        1, finger_count, 0,
        0, 0, x & 0xff, x >> 8, y & 0xff, y >> 8,
    };

    s->touch_last_ms = finger_count ? now : 0;
    trace_mh2p_rstp_touch(finger_count, s->last_x, s->last_y, rec[2]);
    rstp_tx_queue(s, rec, sizeof(rec));
}

static void rstp_queue_key(MH2PRstpChardev *s, uint8_t kbd, uint8_t key,
                           uint8_t state)
{
    const uint8_t rec[] = { KP_FID_KEY2, kbd, key, state, 0 };

    trace_mh2p_rstp_key(kbd, key, state);
    rstp_tx_queue(s, rec, sizeof(rec));
}

static void rstp_queue_multitouch(MH2PRstpChardev *s)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t dt = s->touch_last_ms ? now - s->touch_last_ms : 0;
    uint8_t rec[RSTP_MAX_DATA] = {
        KP_FID_TOUCH_COORD, KBD_TOUCHSCREEN_FRONT, MIN(dt, 255),
    };
    unsigned len = 6;

    if (!s->multitouch_dirty) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->contacts); i++) {
        RstpTouchSlot *contact = &s->contacts[i];
        uint16_t x, y;

        if (!contact->active && !contact->released) {
            continue;
        }
        x = qemu_input_scale_axis(contact->x, INPUT_EVENT_ABS_MIN,
                                  INPUT_EVENT_ABS_MAX, 0, TOUCH_RAW_WIDTH - 1);
        y = qemu_input_scale_axis(contact->y, INPUT_EVENT_ABS_MIN,
                                  INPUT_EVENT_ABS_MAX, 0, TOUCH_RAW_HEIGHT - 1);
        rec[3]++;
        rec[4] += contact->active;
        rec[len++] = i;
        /* Original hidcontroller removes this finger ID on status 0xff. */
        rec[len++] = contact->active ? 0 : 0xff;
        stw_le_p(rec + len, x);
        len += 2;
        stw_le_p(rec + len, y);
        len += 2;
        trace_mh2p_rstp_touch_contact(i, contact->active, x, y);
        contact->released = false;
    }
    s->multitouch_dirty = false;
    if (rec[3]) {
        s->touch_last_ms = rec[4] ? now : 0;
        trace_mh2p_rstp_multitouch(rec[3], rec[4], rec[2]);
        rstp_tx_queue(s, rec, len);
    }
}

static void rstp_end_multitouch(MH2PRstpChardev *s)
{
    rstp_queue_multitouch(s);
    for (unsigned i = 0; i < ARRAY_SIZE(s->contacts); i++) {
        RstpTouchSlot *contact = &s->contacts[i];

        if (contact->active) {
            contact->active = false;
            contact->released = true;
            s->multitouch_dirty = true;
        }
    }
    rstp_queue_multitouch(s);
    s->multitouch = false;
}

static void rstp_input_multitouch(MH2PRstpChardev *s,
                                  InputMultiTouchEvent *evt)
{
    RstpTouchSlot *contact;

    if (evt->slot < 0 || evt->slot >= ARRAY_SIZE(s->contacts)) {
        error_report("MH2P RSTP touch slot %" PRId64
                     " outside supported range 0..%u",
                     evt->slot, (unsigned)ARRAY_SIZE(s->contacts) - 1);
        return;
    }
    contact = &s->contacts[evt->slot];
    if (evt->type == INPUT_MULTI_TOUCH_TYPE_BEGIN) {
        if (evt->tracking_id < 0 || contact->active) {
            error_report("MH2P RSTP invalid touch begin: slot %" PRId64
                         ", tracking ID %" PRId64,
                         evt->slot, evt->tracking_id);
            return;
        }
        if (!s->multitouch) {
            if (s->touch_down) {
                rstp_queue_touch(s, 0);
                s->touch_down = false;
            }
            s->touch_moved = false;
            s->multitouch = true;
        }
        /* A reused slot must release its previous ID before a new press. */
        if (contact->released) {
            rstp_queue_multitouch(s);
        }
        *contact = (RstpTouchSlot) {
            .active = true,
            .tracking_id = evt->tracking_id,
        };
    } else {
        bool ending = evt->type == INPUT_MULTI_TOUCH_TYPE_END ||
                      evt->type == INPUT_MULTI_TOUCH_TYPE_CANCEL;

        if (!contact->active ||
            (evt->tracking_id != contact->tracking_id &&
             !(ending && evt->tracking_id == -1))) {
            error_report("MH2P RSTP touch event for inactive or stale slot %"
                         PRId64,
                         evt->slot);
            return;
        }
        if (ending) {
            /* Do not coalesce a press and its release out of existence. */
            rstp_queue_multitouch(s);
            contact->active = false;
            contact->released = true;
        } else if (evt->type == INPUT_MULTI_TOUCH_TYPE_DATA) {
            if (evt->axis == INPUT_AXIS_X) {
                contact->x = CLAMP(evt->value, INPUT_EVENT_ABS_MIN,
                                   INPUT_EVENT_ABS_MAX);
            } else {
                contact->y = CLAMP(evt->value, INPUT_EVENT_ABS_MIN,
                                   INPUT_EVENT_ABS_MAX);
            }
        }
    }
    s->multitouch_dirty = true;
}

static void rstp_update_position(MH2PRstpChardev *s)
{
    QemuConsole *con = s->abs_con ? s->abs_con :
                       qemu_console_lookup_by_index(0);
    int w = qemu_console_get_width(con, 1280);
    int h = qemu_console_get_height(con, 720);

    s->last_x = qemu_input_scale_axis(s->abs_x, INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX, 0, w - 1);
    s->last_y = qemu_input_scale_axis(s->abs_y, INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX, 0, h - 1);
}

static void rstp_input_key(MH2PRstpChardev *s, QemuInputKeyEvent *key)
{
    int qcode = qemu_input_linux_to_qcode(key->key);

    if (qcode <= 0 || qcode >= Q_KEY_CODE__MAX) {
        return;
    }
    if (test_bit(qcode, s->keys_down) == key->down) {
        return;
    }
    if (key->down) {
        set_bit(qcode, s->keys_down);
    } else {
        clear_bit(qcode, s->keys_down);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(mh2p_rstp_keymap); i++) {
        if (mh2p_rstp_keymap[i].qcode == qcode) {
            rstp_queue_key(s, mh2p_rstp_keymap[i].kbd,
                           mh2p_rstp_keymap[i].key,
                           key->down ? KST_PRESSED : KST_RELEASED);
            return;
        }
    }
}

static void rstp_input_event(DeviceState *dev, QemuConsole *src,
                             QemuInputEvent *evt)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        if (s->multitouch) {
            rstp_end_multitouch(s);
        }
        move = &evt->abs;
        if (move->axis == INPUT_AXIS_X) {
            s->abs_x = move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->abs_y = move->value;
        }
        s->abs_con = src;
        s->touch_moved = true;
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = &evt->btn;
        if (btn->button == INPUT_BUTTON_LEFT) {
            if (s->multitouch) {
                rstp_end_multitouch(s);
            }
            rstp_update_position(s);
            if (btn->down != s->touch_down) {
                s->touch_down = btn->down;
                rstp_queue_touch(s, btn->down ? 1 : 0);
            }
            s->touch_moved = false;
        } else if (btn->down && btn->button == INPUT_BUTTON_WHEEL_UP) {
            s->wheel++;
        } else if (btn->down && btn->button == INPUT_BUTTON_WHEEL_DOWN) {
            s->wheel--;
        }
        break;
    case INPUT_EVENT_KIND_KEY:
        rstp_input_key(s, &evt->key);
        break;
    case INPUT_EVENT_KIND_MTT:
        rstp_input_multitouch(s, &evt->mtt);
        break;
    default:
        break;
    }
}

static void rstp_input_sync(DeviceState *dev)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(dev);

    rstp_queue_multitouch(s);
    if (s->touch_down && s->touch_moved &&
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - s->touch_last_ms >=
        TOUCH_MOVE_INTERVAL_MS) {
        int x = s->last_x, y = s->last_y;

        rstp_update_position(s);
        if (x != s->last_x || y != s->last_y) {
            rstp_queue_touch(s, 1);
        }
        s->touch_moved = false;
    }
    if (s->wheel) {
        int steps = MAX(-127, MIN(127, s->wheel));
        const uint8_t rec[] = {
            KP_FID_ENCODER2, KBD_VOLUME_CONTROL, KEY_VOL, 0,
            (uint8_t)steps, 0, 0, 0,
        };

        s->wheel = 0;
        trace_mh2p_rstp_encoder(KBD_VOLUME_CONTROL, steps);
        rstp_tx_queue(s, rec, sizeof(rec));
    }
}

static const QemuInputHandler rstp_input_handler = {
    .name  = "MH2P RCC keypanel",
    .mask  = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS |
             INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_MTT,
    .event = rstp_input_event,
    .sync  = rstp_input_sync,
};

static void rstp_reset(void *opaque)
{
    MH2PRstpChardev *s = opaque;

    fifo8_reset(&s->outbuf);
    timer_del(s->timer);
    s->rx_active = false;
    s->link_up = false;
    s->panels_ready = false;
    s->keypanel.head = 0;
    s->keypanel.count = 0;
    s->pwrman.head = 0;
    s->pwrman.count = 0;
    s->pwrman_seen = false;
    s->touch_down = false;
    s->touch_moved = false;
    s->touch_last_ms = 0;
    memset(s->contacts, 0, sizeof(s->contacts));
    s->multitouch = false;
    s->multitouch_dirty = false;
    s->wheel = 0;
    bitmap_zero(s->keys_down, Q_KEY_CODE__MAX);
    rstp_reset_sequences(s);
    /* Like a booting RCC, announce INIT until the MMX acknowledges it. */
    s->init_sent_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                      RSTP_INIT_RETRY_NS;
    timer_mod(s->timer, s->init_sent_ns + RSTP_INIT_RETRY_NS + 1);
    s->watchdog_counter = 0;
    timer_mod(s->watchdog_timer, s->init_sent_ns + RSTP_INIT_RETRY_NS +
                                 RSTP_WATCHDOG_NS);
}

static bool rstp_chr_open(Chardev *chr, ChardevBackend *backend,
                          Error **errp)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(chr);

    fifo8_create(&s->outbuf, RSTP_OUTBUF_SIZE);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rstp_timer_cb, s);
    s->watchdog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rstp_watchdog_cb, s);
    s->keypanel.chn = RSTP_CHN_KEYPANEL;
    s->pwrman.chn = RSTP_CHN_PWRMAN;
    rstp_reset(s);
    qemu_register_reset(rstp_reset, s);
    s->hs = qemu_input_handler_register((DeviceState *)s,
                                        &rstp_input_handler);
    qemu_input_handler_activate(s->hs);
    return true;
}

static void rstp_chr_finalize(Object *obj)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(obj);

    if (s->hs) {
        qemu_input_handler_unregister(s->hs);
        qemu_unregister_reset(rstp_reset, s);
        timer_free(s->timer);
        timer_free(s->watchdog_timer);
        fifo8_destroy(&s->outbuf);
    }
}

static void rstp_chr_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = rstp_chr_open;
    cc->chr_write = rstp_chr_write;
    cc->chr_accept_input = rstp_chr_accept_input;
}

static void rstp_get_clamp_state(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    int64_t value = CHARDEV_MH2P_RSTP(obj)->clamp_state;

    visit_type_int(v, name, &value, errp);
}

static void rstp_set_clamp_state(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < -1 || value > 3) {
        error_setg(errp, "clamp-state must be -1 (unknown) or 0..3 "
                   "(S in bit 0, KL15 in bit 1)");
        return;
    }
    if (value == s->clamp_state) {
        return;
    }
    if (s->pwrman_seen && value >= 0 &&
        s->pwrman.count == RSTP_TX_DEPTH) {
        error_setg(errp, "RSTP power-state queue is full");
        return;
    }
    s->clamp_state = value;
    if (s->pwrman_seen) {
        rstp_send_clamp_state(s);
    }
}

static void rstp_get_power_state(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    int64_t value = CHARDEV_MH2P_RSTP(obj)->power_state;

    visit_type_int(v, name, &value, errp);
}

static void rstp_set_power_state(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    MH2PRstpChardev *s = CHARDEV_MH2P_RSTP(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }
    if (value < -1 || value > 18) {
        error_setg(errp, "power-state must be -1 (unknown) or an original "
                   "OOC POWERSTATE value 0..18");
        return;
    }
    if (value == s->power_state) {
        return;
    }
    if (s->pwrman_seen && value >= 0 &&
        s->pwrman.count == RSTP_TX_DEPTH) {
        error_setg(errp, "RSTP power-state queue is full");
        return;
    }
    s->power_state = value;
    if (s->pwrman_seen) {
        rstp_send_power_state(s);
    }
}

static void rstp_chr_init(Object *obj)
{
    CHARDEV_MH2P_RSTP(obj)->clamp_state = -1;
    CHARDEV_MH2P_RSTP(obj)->power_state = -1;
    object_property_add(obj, "clamp-state", "int",
                        rstp_get_clamp_state, rstp_set_clamp_state, NULL, NULL);
    object_property_add(obj, "power-state", "int",
                        rstp_get_power_state, rstp_set_power_state, NULL, NULL);
}

static const TypeInfo rstp_chr_type_info = {
    .name = TYPE_CHARDEV_MH2P_RSTP,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(MH2PRstpChardev),
    .instance_init = rstp_chr_init,
    .instance_finalize = rstp_chr_finalize,
    .class_init = rstp_chr_class_init,
};

static void register_types(void)
{
    type_register_static(&rstp_chr_type_info);
}

type_init(register_types);
