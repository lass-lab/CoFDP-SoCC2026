#include "../nvme.h"
#include "./fdpftl.h"

static void fdp_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    /* same identify strings as the black-box SSD (guest visible, kept) */
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

static void fdp_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    fdp_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in FDP-SSD mode ...\n");
    fdpssd_init(n);
}

static void fdp_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

/*
 * DSM and I/O Management are handled in nvme_io_cmd() and executed by the
 * FTL thread; only reads and writes reach the extension.
 */
static uint16_t fdp_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t fdp_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        fdp_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static size_t sizeof_fdp_conf_descr(size_t nruh, size_t vss)
{
    size_t entry_siz = sizeof(NvmeFdpDescrHdr) + nruh * sizeof(NvmeRuhDescr)
                       + vss;
    return ROUND_UP(entry_siz, 8);
}

/*
 * FDP Configurations log page (LID 20h): a single configuration with
 * stream_number * rg_number persistently isolated reclaim unit handles.
 * NOTE: NRUH, FDPA (RGIF), RUNS and the log page offset do not describe the
 * emulated layout; see KNOWN_ISSUES (FEMU-12). mkfs.f2fs, the CoFDP tools and
 * the benchmarks read the Reclaim Unit Handle Status instead.
 */
static uint16_t fdp_log_confs(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint32_t dw12 = le32_to_cpu(cmd->cdw12);
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t numdl, numdu;
    uint64_t off, lpol, lpou;
    size_t buf_len;
    uint32_t log_size, trans_len;
    g_autofree uint8_t *buf = NULL;
    NvmeFdpConfsHdr *log;
    NvmeFdpDescrHdr *hdr;
    NvmeRuhDescr *ruhd;
    size_t nruh, fdp_descr_size;
    int i;

    numdl = (dw10 >> 16);
    numdu = (dw11 & 0xffff);
    lpol = dw12;
    lpou = dw13;

    buf_len = (((numdu << 16) | numdl) + 1) << 2;
    off = (lpou << 32ULL) | lpol;

    nruh = n->stream_number * n->rg_number;
    fdp_descr_size = sizeof_fdp_conf_descr(nruh, 0);
    log_size = sizeof(NvmeFdpConfsHdr) + fdp_descr_size;

    if (off >= log_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    trans_len = MIN(log_size - off, buf_len);

    buf = g_malloc0(log_size);
    log = (NvmeFdpConfsHdr *)buf;
    hdr = (NvmeFdpDescrHdr *)(log + 1);
    ruhd = (NvmeRuhDescr *)(buf + sizeof(*log) + sizeof(*hdr));

    log->num_confs = cpu_to_le16(0);
    log->size = cpu_to_le32(log_size);

    hdr->descr_size = cpu_to_le16(fdp_descr_size);
    hdr->fdpa = 0;
    hdr->nrg = cpu_to_le32(n->rg_number);
    hdr->nruh = cpu_to_le16(n->stream_number * n->rg_number);
    hdr->maxpids = cpu_to_le16(NVME_FDP_MAXPIDS - 1);
    hdr->nnss = cpu_to_le32(n->num_namespaces);
    hdr->runs = cpu_to_le64(spp->tt_blks);

    for (i = 0; i < nruh; i++) {
        ruhd->ruht = NVME_RUHT_PERSISTENTLY_ISOLATED;
        ruhd++;
    }

    dma_read_prp(n, (uint8_t *)buf, trans_len, prp1, prp2);
    return NVME_SUCCESS;
}

static uint16_t fdp_get_log(FemuCtrl *n, NvmeCmd *cmd)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint16_t lid = dw10 & 0xffff;

    switch (lid) {
    case NVME_LOG_FDP_CONFS:
        return fdp_log_confs(n, cmd);
    case NVME_LOG_FDP_RUH_USAGE:
    case NVME_LOG_FDP_STATS:
    case NVME_LOG_FDP_EVENTS:
        /* not emulated: succeed without transferring data */
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_LOG_ID | NVME_DNR;
    }
}

int nvme_register_fdpssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = fdp_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = fdp_admin_cmd,
        .io_cmd           = fdp_io_cmd,
        .get_log          = fdp_get_log,
    };

    return 0;
}
