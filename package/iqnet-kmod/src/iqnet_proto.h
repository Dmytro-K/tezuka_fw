/* SPDX-License-Identifier: MIT */
/*
 * iqnet wire format and control protocol.
 *
 * Shared by the kernel module, iqnetd, the host test tool
 * (app/iqnet_rx) and SoapyPlutoPAPR (a verbatim copy lives there as
 * iqnet_proto.h). Keep all copies identical.
 *
 * DATA (UDP, board -> host): every datagram is
 *     struct iqnet_hdr (16 bytes, little-endian) + payload_len IQ bytes
 * The IQ bytes are the raw IIO DMA stream (CS12/CS8/CS16 exactly as the PL
 * wrote it). payload_len is a multiple of 24 (one CS12 burst), so the CS12
 * burst phase is the same in every datagram and survives packet loss.
 * Datagrams never span two IIO blocks; block size is a multiple of
 * payload_len.
 * Loss on the board (DMA overflow, short/aborted blocks) is signalled by a
 * jump in offset (always a multiple of 24), exactly like network loss is
 * signalled by a jump in seq/offset: receivers must treat any offset gap as
 * an overflow. seq == 0 && offset == 0 marks the first datagram of a new
 * stream (START).
 *
 * CONTROL (TCP, host -> board, port IQNET_CTRL_PORT), ASCII lines ending
 * in '\n':
 *   START <udp_port> <mode> [blocks=<n>] [block_size=<bytes>] [gso=<segs>]
 *       mode: cs12 | cs8 | cs16. The UDP destination IP is the TCP peer.
 *       reply: "OK <payload_len> <block_size>\n" or "ERR <message>\n"
 *   STOP   -> "OK\n"
 *   STATS  -> "STATS datagrams=<n> bytes=<n> blocks=<n> send_errors=<n>
 *              zc_copied=<n> overflows=<n> short_blocks=<n> copy_batches=<n>
 *              inflight=<n> running=<0|1>\n" (one line, key=value, parse
 *              tolerantly: unknown keys may be added later)
 *   Closing the TCP connection stops the stream.
 */
#ifndef IQNET_PROTO_H
#define IQNET_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef __u32 iqnet_u32;
typedef __u64 iqnet_u64;
#else
#include <stdint.h>
typedef uint32_t iqnet_u32;
typedef uint64_t iqnet_u64;
#endif

#define IQNET_MAGIC 0x31514e49u /* bytes 'I' 'N' 'Q' '1' on the wire */
#define IQNET_HDR_LEN 16
#define IQNET_BURST 24              /* one CS12 burst = 8 IQ samples */
#define IQNET_MAX_PAYLOAD 1440      /* 60 bursts; 16 + 1440 + 8 + 20 <= 1500 */
#define IQNET_DEFAULT_PAYLOAD 1440
#define IQNET_DEFAULT_BLOCK_SIZE (728u * IQNET_DEFAULT_PAYLOAD) /* 1048320 */
#define IQNET_DEFAULT_BLOCKS 16
#define IQNET_MAX_BLOCKS 64         /* kernel limit in iio_dma_buffer_alloc_blocks */
#define IQNET_MAX_GSO 44            /* datagrams per GSO send: 44 * (16 + 1440) = 64064 B */
#define IQNET_CTRL_PORT 30433         /* not 30431: that is iiod (IIOD_PORT) */

struct iqnet_hdr {
	iqnet_u32 magic;  /* IQNET_MAGIC */
	iqnet_u32 seq;    /* datagram counter since START, starts at 0 */
	iqnet_u64 offset; /* stream byte offset of the first payload byte since START */
};

#endif /* IQNET_PROTO_H */
