/* SNDN2DRV.IRX ('sndn') sound driver - protocol-level HLE.
 *
 * fno 100: a batch of 16-byte commands {type, arg1, arg2, arg3}; reply is a
 * 512-byte status block (double buffered on the IOP side).
 *  - Commands 32/33 (data transfers) carry a sequence number in arg1>>8 that
 *    the driver echoes at status+0x1c0 so the EE can wait for completion.
 *  - Commands 70..79 drive 16 PCM stream channels (title music, cut-scene
 *    audio). Each channel reads a ring buffer in IOP RAM that the EE refills
 *    from CD; the driver advances 0x400 bytes per 256-sample block at 48 kHz
 *    and reports read positions at status+0x180 + ch*4.
 * Stream PCM is 16-bit, stereo channels interleaved in 512-byte blocks.
 */
#include "rt.h"

static uint32_t status_buf[2];
static uint32_t result_word;
static int flip;
static uint32_t ack_seq;

typedef struct {
    uint32_t stop, buf, size, pos, step_shift, volL, volR, saved_shift;
} StreamCh;
static StreamCh sch[16];
static int stream_running;
static uint64_t stream_last_us;
static double block_acc;

uint32_t snd_stream_snapshot(int ch, uint32_t *buf, uint32_t *size, uint32_t *pos, int *playing, int *vl, int *vr) {
    StreamCh *s = &sch[ch & 15];
    *buf = s->buf; *size = s->size; *pos = s->pos; *playing = stream_running && (s->step_shift >> 16) && s->pos != s->stop;
    *vl = (int)s->volL; *vr = (int)s->volR;
    return s->step_shift & 0xffff;
}

static void stream_advance(void) {
    uint64_t now = host_usec();
    if (!stream_last_us) stream_last_us = now;
    double blocks = (double)(now - stream_last_us) * (48000.0 / 256.0) / 1e6 + block_acc;
    stream_last_us = now;
    if (!stream_running) { block_acc = 0; return; }
    int n = (int)blocks;
    block_acc = blocks - n;
    if (n > 200) n = 200;
    for (int b = 0; b < n; b++)
        for (int c = 0; c < 16; c++) {
            StreamCh *s = &sch[c];
            if (!(s->step_shift >> 16) || s->pos == s->stop) continue;
            if (s->pos == s->size) s->pos = 0;
            s->pos += s->step_shift & 0xffff;
        }
}

static void sound_command(const uint32_t *cmd) {
    uint32_t type = cmd[0];
    { static int nl; if (nl < 400 && g_log_level >= 3 && type != 0) { nl++; TRACE("[snd] cmd %u %08x %08x %08x (frame %u)\n", type, cmd[1], cmd[2], cmd[3], g_frame); } }
    switch (type) {
    case 32: case 33: ack_seq = cmd[1] >> 8; break;
    case 70: stream_advance(); stream_running = 1; break;
    case 71: stream_advance(); stream_running = 0; break;
    case 72: { StreamCh *s = &sch[(cmd[1] >> 24) & 15];
        s->stop = 0x200000; s->step_shift = cmd[1] & 0xffff; s->saved_shift = cmd[1] & 0xff0000;
        s->buf = cmd[2]; s->size = cmd[3]; s->pos = 0; break; }
    case 73: memset(&sch[cmd[1] & 15], 0, sizeof(StreamCh)); break;
    case 74: for (int c = 0; c < 16; c++) if ((cmd[1] >> c) & 1) { sch[c].volL = cmd[2]; sch[c].volR = cmd[3]; } break;
    case 75: stream_advance(); for (int c = 0; c < 16; c++) if ((cmd[1] >> c) & 1) sch[c].step_shift |= sch[c].saved_shift; break;
    case 76: stream_advance(); for (int c = 0; c < 16; c++) if ((cmd[1] >> c) & 1) sch[c].step_shift &= 0xffff; break;
    case 77: sch[cmd[1] & 15].pos = cmd[2]; break;
    case 79: for (int c = 0; c < 16; c++) if ((cmd[1] >> c) & 1) sch[c].stop = (cmd[3] == 1) ? cmd[2] : 0x200000; break;
    default: break;
    }
}

static uint32_t sndn_handler(uint32_t sid, int fno, uint32_t buf, int size, int *rs) {
    (void)sid; (void)rs;
    if (fno == 100) {
        for (int off = 0; off + 16 <= size; off += 16) sound_command((const uint32_t *)IOP(buf + off));
        stream_advance();
        flip ^= 1;
        uint32_t st = status_buf[flip];
        memset(IOP(st), 0, 512);
        /* positions are reported in whole 2 KB units (one CD sector of stream data):
         * the EE refills by sectors and treats a zero-sector refill as end of stream */
        for (int c = 0; c < 16; c++) *(uint32_t *)IOP(st + 0x180 + c * 4) = sch[c].pos & ~0x7ffu;
        *(uint32_t *)IOP(st + 0x1c0) = ack_seq;
        return st;
    }
    sound_command((const uint32_t *)IOP(buf));
    *(uint32_t *)IOP(result_word) = 1;
    return result_word;
}

void iop_sndn_start(void);
void iop_sound_init(void) {
    if (!getenv("AE2_SNDHLE")) { iop_sndn_start(); return; }   /* real driver, recompiled */
    status_buf[0] = iop_alloc(512);
    status_buf[1] = iop_alloc(512);
    result_word = iop_alloc(64);
    iop_register_server(0x736e646e, sndn_handler, 0x4000);
}
