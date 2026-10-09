#ifndef __FEMU_FDP_FTL_H
#define __FEMU_FDP_FTL_H

/*
 * FEMU FDP SSD model (femu_mode=6), the configurable FDP emulator of CoFDP.
 *
 * It is a copy of the black-box SSD FTL (bbssd/ftl.c) extended with NVMe
 * Flexible Data Placement:
 *
 * - Reclaim Group (RG): rg_number RGs partition the nchs x luns_per_ch dies.
 *   An RG covers several whole channels (rg_number < nchs), exactly one
 *   channel (rg_number == nchs) or luns_per_rg consecutive dies of one
 *   channel (rg_number > nchs). rg_number must be a power-of-two divisor of
 *   the die count; other values are not validated (see rg2physical()).
 * - Reclaim Unit (RU): a "line", i.e. the same block on every die of an RG.
 *   Every RG has its own line management lm[rg] (free list, victim queue and
 *   full list) and its own GC.
 * - Placement handle (ph): handle_number handles per RG. wp[ph][rg] is the
 *   write pointer of the RU that handle ph currently fills in RG rg.
 * - Placement ID (PID): carried in DSPEC (CDW13[31:16]) of a write; DTYPE is
 *   not checked. The PID space is flat: PID = rg * handle_number + ph, i.e.
 *   rg = PID / handle_number and ph = PID % handle_number. RUH Status reports
 *   exactly these PIDs.
 */

#include "../nvme.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

/* MAXPIDS reported in the FDP Configurations log is NVME_FDP_MAXPIDS - 1 */
#define NVME_FDP_MAXPIDS 256

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};

#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    /* reclaim group geometry, see rg2physical() */
    int luns_per_rg;  /* # of LUNs per RG */
    int chnls_per_rg; /* # of whole channels per RG (0: RG within a channel) */
    int rgs_per_chnl; /* # of RGs per channel (0: RG spans channels) */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay; /* set by FEMU_FLIP, but GC is always timed here */

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line; /* # of blocks per RU (= # of LUNs per RG) */
    int tt_lines;      /* # of RUs per RG */

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

/* A line is one reclaim unit (RU) of an RG */
typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
    int ph; /* placement handle that filled this RU, -1 while free */
} line;

/* wp: record next write addr of one (placement handle, RG) pair */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
    uint64_t written; /* host pages written via this RUH since the last reset */
};

/* FTL counters, printed and cleared by the FEMU reset (I/O Mgmt Send MO 2) */
struct fdp_stats {
    uint64_t copied;          /* valid pages copied by GC */
    uint64_t block_erased;    /* blocks erased by GC */
    uint64_t discard;         /* mapped pages deallocated by DSM */
    uint64_t discard_ignored; /* unmapped pages in DSM ranges */
    uint64_t invalidated;     /* pages invalidated by host overwrites */
    uint64_t write_io_n;      /* host write commands */
    uint64_t read_io_n;       /* host read commands */
    uint64_t read_sum;        /* LPNs looked up by host reads */
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer **wp; /* wp[ph][rg] */
    struct line_mgmt *lm;      /* lm[rg] */
    uint8_t rg_number;         /* # of reclaim groups */
    uint8_t handle_number;     /* # of placement handles per RG */
    bool pid_warned;           /* an out-of-range PID has been reported */
    FemuCtrl *femu;
    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
    struct fdp_stats stats;
};

void fdpssd_init(FemuCtrl *n);

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
