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

#include <disksim_interface.h>

typedef struct BDRVDiskSimState {
	struct disksim_interface *disksim;
} BDRVDiskSimState;


static void sim_schedule_callback(disksim_interface_callback_t cb, double time, void* ctx) {
}

static void sim_deschedule_callback(double time, void *ctx) {
}

static void sim_report_completion(double time, struct disksim_request *r, void *ctx) {
}

static int disksim_open(BlockDriverState *bs, const char *filename, int bdrv_flags) {
	BDRVDiskSimState* s = bs->opaque;
	char *parv, *stats, *file;
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

	return ret;
}

static void disksim_close(BlockDriverState *bs) {
}

static BlockDriverAIOCB *disksim_aio_readv(BlockDriverState *bs, int64_t sector_num,
	QEMUIOVector *qiov, int nb_sectors, BlockDriverCompletionFunc *cb, void *opaque) {

	return bdrv_aio_readv(bs->file, sector_num, qiov, nb_sectors, cb, opaque);
}

static BlockDriverAIOCB *disksim_aio_writev(BlockDriverState *bs, int64_t sector_num,
	QEMUIOVector *qiov, int nb_sectors, BlockDriverCompletionFunc *cb, void *opaque) {

	return bdrv_aio_writev(bs->file, sector_num, qiov, nb_sectors, cb, opaque);
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
