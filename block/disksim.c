/*
 * Block driver for DiskSim
 *
 * Copyright (c) 2012 José Orlando Pereira
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu-common.h"
#include "qemu-timer.h"
#include "block_int.h"
#include "module.h"

#include <pthread.h>
#include <disksim_interface.h>

typedef struct BDRVDiskSimState {
	struct disksim_interface *disksim;
	double zero;
	disksim_interface_callback_t cb;
	double next;

	pthread_mutex_t mtx;
	pthread_cond_t cond;
	pthread_t thr;
} BDRVDiskSimState;

typedef struct BDRVDiskSimReq {
	struct disksim_request sr;

	BlockDriverState *bs;

	BlockDriverCompletionFunc *cb;
	void *opaque;

	int status;
	
	int ret;
	int64_t deadline;
} BDRVDiskSimReq;

/* Request states */
#define DS_COMPLETE 1
#define DS_SCHEDULED 2
#define DS_READY (DS_COMPLETE|DS_SCHEDULED)

/* Convert between real and simulation time */
static double r2s_time(BDRVDiskSimState* s) {
	return ((double)qemu_get_clock_ns(rt_clock))/1e6-s->zero;
}

static int64_t s2r_time(double time, BDRVDiskSimState* s) {
	return (time+s->zero)*1e6;
}

/* Request finalization */
static void finalize_request(BDRVDiskSimReq* req) {
	if (req->status != DS_READY)
		return;

	// TODO schedule
	req->cb(req->opaque, req->ret);
	g_free(req);
}

/* Simulated device */
static void sim_schedule_callback(disksim_interface_callback_t cb, double time, void* ctx) {
	BlockDriverState* bs = ctx;
	BDRVDiskSimState* s = bs->opaque;

	/* No locking. This is called already within the lock. */
	s->next = time;
	s->cb = cb;
	pthread_cond_signal(&s->cond);
}

static void sim_deschedule_callback(double time, void *ctx) {
	BlockDriverState* bs = ctx;
	BDRVDiskSimState* s = bs->opaque;

	/* No locking. This is called already within the lock. */
	s->next = -1;
	s->cb = NULL;
	pthread_cond_signal(&s->cond);
}

static void sim_report_completion(double time, struct disksim_request *r, void *ctx) {
	BDRVDiskSimReq* req = (BDRVDiskSimReq*) r;
	
	req->deadline = s2r_time(time, req->bs->opaque);
	req->status |= DS_SCHEDULED;

	finalize_request(req);
}

static void* sim_thread(void* p) {
	BDRVDiskSimState* s = p;
	double time, delta;

	pthread_mutex_lock(&s->mtx);
	while(s->disksim != NULL) {
		time = r2s_time(s);

		/* Advance simulation time */

		while(s->next > 0 && s->next < time) {
			double event = s->next;
			s->next = -1;
			disksim_interface_internal_event(s->disksim, event, 0);
		}

		/* Advance real time */

		if (s->next < 0)
			/* Simulation is quiescent */
			pthread_cond_wait(&s->cond, &s->mtx);
		else {
			struct timespec ts;
			delta = s->next - time;

			if (delta > 0) {
				ts.tv_sec = (int) delta*1e-3;
				ts.tv_nsec = (long int) ((delta-ts.tv_sec*1e3)*1e6);
				pthread_cond_timedwait(&s->cond, &s->mtx, &ts);
			}
		}
	}

	return NULL;
}

/* Real device */
static void driver_report_completion(void *opaque, int ret) {
	BDRVDiskSimReq* req = opaque;
	BDRVDiskSimState* s = req->bs->opaque;

	pthread_mutex_lock(&s->mtx);
	req->ret = ret;
	req->status |= DS_COMPLETE;

	finalize_request(req);
	pthread_mutex_unlock(&s->mtx);
}

static int disksim_open(BlockDriverState *bs, const char *filename, int bdrv_flags) {
	BDRVDiskSimState* s = bs->opaque;
	const char *parv;
	char *stats, *file;
	int ret;

	if (strncmp(filename, "disksim:", strlen("disksim:")))
		return -EINVAL;
	parv = filename + strlen("disksim:");

	if (!(stats = strchr(parv, ':')))
		return -EINVAL;
	*(stats++) = 0;

	if (!(file = strchr(stats, ':')))
		return -EINVAL;
	*(file++) = 0;

	s->disksim = disksim_interface_initialize(parv, stats,
	        sim_report_completion, sim_schedule_callback, sim_deschedule_callback,
	        bs, 0, NULL);
	if (s->disksim == NULL)
		return -EINVAL;

	ret = bdrv_file_open(&bs->file, file, bdrv_flags);
	if (ret != 0)
		disksim_interface_shutdown(s->disksim, 0);

	s->zero = r2s_time(s);
	s->next = -1;

	pthread_mutex_init(&s->mtx, NULL);
	pthread_cond_init(&s->cond, NULL);
	pthread_create(&s->thr, NULL, sim_thread, s);

	return ret;
}

static void disksim_close(BlockDriverState *bs) {
	BDRVDiskSimState* s = bs->opaque;

	pthread_mutex_lock(&s->mtx);
	disksim_interface_shutdown(s->disksim, r2s_time(s));
	s->disksim = NULL;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->mtx);

	pthread_join(s->thr, NULL);
	pthread_mutex_destroy(&s->mtx);
	pthread_cond_destroy(&s->cond);
}

static BlockDriverAIOCB *disksim_aio_readv(BlockDriverState *bs, int64_t sector_num,
	QEMUIOVector *qiov, int nb_sectors, BlockDriverCompletionFunc *cb, void *opaque) {

	BDRVDiskSimState* s = bs->opaque;

	BDRVDiskSimReq* req = g_malloc0(sizeof(BDRVDiskSimReq));

	req->bs = bs;
	req->cb = cb;
	req->opaque = opaque;

	req->sr.start = r2s_time(s);
	req->sr.flags = DISKSIM_READ;
	req->sr.devno = 0;
	req->sr.bytecount = nb_sectors*512;
	req->sr.blkno = sector_num;

	pthread_mutex_lock(&s->mtx);
	disksim_interface_request_arrive(s->disksim, req->sr.start, &req->sr);
	pthread_mutex_unlock(&s->mtx);

	return bdrv_aio_readv(bs->file, sector_num, qiov, nb_sectors, driver_report_completion, req);
}

static BlockDriverAIOCB *disksim_aio_writev(BlockDriverState *bs, int64_t sector_num,
	QEMUIOVector *qiov, int nb_sectors, BlockDriverCompletionFunc *cb, void *opaque) {

	BDRVDiskSimState* s = bs->opaque;

	BDRVDiskSimReq* req = g_malloc0(sizeof(BDRVDiskSimReq));

	req->bs = bs;
	req->cb = cb;
	req->opaque = opaque;

	req->sr.start = r2s_time(s);
	req->sr.flags = DISKSIM_WRITE;
	req->sr.devno = 0;
	req->sr.bytecount = nb_sectors*512;
	req->sr.blkno = sector_num;

	pthread_mutex_lock(&s->mtx);
	disksim_interface_request_arrive(s->disksim, req->sr.start, &req->sr);
	pthread_mutex_unlock(&s->mtx);

	return bdrv_aio_writev(bs->file, sector_num, qiov, nb_sectors, driver_report_completion, req);
}

static int64_t disksim_getlength(BlockDriverState *bs) {
	return bdrv_getlength(bs->file);
}

static BlockDriver bdrv_disksim = {
    .format_name        = "disksim",
    .protocol_name      = "disksim",
    .instance_size      = sizeof(BDRVDiskSimState),

    .bdrv_file_open     = disksim_open,
    .bdrv_close         = disksim_close,

    .bdrv_aio_readv     = disksim_aio_readv,
    .bdrv_aio_writev    = disksim_aio_writev,

	.bdrv_getlength		= disksim_getlength,
};

static void bdrv_file_init(void)
{
    bdrv_register(&bdrv_disksim);
}

block_init(bdrv_file_init);
