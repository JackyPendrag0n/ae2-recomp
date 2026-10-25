/* SIF (EE<->IOP) emulation at the sifcmd/sifrpc protocol level.
 *
 * The EE-side Sony libraries run unmodified. Their DMA transfers land in a
 * simulated 2 MB IOP RAM; packets addressed to the IOP command buffer are
 * decoded and serviced by host implementations of the IOP RPC servers, and
 * replies are DMA'd back into EE RAM followed by a SIF0 DMAC interrupt.
 */
#include "rt.h"

uint8_t *g_iopram;

#define IOP_CMDBUF   0x000F0000u   /* where the EE sends command packets */
#define IOP_SD_BASE  0x000F1000u   /* fake server-data structs */
#define IOP_HEAP_LO  0x00100000u
#define IOP_HEAP_HI  0x001F0000u

#define SIF_CMD_CHANGE_SADDR 0x80000000u
#define SIF_CMD_SET_SREG     0x80000001u
#define SIF_CMD_INIT_CMD     0x80000002u
#define SIF_CMD_RESET_CMD    0x80000003u
#define SIF_CMD_RPC_END      0x80000008u
#define SIF_CMD_RPC_BIND     0x80000009u
#define SIF_CMD_RPC_CALL     0x8000000Au
#define SIF_CMD_RPC_RDATA    0x8000000Cu

static uint32_t ee_pktbuf;        /* EE address of its receive buffer */
static uint32_t sysregs[8];       /* 0x80000000.. */
static uint32_t regs[8];
static int32_t dma_id = 1;

/* ---------------- server table */
typedef struct { uint32_t sid; RpcHandler h; uint32_t buf, bufsize, sd; } Server;
static Server servers[64];
static int nservers;

void iop_register_server(uint32_t sid, RpcHandler h, uint32_t bufsize) {
    Server *s = &servers[nservers];
    s->sid = sid; s->h = h; s->bufsize = bufsize ? bufsize : 0x1000;
    s->buf = iop_alloc(s->bufsize);
    s->sd = IOP_SD_BASE + nservers * 0x40;
    nservers++;
}
/* server whose receive buffer lives in an IOP module's own memory */
void iop_register_server_buf(uint32_t sid, RpcHandler h, uint32_t buf, uint32_t bufsize) {
    Server *s = &servers[nservers];
    s->sid = sid; s->h = h; s->bufsize = bufsize; s->buf = buf;
    s->sd = IOP_SD_BASE + nservers * 0x40;
    nservers++;
}
static Server *find_sid(uint32_t sid) { for (int i = 0; i < nservers; i++) if (servers[i].sid == sid) return &servers[i]; return NULL; }
static Server *find_sd(uint32_t sd) { for (int i = 0; i < nservers; i++) if (servers[i].sd == sd) return &servers[i]; return NULL; }

/* ---------------- IOP heap: first-fit free list over 0x40000..0x1F0000 */
typedef struct { uint32_t addr, size; int used; } Blk;
static Blk blks[4096];
static int nblks;
uint32_t iop_alloc(uint32_t size) {
    if (!nblks) { blks[0] = (Blk){0x40000, IOP_HEAP_HI - 0x40000, 0}; nblks = 1; }
    size = (size + 63) & ~63u;
    for (int i = 0; i < nblks; i++) {
        if (blks[i].used || blks[i].size < size) continue;
        if (blks[i].size > size && nblks < 4096) {
            memmove(&blks[i + 2], &blks[i + 1], sizeof(Blk) * (nblks - i - 1));
            blks[i + 1] = (Blk){blks[i].addr + size, blks[i].size - size, 0};
            blks[i].size = size;
            nblks++;
        }
        blks[i].used = 1;
        memset(IOP(blks[i].addr), 0, size);
        return blks[i].addr;
    }
    WARN("[iop] heap exhausted (%u bytes requested)\n", size);
    return 0;
}
void iop_free(uint32_t addr) {
    for (int i = 0; i < nblks; i++) {
        if (blks[i].addr != addr || !blks[i].used) continue;
        blks[i].used = 0;
        if (i + 1 < nblks && !blks[i + 1].used) {
            blks[i].size += blks[i + 1].size;
            memmove(&blks[i + 1], &blks[i + 2], sizeof(Blk) * (nblks - i - 2)); nblks--;
        }
        if (i > 0 && !blks[i - 1].used) {
            blks[i - 1].size += blks[i].size;
            memmove(&blks[i], &blks[i + 1], sizeof(Blk) * (nblks - i - 1)); nblks--;
        }
        return;
    }
}

/* ---------------- outgoing (IOP -> EE) packet queue */
typedef struct { uint8_t pkt[128]; uint32_t psize; uint32_t esrc_host; uint32_t edst; uint32_t esize; uint8_t *edata; } OutPkt;
static OutPkt outq[256];
static int outq_head, outq_tail;

static void queue_to_ee(uint32_t cid, uint8_t *pkt, uint32_t psize, const void *extra, uint32_t edst, uint32_t esize) {
    OutPkt *o = &outq[outq_tail];
    outq_tail = (outq_tail + 1) & 255;
    if (outq_tail == outq_head) WARN("[sif] out queue overflow\n");
    uint32_t *h = (uint32_t *)pkt;
    h[0] = (psize & 0xff) | (esize << 8);
    h[1] = esize ? edst : 0;
    h[2] = cid;
    memcpy(o->pkt, pkt, psize);
    o->psize = psize;
    o->edst = edst; o->esize = esize;
    o->edata = NULL;
    if (esize && extra) { o->edata = malloc(esize); memcpy(o->edata, extra, esize); }
    sif_poll();
}

/* Deliver the next packet once the EE handler has consumed the previous one. */
void sif_poll(void) {
    extern int g_ghost; if (g_ghost) return;   /* replies wait until the second-view pass is over */
    if (outq_head == outq_tail || !ee_pktbuf) return;
    uint32_t psz = MR32(ee_pktbuf) & 0xff;
    if (psz != 0) { kernel_raise_dmac(5); return; }  /* previous still pending */
    OutPkt *o = &outq[outq_head];
    outq_head = (outq_head + 1) & 255;
    if (o->esize && o->edata) { gwrite(o->edst, o->edata, o->esize); free(o->edata); }
    gwrite(ee_pktbuf, o->pkt, o->psize);
    kernel_raise_dmac(5);
}

/* ---------------- incoming command processing */
static void send_end(uint32_t cid_req, const uint32_t *req, uint32_t cd, uint32_t sd, uint32_t buf, uint32_t cbuf,
                     const void *extra, uint32_t edst, uint32_t esize) {
    uint32_t p[16] = {0};
    p[4] = req[4]; p[5] = req[5]; p[6] = req[6];
    p[7] = cd; p[8] = cid_req; p[9] = sd; p[10] = buf; p[11] = cbuf;
    queue_to_ee(SIF_CMD_RPC_END, (uint8_t *)p, 64, extra, edst, esize);
}

static void handle_cmd(uint32_t iopaddr) {
    uint32_t *w = (uint32_t *)IOP(iopaddr);
    uint32_t cid = w[2];
    switch (cid) {
    case SIF_CMD_CHANGE_SADDR:
        ee_pktbuf = w[4];
        INFO("[sif] EE receive buffer -> %08x\n", ee_pktbuf);
        break;
    case SIF_CMD_INIT_CMD:
        if (w[3] == 0) {
            ee_pktbuf = w[4];
            INFO("[sif] INIT_CMD, EE buffer %08x\n", ee_pktbuf);
        } else {
            uint32_t p[8] = {0};
            p[4] = 0; p[5] = 1;  /* SIF_SREG_RPCINIT = 1 */
            queue_to_ee(SIF_CMD_SET_SREG, (uint8_t *)p, 24, NULL, 0, 0);
            INFO("[sif] RPC init\n");
        }
        break;
    case SIF_CMD_RESET_CMD: {
        char arg[96] = {0};
        memcpy(arg, &w[5], 80);
        INFO("[sif] IOP reset requested '%s'\n", arg);
        break;
    }
    case SIF_CMD_SET_SREG:
        break;
    case SIF_CMD_RPC_BIND: {
        uint32_t cd = w[7], sid = w[8];
        Server *s = find_sid(sid);
        if (!s) { static int nb; if (nb++ < 3) { WARN("[sif] bind to unknown server %08x pkt:", sid); for (int i = 0; i < 16; i++) WARN(" %08x", w[i]); WARN("\n"); } }
        else INFO("[sif] bind %08x\n", sid);
        send_end(SIF_CMD_RPC_BIND, w, cd, s ? s->sd : 0, s ? s->buf : 0, s ? s->buf : 0, NULL, 0, 0);
        break;
    }
    case SIF_CMD_RPC_CALL: {
        uint32_t cd = w[7], fno = w[8], ssize = w[9], recvbuf = w[10], rsize = w[11], rmode = w[12], sd = w[13];
        Server *s = find_sd(sd);
        uint32_t res = 0;
        int hint = (int)rsize;
        if (s) {
            TRACE("[sif] call %08x fno=%d ssize=%d rsize=%d\n", s->sid, fno, ssize, rsize);
            res = s->h(s->sid, (int)fno, s->buf, (int)ssize, &hint);
        } else WARN("[sif] call to unknown sd %08x\n", sd);
        uint32_t n = (res && recvbuf) ? rsize : 0;
        if (rmode) {
            send_end(SIF_CMD_RPC_CALL, w, cd, 0, 0, 0, n ? IOP(res) : NULL, recvbuf, n);
        } else {
            if (n) gwrite(recvbuf, IOP(res), n);
            uint32_t p[16] = {0};
            p[7] = cd; p[8] = SIF_CMD_RPC_CALL;
            gwrite(w[5], p, 64);  /* rpc_id = rec_id = 0 marks completion */
        }
        break;
    }
    case SIF_CMD_RPC_RDATA: {
        uint32_t rd = w[7], src = w[8], dst = w[9], size = w[10];
        send_end(SIF_CMD_RPC_RDATA, w, rd, 0, 0, 0, IOP(src), dst, size);
        break;
    }
    default:
        WARN("[sif] unhandled command %08x\n", cid);
    }
}

int32_t sif_set_dma(uint32_t trans, int count) {
    extern int g_ghost; if (g_ghost) count = 0;   /* the second-view pass sends nothing to the IOP (no doubled sounds) */
    for (int i = 0; i < count; i++) {
        uint32_t t = trans + i * 16;
        uint32_t src = MR32(t), dst = MR32(t + 4), size = MR32(t + 8), attr = MR32(t + 12);
        uint32_t d = dst & 0x1fffff;
        size = (size + 3) & ~3u;
        if (d + size > 0x200000) { WARN("[sif] dma out of range %08x+%x\n", dst, size); continue; }
        gread(IOP(d), src, size);
        TRACE("[sif] dma %08x -> iop %08x (%x) attr %x\n", src, dst, size, attr);
        if (d == IOP_CMDBUF) handle_cmd(d);
    }
    int32_t id = dma_id++;
    if (dma_id > 0xffff) dma_id = 1;
    return id;
}

uint32_t sif_get_reg(uint32_t reg) {
    if (reg & 0x80000000u) return sysregs[reg & 7];
    switch (reg) {
    case 2: return IOP_CMDBUF;               /* SIF_REG_SUBADDR */
    case 4: return 0x00070000;               /* SMFLAG: SIFINIT|CMDINIT|BOOTEND */
    default: return regs[reg & 7];
    }
}
void sif_set_reg(uint32_t reg, uint32_t val) {
    if (reg & 0x80000000u) sysregs[reg & 7] = val;
    else regs[reg & 7] = val;
}

void sif_init(void) {
    g_iopram = calloc(1, 0x200000);
    iop_services_init();
}
