/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * iqnet: zero-copy UDP streaming of IIO DMA blocks (ioctl interface).
 *
 * Shared by the kernel module (package/iqnet-kmod/src/iqnet_main.c, iqnet_pl.c) and the
 * control daemon (package/iqnetd/src/iqnetd.c). Keep both in sync.
 */
#ifndef _UAPI_IQNET_H
#define _UAPI_IQNET_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IQNET_DEV "/dev/iqnet"

/*
 * IQNET_IOC_START: hand an IIO buffer over to the module.
 *
 * Before calling, userspace must have: selected the scan elements,
 * allocated blocks with IIO_BLOCK_ALLOC_IOCTL and enqueued all of them with
 * IIO_BLOCK_ENQUEUE_IOCTL (bytes_used = block size). The buffer may be
 * enabled before or after START (enabling after START is preferred, so no
 * samples are lost before the first datagram). buffer_fd must be the
 * /dev/iio:deviceN chardev itself (anonymous buffer fds are rejected).
 * From START until IQNET_IOC_STOP returns the module owns the blocks: the
 * block ioctls on that buffer return -EBUSY, and userspace must not free
 * the blocks, close the fd or toggle buffer/enable.
 */
struct iqnet_start {
	__s32 buffer_fd;   /* fd of the opened /dev/iio:deviceN */
	__u32 dst_addr;    /* IPv4 destination, network byte order */
	__u16 dst_port;    /* UDP destination port, network byte order */
	__u16 payload_len; /* IQ bytes per datagram, multiple of 24, <= IQNET_MAX_PAYLOAD */
	__u32 gso_segs;    /* datagrams per sendmsg via UDP_SEGMENT; 0 or 1 = no GSO, <= IQNET_MAX_GSO */
	__u32 flags;       /* IQNET_F_* */
	__u32 src_addr;    /* IPv4 source to bind, network byte order; 0 = chosen by routing */
	__u32 reserved[2]; /* must be zero */
};

/* debug: let the stack copy the payload instead of zero-copy */
#define IQNET_F_NO_ZEROCOPY (1u << 0)
/* debug: zero-copy, but do not set SKBFL_COHERENT_FRAGS (keep cache maintenance) */
#define IQNET_F_SYNC_CACHE  (1u << 1)

struct iqnet_stats {
	__u64 datagrams;       /* datagrams handed to the UDP stack */
	__u64 bytes;           /* IQ payload bytes handed to the UDP stack */
	__u64 blocks;          /* IIO blocks fully sent and returned to DMA */
	__u64 send_errors;     /* sock_sendmsg() failures (datagram dropped) */
	__u64 zc_copied;       /* completions with zerocopy_success == false (does NOT prove
				* zero-copy: linearize/segment copies report success) */
	__u64 overflows;       /* times no block was left queued to the DMA (PL dropped
				* samples); each one advances the stream offset by one payload */
	__u64 short_blocks;    /* short or aborted blocks dropped (offset advanced by block size) */
	__u64 copy_batches;    /* batches sent in copy mode because zero-copy was not possible */
	__u32 blocks_inflight; /* blocks dequeued from IIO, not yet returned */
	__u32 running;         /* 1 while a stream is active */
};

/*
 * IQNET_IOC_PL_START: stream the IIO buffer through the PL streamer.
 *
 * Same preconditions as IQNET_IOC_START (scan elements selected, blocks
 * allocated and enqueued, buffer not yet enabled). The module claims the
 * buffer, resolves the next-hop MAC, programs the streamer and arms it;
 * userspace then writes buffer/enable=1. No block ever completes: the
 * streamer keeps the DMA input idle while armed.
 *
 * Errors: ENODEV no streamer in the bitstream, EAGAIN next-hop MAC not
 * resolved yet (neighbour probe started, retry), ENETUNREACH no route or
 * route not via the PL-attached netdev, ENETDOWN link down or not
 * 1000/full, EINVAL bad payload_len/reserved, EBUSY a stream is active,
 * EIO the streamer did not arm within 10 ms (ADC or GMII clock not
 * running).
 */
struct iqnet_pl_start {
	__s32 buffer_fd;   /* fd of the opened /dev/iio:deviceN */
	__u32 dst_addr;    /* IPv4 destination, network byte order */
	__u16 dst_port;    /* UDP destination port, network byte order */
	__u16 payload_len; /* multiple of 24, 24..IQNET_MAX_PAYLOAD */
	__u32 flags;       /* must be zero */
	__u32 reserved[4]; /* must be zero */
};

#define IQNET_PATH_NONE   0
#define IQNET_PATH_KERNEL 1
#define IQNET_PATH_PL     2

/* IQNET_IOC_STATS plus the fields of the PL streamer path. */
struct iqnet_stats2 {
	struct iqnet_stats base;
	__u32 path;            /* IQNET_PATH_NONE / _KERNEL / _PL */
	__u32 pl_present;      /* 1 if the loaded bitstream has the streamer */
	__u64 linux_frames;    /* GEM frames passed through the streamer */
	__u64 linux_drops;     /* GEM frames dropped by the streamer; nonzero is a defect */
	__u64 fifo_hwm_bytes;  /* peak fill of the streamer data FIFO since START */
};

#define IQNET_IOC_MAGIC 'q'
#define IQNET_IOC_START _IOW(IQNET_IOC_MAGIC, 1, struct iqnet_start)
/* stop streaming; returns only after every in-flight block is back in IIO */
#define IQNET_IOC_STOP  _IO(IQNET_IOC_MAGIC, 2)
#define IQNET_IOC_STATS _IOR(IQNET_IOC_MAGIC, 3, struct iqnet_stats)
#define IQNET_IOC_PL_START _IOW(IQNET_IOC_MAGIC, 4, struct iqnet_pl_start)
#define IQNET_IOC_STATS2 _IOR(IQNET_IOC_MAGIC, 5, struct iqnet_stats2)

#endif /* _UAPI_IQNET_H */
