/* SPDX-License-Identifier: GPL-2.0 */
/*
 * iqnet: PL streamer path (iqnet_pl.c), interface to iqnet_main.c.
 *
 * Internal to iqnet.ko, nothing here is exported. The main module owns the
 * "one stream per board" decision (under its iqnet.lock) and calls in here
 * for everything that touches the streamer.
 */
#ifndef _IQNET_PL_H
#define _IQNET_PL_H

#include <linux/types.h>

struct file;
struct iqnet_pl_start;

/* Streamer counters, from one snapshot. */
struct iqnet_pl_counters {
	u64 datagrams;
	u64 bytes;
	u64 overflows;
	u64 linux_frames;
	u64 linux_drops;
	u64 fifo_hwm_bytes;
};

int iqnet_pl_init(void);
void iqnet_pl_exit(void);

/* A streamer was found in the loaded bitstream and the driver is bound. */
bool iqnet_pl_present(void);
/* A PL stream is armed. Only START (under iqnet.lock) can make it true. */
bool iqnet_pl_active(void);

/* Called with iqnet.lock held and no kernel-path stream or zombie. */
int iqnet_pl_start(const struct iqnet_pl_start *req, struct file *owner);
/*
 * Stop the PL stream if one is active and @owner is NULL or started it.
 * Returns 0 (also when there was nothing to stop) or -ETIMEDOUT if the
 * streamer did not disarm in time (the stream is released anyway).
 */
int iqnet_pl_stop(struct file *owner);
/*
 * Counters of the active PL stream (fresh snapshot), else those of the
 * last one. Returns 0 or -ETIMEDOUT if the snapshot handshake failed.
 */
int iqnet_pl_read_counters(struct iqnet_pl_counters *c, bool *running);

#endif /* _IQNET_PL_H */
