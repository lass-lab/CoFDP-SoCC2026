#include "fdpftl.h"
#include <time.h>

//#define FEMU_DEBUG_FTL

static void *fdpftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd, int rg_id)
{
    return (ssd->lm[rg_id].free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd, int rg_id)
{
    return (ssd->lm[rg_id].free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

/*
 * Target RG for a host write that is redirected because its RG is still at
 * the high GC watermark, and for the valid pages that forced GC copies out of
 * a victim with few invalid pages. The choice is wall-clock based (current
 * second modulo rg_number); the target is not checked for free RUs and may
 * be the source RG itself. NOTE: see KNOWN_ISSUES (FEMU-L2).
 */
static uint8_t fdp_spill_rg(struct ssd *ssd)
{
    time_t t;

    time(&t);
    return t % ssd->rg_number;
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

/* (Re)build the RU lists of RG rg with every RU free */
static void fdpssd_init_lines(struct ssd *ssd, int rg, bool is_init)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm[rg];
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    if (is_init) {
        lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);
    } else {
        pqueue_free(lm->victim_line_pq);
    }

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->ph = -1;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

/*
 * Dies of RG rg_id: channels start_ch..end_ch x LUNs start_lun..end_lun.
 * Unsupported rg_number values (not a power-of-two divisor of the die count)
 * yield -1 and are not rejected. NOTE: see KNOWN_ISSUES (FEMU-11).
 */
static inline void rg2physical(struct ssdparams *spp, int rg_id, int *start_ch,
                               int *start_lun, int *end_ch, int *end_lun)
{
    if (spp->chnls_per_rg > 1) {
        /* the RG spans several whole channels */
        *start_ch = rg_id * spp->chnls_per_rg;
        *start_lun = 0;
        *end_ch = *start_ch + spp->chnls_per_rg - 1;
        *end_lun = spp->luns_per_ch - 1;
    } else if (spp->rgs_per_chnl > 1) {
        /* several RGs share one channel */
        *start_ch = rg_id / spp->rgs_per_chnl;
        *start_lun = (rg_id % spp->rgs_per_chnl) * spp->luns_per_rg;
        *end_ch = rg_id / spp->rgs_per_chnl;
        *end_lun = *start_lun + spp->luns_per_rg - 1;
    } else if (spp->rgs_per_chnl == 1 && spp->chnls_per_rg == 1) {
        /* one RG per channel */
        *start_ch = rg_id;
        *start_lun = 0;
        *end_ch = rg_id;
        *end_lun = spp->luns_per_ch - 1;
    } else {
        *start_ch = -1;
        *start_lun = -1;
        *end_ch = -1;
        *end_lun = -1;
    }
}

/*
 * Open the first free RU of RG rg_id for placement handle ph. The host pages
 * written through this RUH since the previous reset are reported and then
 * cleared; the "RG %d STREAM %d : written" line is WAF measurement output and
 * must keep its format. NOTE: the first report (at QEMU start) prints an
 * uninitialized counter; see KNOWN_ISSUES (FEMU-8).
 */
static void fdpssd_init_write_pointer(struct ssd *ssd, int ph, int rg_id)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp[ph][rg_id];
    struct line_mgmt *lm = &ssd->lm[rg_id];
    struct line *curline = NULL;
    int end_ch, end_lun;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    printf("RG %d STREAM %d : written : %5lu\n", rg_id, ph, wpp->written);

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    curline->ph = ph;
    /* start at the first die of the RG */
    rg2physical(spp, rg_id, &wpp->ch, &wpp->lun, &end_ch, &end_lun);
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
    wpp->written = 0;
}

static struct line *get_next_free_line(struct ssd *ssd, int rg_id)
{
    struct line_mgmt *lm = &ssd->lm[rg_id];
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

/* Stripe channel first, then die, then page, over the dies of the RG */
static void ssd_advance_write_pointer(struct ssd *ssd, int ph, int rg_id)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp[ph][rg_id];
    struct line_mgmt *lm = &ssd->lm[rg_id];
    int start_ch, start_lun, end_ch, end_lun;

    rg2physical(spp, rg_id, &start_ch, &start_lun, &end_ch, &end_lun);

    wpp->ch++;
    if (wpp->ch > end_ch) {
        wpp->ch = start_ch;
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun > end_lun) {
            wpp->lun = start_lun;
            /* go to next page in the block */
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current RU is used up, open the next free RU of the RG */
                wpp->curline = get_next_free_line(ssd, rg_id);
                if (!wpp->curline) {
                    ftl_err("RG %d: no free RU for placement handle %d "
                            "(victim=%d, full=%d)\n", rg_id, ph,
                            lm->victim_line_cnt, lm->full_line_cnt);
                    abort();
                }
                wpp->curline->ph = ph;
                wpp->blk = wpp->curline->id;
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd, int ph, int rg_id)
{
    struct write_pointer *wpp = &ssd->wp[ph][rg_id];
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    uint64_t nand_block_size = (n->bb_params.nand_block_size_mb << 20);
    uint64_t nand_page_size = (n->bb_params.nand_page_size_kb << 10);
    uint64_t user_device_size_mb = n->memsz;
    uint64_t user_space_ratio, total_device_size_mb, lun_size_mb;

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent / 100.0;
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high / 100.0;

    /*
     * devsz_mb is the user capacity. The NAND capacity is chosen so that the
     * user capacity is gc_thres_pcent percent of it, i.e. over-provisioning
     * is tied to the background GC threshold.
     */
    user_space_ratio = 100 * (spp->gc_thres_pcent);
    total_device_size_mb = (user_device_size_mb * 100) / user_space_ratio;

    /* one sector == one 4 KiB LBA (see femu.c) */
    spp->secsz = 4096;
    spp->secs_per_pg = nand_page_size / spp->secsz;
    spp->pgs_per_blk = nand_block_size / nand_page_size;
    spp->pls_per_lun = n->bb_params.pls_per_lun;
    spp->luns_per_ch = n->bb_params.luns_per_ch;
    spp->nchs = n->bb_params.nchs;

    lun_size_mb = total_device_size_mb / (spp->nchs * spp->luns_per_ch);
    spp->blks_per_pl = lun_size_mb / (n->bb_params.nand_block_size_mb);

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end: one line (RU) spans an RG */
    spp->blks_per_line = spp->tt_luns / n->rg_number; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    /* reclaim group geometry */
    spp->rgs_per_chnl = n->rg_number / spp->nchs;
    spp->luns_per_rg = (spp->luns_per_ch * spp->nchs) / n->rg_number;
    spp->chnls_per_rg = spp->luns_per_rg / spp->luns_per_ch;
}

/*
 * The ssd_init_* helpers allocate the NAND state when is_init is true and
 * only reset it otherwise (FEMU reset, see fdpssd_femu_reset()).
 */
static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp,
                               bool is_init)
{
    pg->nsecs = spp->secs_per_pg;
    if (is_init) {
        pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    }
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp,
                              bool is_init)
{
    blk->npgs = spp->pgs_per_blk;
    if (is_init) {
        blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    }
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp, is_init);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp,
                                bool is_init)
{
    pl->nblks = spp->blks_per_pl;
    if (is_init) {
        pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    }
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp, is_init);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp,
                              bool is_init)
{
    lun->npls = spp->pls_per_lun;
    if (is_init) {
        lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    }
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp, is_init);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp,
                        bool is_init)
{
    ch->nluns = spp->luns_per_ch;
    if (is_init) {
        ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    }
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp, is_init);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd, bool is_init)
{
    struct ssdparams *spp = &ssd->sp;

    if (is_init) {
        ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    }
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd, bool is_init)
{
    struct ssdparams *spp = &ssd->sp;

    if (is_init) {
        ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    }
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void fdpssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    int i, j;

    ftl_assert(ssd);

    ssd->rg_number = n->rg_number;
    ssd->handle_number = n->handle_number;
    ssd->femu = n;
    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);

    /* RU lists per RG and one write pointer per (placement handle, RG) */
    ssd->lm = g_malloc0(sizeof(struct line_mgmt) * ssd->rg_number);
    ssd->wp = g_malloc(sizeof(struct write_pointer *) * ssd->handle_number);
    for (i = 0; i < ssd->handle_number; i++) {
        ssd->wp[i] = g_malloc(sizeof(struct write_pointer) * ssd->rg_number);
    }

    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp, true);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd, true);

    /* initialize rmap */
    ssd_init_rmap(ssd, true);

    /* initialize all the lines */
    for (i = 0; i < ssd->rg_number; i++) {
        fdpssd_init_lines(ssd, i, true);
    }

    /* initialize write pointer, this is how we allocate new pages for writes */
    for (i = 0; i < ssd->handle_number; i++) {
        for (j = 0; j < ssd->rg_number; j++) {
            fdpssd_init_write_pointer(ssd, i, j);
        }
    }

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FDP-Thread", fdpftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

/* RG that owns the die of ppa (-1 for an unsupported rg_number) */
static inline int get_rg_id(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;

    if (spp->chnls_per_rg > 1) {
        return ppa->g.ch / spp->chnls_per_rg;
    } else if (spp->rgs_per_chnl > 1) {
        return (ppa->g.ch * spp->rgs_per_chnl) + (ppa->g.lun / spp->luns_per_rg);
    } else if (spp->rgs_per_chnl == 1 && spp->chnls_per_rg == 1) {
        return ppa->g.ch;
    }

    return -1;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm[get_rg_id(ssd, ppa)].lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

/*
 * Unlike bbssd, both directions model the channel: a read occupies the die,
 * then the channel; a write occupies the channel, then the die.
 */
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    uint64_t chnl_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    struct ssd_channel *ch = get_ch(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    if (pg->status == PG_INVALID) {
        /* must not happen: the page has been invalidated before */
        ftl_err("page of LPN %" PRIu64 " is already invalid\n",
                get_rmap_ent(ssd, ppa));
        return;
    }
    pg->status = PG_INVALID;
    lm = &ssd->lm[get_rg_id(ssd, ppa)];

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

/*
 * GC reads, writes and erases are always timed in this model (enable_gc_delay
 * is not consulted) and are stamped with the current wall-clock time.
 */
static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_cmd gcr;

    gcr.type = GC_IO;
    gcr.cmd = NAND_READ;
    gcr.stime = 0;
    ssd_advance_status(ssd, ppa, &gcr);
}

/*
 * Move one valid page (already in DRAM) of a victim RU to the open RU of the
 * same placement handle, in the victim's RG or, if spill is set, in the RG
 * chosen by fdp_spill_rg() for this page.
 */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int ph,
                              int rg_id, bool spill)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    struct nand_cmd gcw;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);
    int new_rg_id = spill ? fdp_spill_rg(ssd) : rg_id;

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, ph, new_rg_id);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);
    mark_page_invalid(ssd, old_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, ph, new_rg_id);

    gcw.type = GC_IO;
    gcw.cmd = NAND_WRITE;
    gcw.stime = 0;
    ssd_advance_status(ssd, &new_ppa, &gcw);

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force, int rg_id)
{
    struct line_mgmt *lm = &ssd->lm[rg_id];
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa, int rg_id)
{
    struct line_mgmt *lm = &ssd->lm[rg_id];
    struct line *line = get_line(ssd, ppa);

    line->ph = -1;
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

/* Reclaim one RU of RG rg_id; returns -1 if there is no eligible victim */
static int do_gc(struct ssd *ssd, bool force, int rg_id)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm[rg_id];
    struct nand_page *pg_iter = NULL;
    int start_ch, start_lun, end_ch, end_lun;
    struct ppa ppa;
    int ch, lun, cnt = 0;
    uint16_t pg;
    bool spill;
    int ph;
    char tbuf[26];
    struct tm tm;
    time_t t;

    victim_line = select_victim_line(ssd, force, rg_id);
    if (!victim_line) {
        return -1;
    }

    ph = victim_line->ph;
    ppa.g.blk = victim_line->id;

    rg2physical(spp, rg_id, &start_ch, &start_lun, &end_ch, &end_lun);

    /* Per-GC log line (GC time series); keep its format */
    time(&t);
    localtime_r(&t, &tm);
    printf("[%s]GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d,stream_id=%d,rg_id=%d, discard %lu read/write %lu/%lu block_erased %lu copied %lu\n",
           asctime_r(&tm, tbuf), ppa.g.blk,
           victim_line->ipc, lm->victim_line_cnt, lm->full_line_cnt,
           lm->free_line_cnt, ph, rg_id,
           ssd->stats.discard, ssd->stats.read_io_n, ssd->stats.write_io_n,
           ssd->stats.block_erased, ssd->stats.copied);

    /*
     * A victim with less than 10% invalid pages (only forced GC picks one)
     * is copied out of its RG. NOTE: see KNOWN_ISSUES (FEMU-L2).
     */
    spill = (victim_line->ipc < (spp->pgs_per_line / 10));

    /* copy back valid data: page by page, striped like the host writes */
    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
        for (ch = start_ch; ch <= end_ch; ch++) {
            for (lun = start_lun; lun <= end_lun; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                ppa.g.pg = pg;
                pg_iter = get_pg(ssd, &ppa);
                if (pg_iter->status == PG_VALID) {
                    gc_read_page(ssd, &ppa);
                    /* delay the maptbl update until "write" happens */
                    gc_write_page(ssd, &ppa, ph, rg_id, spill);
                    cnt++;
                }
                pg_iter->status = PG_FREE;
            }
        }
    }
    ssd->stats.copied += cnt;

    /* erase every block of the RU */
    for (ch = start_ch; ch <= end_ch; ch++) {
        for (lun = start_lun; lun <= end_lun; lun++) {
            struct nand_block *blk;
            struct nand_cmd gce;

            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            blk = get_blk(ssd, &ppa);
            blk->ipc = 0;
            blk->vpc = 0;
            blk->erase_cnt++;
            ssd->stats.block_erased++;

            gce.type = GC_IO;
            gce.cmd = NAND_ERASE;
            gce.stime = 0;
            ssd_advance_status(ssd, &ppa, &gce);
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa, rg_id);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    /* one LBA is one page and req->nlb is 0-based: end_lpn is inclusive */
    uint64_t start_lpn = lba;
    uint64_t end_lpn = (start_lpn + nsecs);
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    ssd->stats.read_io_n++;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ssd->stats.read_sum++;
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

/*
 * I/O Management Receive, MO 1: Reclaim Unit Handle Status. One descriptor
 * per (ph, rg) pair, ph-major, with PID == RUHID == rg * handle_number + ph.
 * All other fields, including the reserved header bytes, are zero.
 */
static uint64_t fdpssd_ruh_status(struct ssd *ssd, NvmeRequest *req, size_t len)
{
    unsigned int nruhsd = ssd->handle_number * ssd->rg_number;
    uint64_t prp1 = le64_to_cpu(req->cmd.dptr.prp1);
    uint64_t prp2 = le64_to_cpu(req->cmd.dptr.prp2);
    size_t trans_len = sizeof(NvmeRuhStatus) +
                       nruhsd * sizeof(NvmeRuhStatusDescr);
    NvmeRuhStatus *hdr;
    NvmeRuhStatusDescr *ruhsd;
    uint8_t *buf;
    uint8_t ph, rg;

    buf = g_malloc0(trans_len);
    trans_len = MIN(trans_len, len);

    hdr = (NvmeRuhStatus *)buf;
    ruhsd = (NvmeRuhStatusDescr *)(buf + sizeof(NvmeRuhStatus));

    hdr->nruhsd = cpu_to_le16(nruhsd);

    for (ph = 0; ph < ssd->handle_number; ph++) {
        for (rg = 0; rg < ssd->rg_number; rg++, ruhsd++) {
            ruhsd->earutr = 0;
            ruhsd->ruamw = 0;
            ruhsd->pid = cpu_to_le16((uint16_t)(rg * ssd->handle_number + ph));
            ruhsd->ruhid = cpu_to_le16((uint16_t)(rg * ssd->handle_number + ph));
        }
    }

    dma_read_prp(ssd->femu, buf, trans_len, prp1, prp2);
    g_free(buf);
    return 0;
}

/*
 * Per-RU part of the FEMU reset dump: every RU of RG rg as it is before the
 * reset, i.e. the placement handle that wrote it ("stream id", -1 if the RU
 * is free), its invalid and valid page counts and its position in the victim
 * queue (0 if it is not queued). Measurement output: keep the format.
 */
static void fdpssd_print_rus(struct ssd *ssd, int rg)
{
    struct line_mgmt *lm = &ssd->lm[rg];
    struct line *line;
    int i;

    printf("\nRG %u-----------------------------\n", (unsigned int)rg);
    for (i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        printf("[%d] stream id=%d ipc=%d vpc=%d pos=%lu\n", i, line->ph,
               line->ipc, line->vpc, line->pos);
    }
    printf("------------------------------------\n");
}

/*
 * I/O Management Send, MO 2 (FEMU reset): print the measurement dump and
 * reset the FTL to an empty device. The dump starts with a banner line and the
 * RU geometry, then lists the RUs of every RG, the dies of every RG, the host
 * pages written through every RUH and the FTL counters. WAF is computed
 * offline from it: the per-RUH "written" lines
 * (host pages) and the "copied" counter (GC pages). The format of every
 * printed line is relied upon by the experiment scripts and must not change.
 */
static uint64_t fdpssd_femu_reset(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    int start_ch, start_lun, end_ch, end_lun;
    /* NOTE: never accumulated, always reported as 0; see KNOWN_ISSUES (FEMU-8) */
    uint64_t sum_written = 0;
    char tbuf[26];
    struct tm tm;
    time_t t;
    int i, j;

    printf("msssd_io_mgmt_send_sungjin\n");
    printf("print_sungjin(spp->blks_per_line) : {%ld}\n", (long)spp->blks_per_line);
    printf("print_sungjin(spp->pgs_per_line) : {%ld}\n", (long)spp->pgs_per_line);

    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp, false);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd, false);

    /* initialize rmap */
    ssd_init_rmap(ssd, false);

    /* dump the RUs of every RG, then rebuild its RU lists with every RU free */
    for (i = 0; i < ssd->rg_number; i++) {
        fdpssd_print_rus(ssd, i);
        fdpssd_init_lines(ssd, i, false);
    }

    for (i = 0; i < ssd->rg_number; i++) {
        rg2physical(spp, i, &start_ch, &start_lun, &end_ch, &end_lun);
        printf("RG %d channel %d-%d / lun %d-%d\n", i, start_ch, end_ch,
               start_lun, end_lun);
    }

    /* prints and clears the per-RUH "written" counters */
    for (i = 0; i < ssd->handle_number; i++) {
        for (j = 0; j < ssd->rg_number; j++) {
            fdpssd_init_write_pointer(ssd, i, j);
        }
    }

    printf("print_sungjin(ssd->sungjin_stat.block_erased) : {%ld}\n",
           (long)ssd->stats.block_erased);
    printf("print_sungjin(ssd->sungjin_stat.copied) : {%ld}\n",
           (long)ssd->stats.copied);
    printf("print_sungjin(ssd->sungjin_stat.discard) : {%ld}\n",
           (long)ssd->stats.discard);
    printf("print_sungjin(ssd->sungjin_stat.discard_ignored) : {%ld}\n",
           (long)ssd->stats.discard_ignored);
    printf("print_sungjin(ssd->sungjin_stat.invalidated) : {%ld}\n",
           (long)ssd->stats.invalidated);
    printf("print_sungjin(sum_written) : {%ld}\n", (long)sum_written);
    printf("print_sungjin(ssd->sungjin_stat.write_io_n) : {%ld}\n",
           (long)ssd->stats.write_io_n);
    printf("print_sungjin(ssd->sungjin_stat.read_io_n) : {%ld}\n",
           (long)ssd->stats.read_io_n);
    printf("print_sungjin(ssd->sungjin_stat.read_sum) : {%ld}\n",
           (long)ssd->stats.read_sum);

    memset(&ssd->stats, 0, sizeof(ssd->stats));

    time(&t);
    localtime_r(&t, &tm);
    printf("INITIALIZED current time: %s", asctime_r(&tm, tbuf));

    return 0;
}

static uint64_t fdpssd_io_mgmt_send(struct ssd *ssd, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint8_t mo = (cdw10 & 0xff);

    switch (mo) {
    case NVME_IOMS_MO_NOP:
        return 0;
    case NVME_IOMS_MO_RUH_UPDATE:
        /* accepted, but handles are never moved to a new RU */
        return NVME_SUCCESS;
    case NVME_IOMS_MO_FEMU_RESET:
        return fdpssd_femu_reset(ssd);
    default:
        /* NOTE: status returned as latency; see KNOWN_ISSUES (FEMU-13) */
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static uint64_t fdpssd_io_mgmt_recv(struct ssd *ssd, NvmeRequest *req)
{
    NvmeCmd *cmd = &req->cmd;
    uint32_t cdw10 = le32_to_cpu(cmd->cdw10);
    uint32_t numd = le32_to_cpu(cmd->cdw11);
    uint8_t mo = (cdw10 & 0xff);
    size_t len = (numd + 1) << 2;

    switch (mo) {
    case NVME_IOMR_MO_NOP:
        return 0;
    case NVME_IOMR_MO_RUH_STATUS:
        return fdpssd_ruh_status(ssd, req, len);
    default:
        /* NOTE: status returned as latency; see KNOWN_ISSUES (FEMU-13) */
        return NVME_INVALID_FIELD | NVME_DNR;
    }
}

/* Dataset Management (deallocate): unmap the LPNs of every range */
static uint64_t fdpssd_trim(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    NvmeDsmCmd *dsm = (NvmeDsmCmd *)&req->cmd;
    uint32_t attr = le32_to_cpu(dsm->attributes);
    uint16_t nr = (le32_to_cpu(dsm->nr) & 0xff) + 1;
    NvmeDsmRange *range;
    struct ppa ppa;
    uint64_t slpn;
    uint32_t nlp;
    int i, j;

    if (!(attr & NVME_DSMGMT_AD)) {
        return 0;
    }

    /* range list handed over (and owned) by nvme_dsm() */
    range = (NvmeDsmRange *)(uintptr_t)req->cmd.dsm_ranges;
    for (i = 0; i < nr; i++) {
        slpn = le64_to_cpu(range[i].slba) / spp->secs_per_pg;
        /* NOTE: nlb + 1 LBAs per range; see KNOWN_ISSUES (FEMU-2) */
        nlp = (le32_to_cpu(range[i].nlb) + 1) / spp->secs_per_pg;

        for (j = 0; j < nlp; j++) {
            ppa = get_maptbl_ent(ssd, slpn + j);
            if (!valid_ppa(ssd, &ppa)) {
                ssd->stats.discard_ignored++;
                continue;
            }

            if (mapped_ppa(&ppa)) {
                ssd->stats.discard++;
                mark_page_invalid(ssd, &ppa);
                set_rmap_ent(ssd, INVALID_LPN, &ppa);
                ppa.ppa = UNMAPPED_PPA;
                set_maptbl_ent(ssd, slpn + j, &ppa);
            }
        }
    }
    g_free(range);

    return 0;
}

static uint64_t fdpssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    /* one LBA is one page and req->nlb is 0-based: end_lpn is inclusive */
    uint64_t start_lpn = lba;
    uint64_t end_lpn = (start_lpn + len);
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint16_t pid = le16_to_cpu(rw->dspec);
    /* flat PID space, see fdpftl.h */
    uint8_t ph = pid % ssd->handle_number;
    uint8_t rg_id = pid / ssd->handle_number;
    struct write_pointer *wpp;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int i, r;

    ssd->stats.write_io_n++;

    if (pid >= ssd->handle_number * ssd->rg_number) {
        /*
         * Not a PID of this device: the 8-bit RG index wraps for PIDs from
         * 256 * handle_number on, and RG indexes past the last RG are folded
         * into the last RG. NOTE: see KNOWN_ISSUES (FEMU-L3).
         */
        if (rg_id >= ssd->rg_number) {
            rg_id = ssd->rg_number - 1;
        }
        if (!ssd->pid_warned) {
            ftl_err("out-of-range PID %u (%d PIDs) placed in RG %u, handle %u "
                    "(reported once)\n", pid,
                    ssd->handle_number * ssd->rg_number, rg_id, ph);
            ssd->pid_warned = true;
        }
    }

    /*
     * Host pages are credited to the requested RUH, even if the write is
     * redirected to another RG below. NOTE: see KNOWN_ISSUES (FEMU-L4).
     */
    wpp = &ssd->wp[ph][rg_id];
    wpp->written += (len + 1);

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* foreground GC: at most 128 forced GCs while at the high watermark */
    for (i = 0; i < 128 && should_gc_high(ssd, rg_id); i++) {
        r = do_gc(ssd, true, rg_id);
        if (r == -1) {
            break;
        }
    }
    if (should_gc_high(ssd, rg_id)) {
        /* still at the watermark: redirect the whole write, see fdp_spill_rg() */
        rg_id = fdp_spill_rg(ssd);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            ssd->stats.invalidated++;
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd, ph, rg_id);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, ph, rg_id);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static void *fdpftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i, rg;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = fdpssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = fdpssd_trim(ssd, req);
                break;
            case NVME_CMD_IO_MGMT_RECV:
                lat = fdpssd_io_mgmt_recv(ssd, req);
                break;
            case NVME_CMD_IO_MGMT_SEND:
                lat = fdpssd_io_mgmt_send(ssd, req);
                break;
            default:
                /* NOTE: lat keeps its previous value; see KNOWN_ISSUES (FEMU-13) */
                break;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one RU per RG if needed (in the background) */
            for (rg = 0; rg < ssd->rg_number; rg++) {
                if (should_gc(ssd, rg)) {
                    do_gc(ssd, false, rg);
                }
            }

            /*
             * The scan resumes at ring rg_number + 1, not at the next ring.
             * NOTE: see KNOWN_ISSUES (FEMU-1).
             */
            i = ssd->rg_number;
        }
    }

    return NULL;
}
