#include "ftl.h"

//#define FEMU_DEBUG_FTL ver 2R++
// 쓰기 1
// 카피백 2
// 페이지가 2나 3에 있으면 3에 카피백

// 1과 2는 normal region
// 3은 cold region

uint64_t write_count = 0;
uint64_t gc_write_count = 0;
uint64_t prev_write = 0;
uint64_t gc_prev_write = 0;
uint64_t false_write = 0, init_write = 0, nf_write = 0, mf_write = 0;
uint64_t cp_cnt = 0, ncp_cnt = 0, rcp_cnt = 0;

static void write_stats_to_file(struct ssd *ssd)
{
    char str[4096] = {0};
    bool may_write = false;
    int fd, cnt;
    struct ssdparams *spp = &ssd->sp;

    if (gc_write_count != gc_prev_write && write_count > (prev_write + 20000))
        may_write = true;

    if (write_count % 3000000 == 0 && write_count != 0 && write_count != prev_write)
        may_write = true;

    if (!may_write)
        return;

    if (write_count % 6000000 == 0 && write_count != 0 && write_count != prev_write){
        /*struct tr_mgmt *tr_info = &ssd->tr_info;
        struct vlist *i = tr_info->head;
        uint64_t normal_vpc = 0, cold_vpc = 0, normal_line_cnt = 0, cold_line_cnt = 0;
        printf("write : %ld\n", write_count);
        while(i != NULL){
            if(i->vline->cold == 1 || i->vline->cold == 2){
                normal_line_cnt++;
                normal_vpc += i->vline->vpc;
            }
            else if(i->vline->cold == 3){
                cold_line_cnt++;
                cold_vpc += i->vline->vpc;
            }
            i = i->next;
        }
        printf("%ld %ld %ld %ld\n", normal_line_cnt, normal_vpc, cold_line_cnt, cold_vpc);*/

        if ((write_count % 30000000 == 0 || write_count == 54000000) && write_count != 0){
            //printf("false write : %ld %ld %ld %ld\ncopyback : %ld %ld %ld %ld\n", init_write, nf_write, mf_write, false_write, gc_write_count, ncp_cnt, cp_cnt, rcp_cnt);
            struct line_mgmt *lm = &ssd->lm;
            struct line *line;

            printf("util\n\n");

            for (int i = 0; i < lm->tt_lines; i++) {
                line = &lm->lines[i];
                double util = (double) line->vpc / (double) spp->pgs_per_line;
                printf("%lf\n", util);
            }
        }
    }

    cnt = sprintf(str, "%ld %ld %f\n",
                write_count,
                gc_write_count,
                (double)((double)(write_count + gc_write_count) / (double)write_count));

    if (cnt <= 0) {
        printf("[FEMU] %s:%d sprintf error cnt %d\n",
	       __func__, __LINE__, cnt);
        return;
    }

    fd = open("femu_stat_2r_owp.txt", O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        printf("[FEMU] %s:%d open error fd %d\n",
	       __func__, __LINE__, fd);
        return;
    }

    cnt = write(fd, str, cnt);
    if (cnt <= 0)
	    printf("[FEMU] %s:%d write cnt error %d\n",
		   __func__, __LINE__, cnt);

    close(fd);

    prev_write = write_count;
    gc_prev_write = gc_write_count;

    return;
}

static void *ftl_thread(void *arg);

static int do_gc(struct ssd *ssd, bool force);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
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

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct tr_mgmt *tr_info = &ssd->tr_info;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

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
        line->cold = 1;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;

    tr_info->head = NULL;
    tr_info->tail = NULL;
    tr_info->fifo_head = NULL;
    tr_info->list_threshold = 0.8;
    tr_info->cold_valid_ratio = 0.5;
    tr_info->frozen_valid_ratio = 0.7;
    tr_info->count = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct write_pointer *cpp = &ssd->cp;
    struct write_pointer *fpp = &ssd->fp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;

    cpp->curline = NULL;
    cpp->ch = 0;
    cpp->lun = 0;
    cpp->pg = 0;
    cpp->blk = 0;
    cpp->pl = 0;

    fpp->curline = NULL;
    fpp->ch = 0;
    fpp->lun = 0;
    fpp->pg = 0;
    fpp->blk = 0;
    fpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        struct tr_mgmt *tr_info = &ssd->tr_info;
        struct vlist *i = tr_info->head;
        struct line_mgmt *lm = &ssd->lm;
        
        printf("\nvictim\n");
        while(i != NULL){
            printf("%d %d %d\n", i->vline->id, i->vline->cold, i->vline->vpc);
            i = i->next;
        }

        line *ab = lm->free_line_list.tqh_first;
        printf("\nfree\n");
        while(ab != NULL){
            printf("%d %d %d",ab->id, ab->cold, ab->vpc);
            ab = ab->entry.tqe_next;
        }

        if(ssd->cp.curline != NULL){
            printf("\nc : %d %d %d\n", ssd->cp.curline->id, ssd->cp.curline->cold, ssd->cp.curline->vpc);
        }

        if(ssd->wp.curline != NULL){
            printf("w : %d %d %d\n", ssd->wp.curline->id, ssd->wp.curline->cold, ssd->wp.curline->vpc);
        }

        if(ssd->fp.curline != NULL){
            printf("f : %d %d %d\n", ssd->fp.curline->id, ssd->fp.curline->cold, ssd->fp.curline->vpc);
        }

        if(tr_info->fifo_head != NULL){
            printf("fd : %d %d %d\n", tr_info->fifo_head->vline->id, tr_info->fifo_head->vline->cold, tr_info->fifo_head->vline->vpc);
        }

        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, int cold)
{
    struct ssdparams *spp = &ssd->sp;
    struct tr_mgmt *tr_info = &ssd->tr_info;
    struct write_pointer *wpp = &ssd->wp;
    int r = 0;
    int block_cold = 1;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                struct vlist *victim = g_malloc0(sizeof(struct vlist));
                victim->vline = wpp->curline;

                if(tr_info->head == NULL && tr_info->tail == NULL){
                    tr_info->head = victim;
                    tr_info->tail = victim;
                    tr_info->fifo_head = victim;
                    victim->next = NULL;
                    victim->prev = NULL;
                }
                else{
                    victim->next = NULL;
                    victim->prev = tr_info->tail;
                    tr_info->tail->next = victim;
                    tr_info->tail = victim;
                }

                tr_info->count++;

                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                wpp->curline->cold = block_cold;
                if (!wpp->curline){
                    /* TODO */
                    abort();
                }

                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);

                while (should_gc_high(ssd)) {
                    /* perform GC here until !should_gc(ssd) */
                    r = do_gc(ssd, true);
                    if (r == -1)
                        break;
                }
            }
        }
    }
}

static void ssd_advance_write_pointer_gc(struct ssd *ssd, int cold)
{
    struct ssdparams *spp = &ssd->sp;
    struct tr_mgmt *tr_info = &ssd->tr_info;
    struct write_pointer *wpp = &ssd->cp;
    int block_cold = 2;

    if(cold == 3 || cold == 2){
        wpp = &ssd->fp;
        block_cold = 3;
    }

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                struct vlist *victim = g_malloc0(sizeof(struct vlist));
                victim->vline = wpp->curline;

                if(tr_info->head == NULL && tr_info->tail == NULL){
                    tr_info->head = victim;
                    tr_info->tail = victim;
                    tr_info->fifo_head = victim;
                    victim->next = NULL;
                    victim->prev = NULL;
                }
                else{
                    victim->next = NULL;
                    victim->prev = tr_info->tail;
                    tr_info->tail->next = victim;
                    tr_info->tail = victim;
                }

                tr_info->count++;

                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                //printf("ssd_advance_write_pointer_gc\n");
                wpp->curline = get_next_free_line(ssd);
                wpp->curline->cold = block_cold;
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd, int cold)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    int block_cold = 1;

    if(wpp->curline == NULL){
        //printf("get_new_page_gc\n");
        wpp->curline = get_next_free_line(ssd);
        if (!wpp->curline) {
            /* TODO */
            abort();
        }
        wpp->curline->cold = block_cold;
        wpp->blk = wpp->curline->id;
        check_addr(wpp->blk, ssd->sp.blks_per_pl);
        /* make sure we are starting from page 0 in the super block */
        ftl_assert(wpp->pg == 0);
        ftl_assert(wpp->lun == 0);
        ftl_assert(wpp->ch == 0);
        /* TODO: assume # of pl_per_lun is 1, fix later */
        ftl_assert(wpp->pl == 0);
    }

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static struct ppa get_new_page_gc(struct ssd *ssd, int cold)
{
    struct write_pointer *wpp = &ssd->cp;
    struct ppa ppa;
    int block_cold = 2;

    if(cold == 3 || cold == 2){
        wpp = &ssd->fp;
        block_cold = 3;
    }

    if(cold == 1){
        ncp_cnt++;
    }
    else if(cold == 2){
        cp_cnt++;
    }
    else if(cold == 3){
        rcp_cnt++;
    }

    if(wpp->curline == NULL){
        //printf("get_new_page_gc\n");
        wpp->curline = get_next_free_line(ssd);
        if (!wpp->curline) {
            /* TODO */
            abort();
        }
        wpp->curline->cold = block_cold;
        wpp->blk = wpp->curline->id;
        check_addr(wpp->blk, ssd->sp.blks_per_pl);
        /* make sure we are starting from page 0 in the super block */
        ftl_assert(wpp->pg == 0);
        ftl_assert(wpp->lun == 0);
        ftl_assert(wpp->ch == 0);
        /* TODO: assume # of pl_per_lun is 1, fix later */
        ftl_assert(wpp->pl == 0);
    }

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 16; //pgs_per_line == 64 * pgs_per_blk  144 : 9216, 72 : 4608,  36 : 2304,  18 : 1152
    spp->blks_per_pl = 2816; /* == tt_lines, 256 : 16GB          256         512         1024        2048*/
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

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

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_lines_high = 2;
    spp->enable_gc_delay = true;

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);
    /* initialize rmap */
    ssd_init_rmap(ssd);
    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
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
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
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

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    //struct line_mgmt *lm = &ssd->lm;
    //struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    //bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    /*if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }*/
    line->ipc++;
    ftl_assert(line->vpc >= 0 && line->vpc <= spp->pgs_per_line);
    line->vpc--;
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

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int cold)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page_gc(ssd, cold);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer_gc(ssd, cold);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);

        gc_write_count++;
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct vlist *select_victim_line(struct ssd *ssd, bool force){
    struct ssdparams *spp = &ssd->sp;
    struct tr_mgmt *tr_info = &ssd->tr_info;
    struct write_pointer *cpp = &ssd->cp;
    struct write_pointer *fpp = &ssd->fp;

    struct vlist *temp = tr_info->head;
    struct vlist *i = tr_info->fifo_head;
    struct vlist *v_line = NULL;
    struct vlist *inext = NULL;

    int j = 0, k = 0;
    int valid_sum, is_cold_merge;
    int fifo_tail = -1, fth = 0, count = 0;
    int was_cold = 0, was_frozen = 0;
    int threshold = (int)((double)tr_info->count * tr_info->list_threshold);
    int vlist[260] = {0, };
    
    double cold_block_valid_ratio = tr_info->cold_valid_ratio;
    double frozen_block_valid_ratio = tr_info->frozen_valid_ratio;

    if(tr_info->fifo_head == NULL){
        tr_info->fifo_head = tr_info->head;
        i = tr_info->head;
    }

    while(temp->next != NULL){
        if(count == threshold){
            fifo_tail = temp->vline->id;
            break;
        }

        if(temp->vline->id == tr_info->fifo_head->vline->id){
            fth = 1;
        }

        temp = temp->next;
        count++;
    }

    if(fifo_tail == -1){
        printf("fifo fail %d %d\n", tr_info->count, count);
        fifo_tail = tr_info->tail->vline->id;
    }

    if(fth == 0){
        i = tr_info->head;
    }

    while(1){
        
        if (cold_block_valid_ratio > 0.9) {
            cold_block_valid_ratio = 0.5;
        }

        if (frozen_block_valid_ratio > 0.94) {
            frozen_block_valid_ratio = 0.7;
        }

        int out = 0;

        while(1){
            char cold_condition = (double) i->vline->vpc / (double) spp->pgs_per_line <= cold_block_valid_ratio;
            char frozen_condition = (double) i->vline->vpc / (double) spp->pgs_per_line <= frozen_block_valid_ratio;

            if(was_cold == 1){
                cold_condition = 0;
            }

            if(was_frozen == 1){
                frozen_condition = 0;
            }

            if ((i->vline->cold == 3 && frozen_condition) || (i->vline->cold == 2 && cold_condition) || (i->vline->cold == 1 && cold_condition)) {
                if(i->vline->vpc != spp->pgs_per_line){
                    vlist[0] = i->vline->id;
                    break;
                }
            }

            i = i->next;

            if(i == NULL){
                printf("i is null\n");
            }

            if(i == NULL || i->vline->id == fifo_tail){
                if(cold_block_valid_ratio < 1){
                    cold_block_valid_ratio += 0.02;
                }

                if(frozen_block_valid_ratio < 1){
                    frozen_block_valid_ratio += 0.02;
                }

                if(frozen_block_valid_ratio > 0.9 || cold_block_valid_ratio > 0.9){
                    fifo_tail = tr_info->tail->vline->id;
                }

                if(frozen_block_valid_ratio > 0.98 && cold_block_valid_ratio > 0.98){
                    printf("first victim selection fail!\n");
                    return NULL;
                }

                i = tr_info->head;
            }
        }//첫 번째 빅팀 선택 반복문

        j = 1;
        valid_sum = i->vline->vpc;
        is_cold_merge = i->vline->cold;

        int write_point = 0;

        if(is_cold_merge == 2){
            is_cold_merge = 1;
        }

        if(is_cold_merge == 3){
            if(fpp->curline != NULL){
                valid_sum += fpp->ch + fpp->lun * spp->nchs + fpp->pg * spp->nchs * spp->luns_per_ch;
                write_point = 1;
            }
        }
        else{
            if(cpp->curline != NULL){
                valid_sum += cpp->ch + cpp->lun * spp->nchs + cpp->pg * spp->nchs * spp->luns_per_ch;
                write_point = 1;
            }
        }

        while(1){
            char is_exist_already = 0;
            int tmp_cold = 0;
            for(int k = 0; k < j; k++){
                if(i->vline->id == vlist[k]){
                    is_exist_already = 1;
                    break;
                }
            }

            if(is_exist_already != 1){
                tmp_cold = i->vline->cold;

                if(tmp_cold == 2){
                    tmp_cold = 1;
                }

                char cold_condition = (double) i->vline->vpc / (double) spp->pgs_per_line <= cold_block_valid_ratio;
                char frozen_condition = (double) i->vline->vpc / (double) spp->pgs_per_line <= frozen_block_valid_ratio;

                if ((is_cold_merge == 3 && frozen_condition) || (is_cold_merge == 1 && cold_condition)) {
                    if(tmp_cold == is_cold_merge && i->vline->vpc != spp->pgs_per_line){
                        valid_sum += i->vline->vpc;
                        vlist[j++] = i->vline->id;

                        if(write_point == 1){
                            if (valid_sum <= j * spp->pgs_per_line) {
                                out = 1;
                                break;
                            }
                        }
                        else{
                            if (valid_sum <= (j - 1) * spp->pgs_per_line) {
                                out = 1;
                                break;
                            }
                        }
                    }
                }
            }

            i = i->next;

            if(i == NULL){
                printf("i is null1\n");
            }

            if(i == NULL || i->vline->id == fifo_tail){
                if(is_cold_merge == 1 && cold_block_valid_ratio < 1){
                    cold_block_valid_ratio += 0.02;
                }

                if(is_cold_merge == 3 && frozen_block_valid_ratio < 1){
                    frozen_block_valid_ratio += 0.02;
                }

                if(cold_block_valid_ratio > 0.9 || frozen_block_valid_ratio > 0.9){
                    fifo_tail = tr_info->tail->vline->id;
                }

                if((is_cold_merge == 1 && cold_block_valid_ratio > 0.98) || (is_cold_merge == 3 && frozen_block_valid_ratio > 0.98)){
                    if(was_cold + was_frozen == 1){
                        printf("multi victim selection fail!\n");
                        return NULL;
                    }
                    else{
                        i = tr_info->head;
                        break;
                    }
                }

                i = tr_info->head;
            }
        }//멀티 빅팀

        if(out == 1){
            tr_info->fifo_head = i;
            break;
        }
        else{
            if(is_cold_merge == 1){
                was_cold = 1;
            }
            else{
                was_frozen = 1;
            }
        }
    }//전체 빅팀 선택 반복문

    tr_info->cold_valid_ratio = cold_block_valid_ratio;
    tr_info->frozen_valid_ratio = frozen_block_valid_ratio;

    i = tr_info->head;

    while(1){
        inext = i->next;
        for(k = 0; k < j; k++){
            if(i->vline->id == vlist[k]){
                if(tr_info->fifo_head->vline->id == i->vline->id){
                    if(i->next->vline->id == fifo_tail){
                        tr_info->fifo_head = tr_info->head;
                    }
                    else{
                        tr_info->fifo_head = i->next;
                    }
                }

                if(i->prev == NULL){
                    i->next->prev = NULL;
                    tr_info->head = i->next;
                }
                else{
                    i->prev->next = i->next;
                    i->next->prev = i->prev;
                }

                if(v_line == NULL){
                    v_line = i;
                    v_line->next = NULL;
                    v_line->prev = NULL;
                }
                else{
                    v_line->prev = i;
                    i->next = v_line;
                    v_line = i;
                    v_line->prev = NULL;
                }

                break;
            }
        }

        i = inext;
        if(i->vline->id == fifo_tail){
            break;
        }
    }//빅팀 정리

    tr_info->count -= j;

    return v_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, int cold)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa, cold);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    line->cold = 1;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force)
{
    //struct line *victim_line = NULL;
    struct vlist *victim_list = NULL;
    struct vlist *delete_list = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun, cold;

    victim_list = select_victim_line(ssd, force);
    if (victim_list == NULL) {
        return -1;
    }

    while(1){
        delete_list = victim_list;
        ppa.g.blk = victim_list->vline->id;
        cold = victim_list->vline->cold;
        ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
                victim_list->vline->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
                ssd->lm.free_line_cnt);

        /* copy back valid data */
        for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);
                clean_one_block(ssd, &ppa, cold);
                mark_block_free(ssd, &ppa);
                

                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    ssd_advance_status(ssd, &ppa, &gce);
                }

                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }

        /* update line status */
        mark_line_free(ssd, &ppa);

        if(victim_list->next != NULL){
            victim_list = victim_list->next;
            victim_list->prev->next = NULL;
            victim_list->prev = NULL;
            free(delete_list);
        }
        else{
            free(delete_list);
            break;
        }
    }

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
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

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    static line *line;
    int cold = 2;
    //int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        cold = 2;
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            line = get_line(ssd, &ppa);
            cold = line->cold;

            if(cold == 3){
                false_write++;
            }
            else if(cold == 2){
                mf_write++;
            }
            else if(cold == 1){
                nf_write++;
            }

            if(get_rmap_ent(ssd, &ppa) != lpn){
                printf("neq %d %ld %ld\n", cold, get_rmap_ent(ssd, &ppa), lpn);
            }
            
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        else{
            init_write++;
        }

        /* new write */
        ppa = get_new_page(ssd, cold);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, cold);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;

        write_count++;
        if(write_count % 3000000 == 0)
            write_stats_to_file(ssd);

        if(write_count == 90000000)
            printf("test done\n");
    }

    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }
        }
        write_stats_to_file(ssd);
    }

    return NULL;
}