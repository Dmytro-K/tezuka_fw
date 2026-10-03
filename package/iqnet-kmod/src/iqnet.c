// SPDX-License-Identifier: GPL-2.0
/*
 * iqnet: zero-copy UDP streaming of IIO DMA blocks.
 *
 * Data path (default, zero-copy):
 *
 *   PL -> axi-dmac -> IIO DMA block (dma_alloc_coherent, CMA)
 *      -> this module dequeues the block in a kthread (legacy block API)
 *      -> each datagram is [16 B header from a coherent header ring]
 *                          [payload_len bytes of the block]
 *         handed to a kernel UDP socket as an ITER_BVEC with MSG_ZEROCOPY
 *         and msg_ubuf = per-block ubuf_info (__ip_append_data() attaches
 *         the pages as skb frags, no CPU copy)
 *      -> GEM (macb) DMA reads the frags; SKBFL_COHERENT_FRAGS tells macb
 *         that the frag pages are DMA-coherent, so it skips the L1/PL310
 *         cache maintenance for them
 *      -> when the last skb referencing the block is freed, the ubuf_info
 *         completion queues a work item that gives the block back to the
 *         IIO DMA queue (enqueue_block).
 *
 * Zero-copy is decided per batch (iqnet_zc_usable()): the frags must never
 * reach a path where the stack copies them through kmap(), because kmap()
 * is a cacheable alias of memory the DMA writes behind the CPU's back
 * (Cortex-A9 PIPT L1 + PL310: lines left over from an earlier kmap() of the
 * same page are stale). That happens when the device lacks SG or checksum
 * offload (__ip_append_data() silently copies) and, for highmem pages, when
 * it lacks NETIF_F_HIGHDMA (illegal_highdma() clears NETIF_F_SG in
 * validate_xmit_skb(), which then linearizes / software-segments by copy).
 * Such batches are sent in copy mode from the uncached coherent mapping.
 * On PlutoSky R2 the board DT (fishball.dtsi, linux,cma with lowmem
 * alloc-ranges) keeps the CMA pool in lowmem, so blocks are lowmem pages
 * whose linear alias __dma_remap() made uncached, and patch 0011 adds
 * NETIF_F_HIGHDMA to macb. The highmem case above only matters with a
 * cma= override or on other boards.
 *
 * Kernel APIs relied upon (linux-custom, ADI 6.12.77 tree, unpatched lines):
 *   include/linux/iio/buffer_impl.h:98   struct iio_buffer_access_funcs
 *                                        (legacy alloc/enqueue/dequeue_block)
 *   include/linux/iio/buffer_impl.h:151  struct iio_buffer (direction, access,
 *                                        pollq)
 *   include/linux/iio/buffer_impl.h:238  iio_buffer_get()/iio_buffer_put()
 *   include/linux/iio/buffer-dma.h:31    enum iio_block_state (DEQUEUED: owned
 *                                        by the legacy block API user)
 *   include/linux/iio/buffer-dma.h:56    struct iio_dma_buffer_block (state)
 *   include/linux/iio/buffer-dma.h:123   struct iio_dma_buffer_queue (buffer
 *                                        is the first member, lock,
 *                                        list_lock, outgoing, num_blocks,
 *                                        blocks[])
 *   drivers/iio/buffer/industrialio-buffer-dma.c:909 / :965
 *                                        iio_dma_buffer_{enqueue,dequeue}_block
 *                                        (take queue->lock -> process context)
 *   drivers/iio/buffer/industrialio-buffer-dma.c:222 DMA completion adds the
 *                                        block to outgoing under list_lock
 *   drivers/iio/buffer/industrialio-buffer-dma.c:539 block memory comes from
 *                                        dma_alloc_coherent(queue->dev)
 *   drivers/iio/industrialio-core.c:1799 / industrialio-buffer.c:1650
 *                                        closing the IIO fd frees all blocks
 *                                        -> we hold a struct file reference
 *   include/linux/skbuff.h:528           struct ubuf_info_ops
 *   include/linux/skbuff.h:543           struct ubuf_info {ops, refcnt, flags}
 *   include/linux/skbuff.h:1790          skb_zcopy_init() copies uarg->flags
 *   include/linux/skbuff.h:1824          net_zcopy_put()
 *   net/core/skbuff.c:1105 / :1111       skb_release_data(): ubuf completion
 *                                        runs BEFORE __skb_frag_unref()
 *   include/linux/socket.h:78            msghdr::msg_ubuf
 *   net/ipv4/ip_output.c:1005-1040       MSG_ZEROCOPY + msg_ubuf path (needs
 *                                        NETIF_F_SG and CHECKSUM_PARTIAL)
 *   net/ipv4/ip_output.c:1299            cork->fragsize = device MTU unless
 *                                        ip_sk_use_pmtu() (pmtudisc < PROBE)
 *   net/core/dev.c:3509                  illegal_highdma()
 *   net/core/dev.c:914                   dev_get_by_index_rcu()
 *   net/core/dev.c:11386                 synchronize_net()
 *   net/core/datagram.c:638              zerocopy_fill_skb_from_iter(): one
 *                                        frag per page, MAX_SKB_FRAGS limit
 *   net/core/sock.c:658                  sock_bindtoindex()
 *   lib/iov_iter.c:1149                  __iov_iter_get_pages_alloc(): bvec
 *                                        pages are get_page()'d
 *   net/ipv4/udp.c:962                   udp_send_skb() (UDP GSO checks,
 *                                        -ENOBUFS only with IP_RECVERR)
 *   net/ipv4/udp.c:1281 / :1308          connected send: sk_dst_check() /
 *                                        re-route honours sk_bound_dev_if
 *   net/ipv4/udp.c:2801                  UDP_SEGMENT sockopt
 *   net/socket.c:755                     sock_sendmsg()
 *   net/socket.c:1647                    sock_create_kern()
 *   net/socket.c:3565 / :3644            kernel_bind() / kernel_connect()
 *   net/ipv4/ip_sockglue.c:615 / :621    ip_sock_set_recverr() /
 *                                        ip_sock_set_mtu_discover()
 *   include/net/sock.h:2110 / :2117      __sk_dst_get() / sk_dst_get()
 *   include/linux/page-flags.h:570       PageHighMem()
 *   include/linux/list.h:751             list_count_nodes()
 *   include/linux/fs.h:1121              file_inode()
 *   include/linux/uio.h:286/288          iov_iter_kvec()/iov_iter_bvec()
 *   include/linux/bvec.h:44              bvec_set_page()
 *   include/linux/dma-mapping.h:475/482  dma_alloc_coherent()/dma_free_coherent()
 *   include/linux/miscdevice.h:91        misc_register()/misc_deregister()
 *   include/linux/wait.h:990             wait_event_killable_timeout()
 *   kernel/kthread.c:571                 kthread_create_on_cpu()
 *   kernel/module/main.c:824 / :849      __module_get() / module_put()
 *   include/linux/wait.h:1197            wait_woken()/woken_wake_function()
 *
 * Provided by tezuka kernel patches (not in the vanilla tree):
 *   iio_buffer_get_from_file()           include/linux/iio/buffer_impl.h
 *   iio_buffer_claim_blocks()            include/linux/iio/buffer_impl.h
 *   iio_buffer_release_blocks()          include/linux/iio/buffer_impl.h
 *   SKBFL_COHERENT_FRAGS                 include/linux/skbuff.h
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/bvec.h>
#include <linux/capability.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/sockptr.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/udp.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/iio/buffer.h>
#include <linux/iio/buffer_impl.h>
#include <linux/iio/buffer-dma.h>

#include <net/dst.h>
#include <net/ip.h>
#include <net/sock.h>

#include "iqnet_proto.h"
#include "iqnet_uapi.h"

#ifndef CONFIG_IIO_DMA_BUF_MMAP_LEGACY
#error "iqnet needs the legacy IIO block API (CONFIG_IIO_DMA_BUF_MMAP_LEGACY)"
#endif

/* Largest UDP payload one sendmsg() may carry (IP_MAX_MTU - IP - UDP). */
#define IQNET_MAX_SEND_BYTES	(IP_MAX_MTU - sizeof(struct iphdr) - \
				 sizeof(struct udphdr))
/* Per-datagram IP + UDP overhead (no IP options on our socket). */
#define IQNET_L3L4_OVERHEAD	(sizeof(struct iphdr) + sizeof(struct udphdr))
/* Bound on the coherent header ring (default config needs ~186 KiB). */
#define IQNET_MAX_RING_BYTES	SZ_4M
/* Idle poll period while waiting for a DMA block (or for buffer/enable). */
#define IQNET_IDLE_TIMEOUT	(HZ / 10)
/* Socket send timeout: bounds how long kthread_stop() may wait. */
#define IQNET_SNDTIMEO_MS	100
/* -ENOBUFS (qdisc full) retries before a datagram batch is dropped. */
#define IQNET_ENOBUFS_RETRIES	200
/* Consecutive hard send errors before the thread starts to back off. */
#define IQNET_ERR_BACKOFF	32
/* Period of the "still draining" warning in teardown. */
#define IQNET_DRAIN_WARN	(2 * HZ)
/*
 * Total time STOP waits for in-flight skbs before giving up and detaching
 * the stream as a zombie. Covers a TX watchdog reset of a stuck GEM (5 s).
 */
#define IQNET_DRAIN_TIMEOUT	(10 * HZ)
/* How long START waits for a draining zombie before returning -EBUSY. */
#define IQNET_ZOMBIE_WAIT	HZ

#define IQNET_KNOWN_FLAGS	(IQNET_F_NO_ZEROCOPY | IQNET_F_SYNC_CACHE)

static_assert(IQNET_MAX_GSO <= UDP_MAX_SEGMENTS);
static_assert((size_t)IQNET_MAX_GSO * (IQNET_HDR_LEN + IQNET_MAX_PAYLOAD) <=
	      IQNET_MAX_SEND_BYTES);
static_assert(sizeof(struct iqnet_hdr) == IQNET_HDR_LEN);
static_assert(IQNET_MAX_PAYLOAD < PAGE_SIZE); /* payload spans <= 2 pages */
static_assert(PAGE_SIZE % IQNET_HDR_LEN == 0); /* header never spans pages */

static unsigned int cpu = 1;
module_param(cpu, uint, 0644);
MODULE_PARM_DESC(cpu, "CPU the streaming thread is bound to (default 1; unbound if offline)");

static unsigned int sndbuf = SZ_4M;
module_param(sndbuf, uint, 0644);
MODULE_PARM_DESC(sndbuf, "UDP socket send buffer in bytes (default 4 MiB); bounds skbs in flight");

/* Wire header as stored in the coherent ring (explicit little endian). */
struct iqnet_hdr_le {
	__le32 magic;
	__le32 seq;
	__le64 offset;
};

static_assert(sizeof(struct iqnet_hdr_le) == IQNET_HDR_LEN);

struct iqnet_stream;

/*
 * One per IIO block. The ubuf_info is the zero-copy completion handle for
 * all skbs that reference the block's pages (and its header slots). The
 * stream thread holds one reference while it is sending the block; every
 * zero-copy skb holds one more (skb_zcopy_set()). When the count drops to
 * zero the work item returns the block to IIO.
 */
struct iqnet_block {
	struct ubuf_info uarg;
	struct work_struct work;
	struct iqnet_stream *s;
	void *vaddr;		/* coherent (uncached) kernel mapping */
	struct page **pages;	/* backing pages, PAGE_ALIGN(size) / PAGE_SIZE */
	unsigned int npages;
	u32 id;
	u32 size;		/* block size in bytes, multiple of payload */
	u32 hdr_base;		/* first header-ring slot of this block */
	bool sent_all;		/* written by the thread before the last put */
	/*
	 * The DMA was starved when this block was handed back, so the
	 * samples lost lie just before its data: force an offset gap when it
	 * is dequeued next. Set under iqnet.dma_lock while the block is with
	 * IIO; read and cleared by the thread once it has dequeued it.
	 */
	bool gap_before;
};

struct iqnet_stream {
	struct file *owner;		/* /dev/iqnet file that started us */
	struct file *iio_file;		/* pins the IIO chardev: no free_blocks */
	struct iio_buffer *buffer;	/* reference from iio_buffer_get_from_file() */
	bool claimed;			/* iio_buffer_claim_blocks() succeeded */
	struct iio_dma_buffer_queue *queue;
	struct device *dma_dev;		/* queue->dev, holds a device ref */
	struct socket *sock;
	struct task_struct *thread;
	int ifindex;			/* route device at START, socket bound to it */

	struct iqnet_block *blocks;
	unsigned int nblocks;
	unsigned int max_dg;		/* max datagrams in one block */

	struct iqnet_hdr_le *ring;	/* nblocks * max_dg headers, coherent */
	dma_addr_t ring_dma;
	size_t ring_size;
	struct page **ring_pages;
	unsigned int ring_npages;
	bool has_highmem;		/* a block or ring page is PageHighMem */

	__be32 dst_addr;
	__be16 dst_port;
	__be32 src_addr;
	unsigned int payload;
	unsigned int batch;		/* datagrams per sendmsg (UDP GSO) */
	bool zerocopy;			/* zero-copy requested and set up */
	u8 zc_flags;			/* ubuf_info::flags */
	int cpu;

	/*
	 * Blocks owned by IIO (incoming, being filled by the DMA, or done
	 * and waiting in the outgoing list), i.e. not dequeued by us.
	 * Protected by iqnet.dma_lock; see iqnet_dma_starved().
	 */
	unsigned int dma_queued;

	/* thread-private state */
	u32 seq;
	u64 offset;
	bool started;			/* first datagram framed (seq may wrap) */
	unsigned int consec_errors;
	bool zc_last;			/* result of the previous iqnet_zc_usable() */
	union {
		struct bio_vec bvec[MAX_SKB_FRAGS];
		struct kvec kvec[2 * IQNET_MAX_GSO];
	};
};

static void iqnet_reap_work(struct work_struct *work);

static struct {
	struct mutex lock;		/* serialises START/STOP/release/reap */
	struct iqnet_stream *stream;	/* protected by lock */
	/*
	 * Serialises the stream's dequeue_block() and enqueue_block() calls
	 * with the iqnet_stream::dma_queued update that goes with each, so
	 * the count is exact at every hand-back. Taken outside queue->lock.
	 */
	struct mutex dma_lock;
	/*
	 * A stopped stream whose in-flight skbs did not drain in time. It
	 * keeps the IIO file, the block claim, the header ring and a module
	 * reference until iqnet_reap_work() frees it. Written under lock,
	 * read locklessly by iqnet_return_block().
	 */
	struct iqnet_stream *zombie;
	struct work_struct reap_work;
	struct workqueue_struct *wq;
	wait_queue_head_t drain_wq;	/* inflight == 0, zombie reaped */
	atomic_t inflight;
	atomic_t running;
	atomic64_t datagrams;
	atomic64_t bytes;
	atomic64_t blocks;
	atomic64_t send_errors;
	atomic64_t zc_copied;
	atomic64_t overflows;
	atomic64_t short_blocks;
	atomic64_t copy_batches;
} iqnet = {
	.lock = __MUTEX_INITIALIZER(iqnet.lock),
	.dma_lock = __MUTEX_INITIALIZER(iqnet.dma_lock),
	.reap_work = __WORK_INITIALIZER(iqnet.reap_work, iqnet_reap_work),
};

/* ------------------------------------------------------------------------ */
/* IIO block hand-back and overflow (DMA starvation) detection               */

/*
 * Is the DMA starved right now, i.e. is no block left for it to fill?
 * Called with iqnet.dma_lock held, just before a block is handed back.
 *
 * The axi-dmac drops samples whenever no block is queued to it. Of the
 * s->dma_queued blocks IIO owns (exact: every dequeue and enqueue updates
 * it under dma_lock), the ones in the outgoing list are already filled;
 * the rest are queued to or being filled by the DMA. The DMA can only
 * move blocks to outgoing, never back, so "starved" seen here stays true
 * until our enqueue_block() ends it.
 *
 * A starvation episode can only end with a hand-back (nothing else gives
 * the DMA a block), so checking here, at the one event that ends it,
 * catches every episode exactly once, however long it lasted and wherever
 * the time went (thread too slow, so blocks pile up in outgoing, or skbs
 * draining too slowly, so blocks stay in flight). Sampling at dequeue time
 * instead misses a steady overload in which the DMA is briefly starved
 * between dequeues.
 *
 * Residual inaccuracy, accepted: the DMA may finish its last block between
 * this check and the enqueue (missed episode of a few microseconds), and an
 * episode that short may have lost nothing thanks to the DMAC FIFO (counted
 * anyway).
 */
static bool iqnet_dma_starved(struct iqnet_stream *s)
{
	unsigned int done;

	lockdep_assert_held(&iqnet.dma_lock);

	spin_lock_irq(&s->queue->list_lock);
	done = list_count_nodes(&s->queue->outgoing);
	spin_unlock_irq(&s->queue->list_lock);

	return s->dma_queued <= done;
}

static void iqnet_return_block(struct iqnet_stream *s, u32 id, u32 size,
			       bool sent)
{
	struct iio_buffer_block blk = {
		.id = id,
		.size = size,
		.bytes_used = size,
	};
	bool starved;
	int ret;

	mutex_lock(&iqnet.dma_lock);
	starved = iqnet_dma_starved(s);
	ret = s->buffer->access->enqueue_block(s->buffer, &blk);
	if (!ret) {
		s->dma_queued++;
		/*
		 * The DMA fills blocks in hand-back order, so @id is the first
		 * block filled after the lost samples: the gap goes right
		 * before its data. The thread cannot dequeue it before we drop
		 * dma_lock. Not while stopping: then nobody consumes the
		 * blocks and the DMA starving is expected.
		 */
		if (starved && id < s->nblocks &&
		    atomic_read(&iqnet.running)) {
			s->blocks[id].gap_before = true;
			atomic64_inc(&iqnet.overflows);
		}
	}
	mutex_unlock(&iqnet.dma_lock);

	if (ret)
		pr_warn_ratelimited("re-enqueue of block %u failed: %d\n",
				    id, ret);
	else if (sent)
		atomic64_inc(&iqnet.blocks);

	/*
	 * Must be the last access to @s: teardown may free it once this
	 * reaches zero. Only globals are touched afterwards.
	 */
	if (atomic_dec_and_test(&iqnet.inflight)) {
		wake_up(&iqnet.drain_wq);
		/*
		 * Pairs with the smp_mb() in iqnet_stream_stop() between
		 * publishing the zombie and re-reading inflight
		 * (atomic_dec_and_test() is fully ordered).
		 */
		if (READ_ONCE(iqnet.zombie))
			queue_work(iqnet.wq, &iqnet.reap_work);
	}
}

/*
 * Take the next filled block from IIO. On success the block is in flight
 * (iqnet.inflight) and no longer counted in s->dma_queued.
 */
static int iqnet_dequeue(struct iqnet_stream *s, struct iio_buffer_block *blk)
{
	int ret;

	mutex_lock(&iqnet.dma_lock);
	ret = s->buffer->access->dequeue_block(s->buffer, blk);
	if (!ret) {
		s->dma_queued--;
		atomic_inc(&iqnet.inflight);
	}
	mutex_unlock(&iqnet.dma_lock);

	return ret;
}

static void iqnet_block_work(struct work_struct *work)
{
	struct iqnet_block *b = container_of(work, struct iqnet_block, work);

	/*
	 * Read everything before the enqueue: as soon as the block is back in
	 * IIO the thread may dequeue it again and re-arm @b.
	 */
	iqnet_return_block(b->s, b->id, b->size, READ_ONCE(b->sent_all));
}

/*
 * Zero-copy completion. Called once per skb that held a reference (from
 * skb_release_data(), any context up to softirq) and once from
 * net_zcopy_put() by the thread (skb == NULL).
 *
 * skb_release_data() calls this BEFORE it drops the frag page references
 * (net/core/skbuff.c:1105 vs :1111), so the block (and its header ring
 * slots) can be back in IIO while that skb still holds get_page()
 * references. Teardown therefore runs synchronize_net() before it frees the
 * ring or lets IIO free the blocks (skb freeing on the TX path runs in
 * BH / RCU read-side sections).
 */
static void iqnet_zc_complete(struct sk_buff *skb, struct ubuf_info *uarg,
			      bool zerocopy_success)
{
	struct iqnet_block *b = container_of(uarg, struct iqnet_block, uarg);

	if (!zerocopy_success)
		atomic64_inc(&iqnet.zc_copied);

	if (!refcount_dec_and_test(&uarg->refcnt))
		return;

	/*
	 * enqueue_block() takes queue->lock (a mutex): defer to process
	 * context. queue_work() does not touch @b after the item is queued.
	 */
	queue_work(iqnet.wq, &b->work);
}

static const struct ubuf_info_ops iqnet_ubuf_ops = {
	.complete = iqnet_zc_complete,
	/* .link_skb is only used by TCP; NULL is checked by the core */
};

/* ------------------------------------------------------------------------ */
/* Sending                                                                   */

/*
 * Advance the stream offset over data that is not sent (board-side loss),
 * so receivers see an offset gap. Before the first datagram of the stream
 * nothing is advanced: the stream then simply starts later, and its first
 * datagram keeps seq == 0 && offset == 0 (the new-stream marker). Tested
 * with s->started, not s->seq, which wraps to 0 on long streams.
 */
static void iqnet_skip(struct iqnet_stream *s, u64 bytes)
{
	if (s->started)
		s->offset += bytes;
}

static void iqnet_fill_headers(struct iqnet_stream *s, struct iqnet_block *b,
			       unsigned int ndg)
{
	struct iqnet_hdr_le *h = &s->ring[b->hdr_base];
	unsigned int i;

	for (i = 0; i < ndg; i++) {
		h[i].magic = cpu_to_le32(IQNET_MAGIC);
		h[i].seq = cpu_to_le32(s->seq++);
		h[i].offset = cpu_to_le64(s->offset);
		s->offset += s->payload;
	}
	if (ndg)
		s->started = true;

	/*
	 * The ring is Normal-NC memory and the GEM reads it by DMA without
	 * any cache maintenance (SKBFL_COHERENT_FRAGS). Drain the CPU store
	 * buffer and the PL310 write buffer (wmb() = dsb + outer_sync on
	 * ARMv7) so the headers are in DRAM before any descriptor that
	 * points at them can be handed to the GEM, possibly by another CPU
	 * running the qdisc.
	 */
	wmb();
}

/*
 * May the next batch be sent zero-copy? Checks the device the socket's
 * route currently points at (the socket is bound to the START device, so
 * this is that device unless it went away). Without SG or checksum offload
 * __ip_append_data() would copy the bvec pages through kmap(); for highmem
 * pages without NETIF_F_HIGHDMA validate_xmit_skb() would linearize or
 * software-segment them by copy, again through kmap(). Both read DMA memory
 * through a cacheable alias, so such batches go out in copy mode instead.
 *
 * Residual races, accepted: features toggled with ethtool between this
 * check and the transmit, and stacked devices (VLAN, bridge) whose lower
 * device lacks a feature the upper one advertises are not walked.
 */
static bool iqnet_zc_usable(struct iqnet_stream *s)
{
	const netdev_features_t csum = NETIF_F_HW_CSUM | NETIF_F_IP_CSUM;
	struct sock *sk = s->sock->sk;
	netdev_features_t f = 0;
	struct net_device *dev;
	struct dst_entry *dst;

	rcu_read_lock();
	dst = __sk_dst_get(sk);
	dev = dst ? dst->dev : dev_get_by_index_rcu(sock_net(sk), s->ifindex);
	if (dev)
		f = READ_ONCE(dev->features);
	rcu_read_unlock();

	if (!(f & NETIF_F_SG) || !(f & csum))
		return false;
	if (s->has_highmem && !(f & NETIF_F_HIGHDMA))
		return false;
	return true;
}

/*
 * Build the iov for datagrams [first, first + n) of @b, stopping early to
 * respect the batch size and, for zero-copy, MAX_SKB_FRAGS.
 *
 * Zero-copy uses one bio_vec per page (header slot; payload split at the
 * page boundary into at most two pieces) instead of multi-page bvecs: this
 * does not rely on the struct pages of a block being virtually contiguous
 * (the pages come from a per-page lookup of the coherent mapping), and it
 * costs nothing because zerocopy_fill_skb_from_iter() creates one skb frag
 * per page anyway. So "bvecs == frags", which lets us bound frags exactly.
 *
 * Copy mode uses kvecs over the coherent (uncached) kernel mapping. Copying
 * through the struct pages would go through kmap(), i.e. a cacheable alias,
 * which can return stale lines for highmem CMA pages.
 *
 * Returns the number of datagrams in the batch (>= 1).
 */
static unsigned int iqnet_build_batch(struct iqnet_stream *s,
				      struct iqnet_block *b, bool zc,
				      unsigned int first, unsigned int n,
				      unsigned int *nvec, size_t *len)
{
	unsigned int segs = 0, v = 0;
	size_t bytes = 0;

	n = min(n, s->batch);

	while (segs < n) {
		unsigned int dg = first + segs;
		size_t off = (size_t)dg * s->payload;
		size_t hoff = (size_t)(b->hdr_base + dg) * IQNET_HDR_LEN;

		if (zc) {
			unsigned int pg = off >> PAGE_SHIFT;
			unsigned int po = offset_in_page(off);
			unsigned int l0 = min_t(unsigned int, s->payload,
						PAGE_SIZE - po);
			unsigned int need = l0 < s->payload ? 3 : 2;

			if (v + need > MAX_SKB_FRAGS)
				break;

			bvec_set_page(&s->bvec[v++],
				      s->ring_pages[hoff >> PAGE_SHIFT],
				      IQNET_HDR_LEN, offset_in_page(hoff));
			bvec_set_page(&s->bvec[v++], b->pages[pg], l0, po);
			if (l0 < s->payload)
				bvec_set_page(&s->bvec[v++], b->pages[pg + 1],
					      s->payload - l0, 0);
		} else {
			s->kvec[v].iov_base = &s->ring[b->hdr_base + dg];
			s->kvec[v++].iov_len = IQNET_HDR_LEN;
			s->kvec[v].iov_base = b->vaddr + off;
			s->kvec[v++].iov_len = s->payload;
		}

		bytes += IQNET_HDR_LEN + s->payload;
		segs++;
	}

	*nvec = v;
	*len = bytes;
	return segs;
}

/*
 * Send one batch, retrying on back-pressure. Returns 0 on success,
 * -EINTR if the thread was asked to stop, or the final send error.
 */
static int iqnet_send_batch(struct iqnet_stream *s, struct iqnet_block *b,
			    bool zc, unsigned int nvec, size_t len)
{
	unsigned int enobufs = 0;
	int ret;

	for (;;) {
		struct msghdr msg = {
			.msg_flags = MSG_NOSIGNAL,
		};

		/* Rebuild the iterator: a failed attempt may have advanced it. */
		if (zc) {
			msg.msg_flags |= MSG_ZEROCOPY;
			msg.msg_ubuf = &b->uarg;
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, s->bvec,
				      nvec, len);
		} else {
			iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, s->kvec,
				      nvec, len);
		}

		ret = sock_sendmsg(s->sock, &msg);
		if (ret >= 0)
			return 0;

		if (kthread_should_stop())
			return -EINTR;

		switch (ret) {
		case -EAGAIN:
			/*
			 * sk_sndbuf full for IQNET_SNDTIMEO_MS: the GEM is the
			 * bottleneck. Keep waiting; nothing was queued.
			 */
			continue;
		case -ENOBUFS:
			/*
			 * qdisc dropped the skb (reported because IP_RECVERR
			 * is set) or an allocation failed. Nothing reached the
			 * wire; back off briefly and resend the same batch.
			 */
			if (++enobufs > IQNET_ENOBUFS_RETRIES)
				return ret;
			usleep_range(100, 200);
			continue;
		default:
			return ret;
		}
	}
}

/* Send all datagrams of a block. Returns false if interrupted by stop. */
static bool iqnet_send_block(struct iqnet_stream *s, struct iqnet_block *b,
			     unsigned int ndg)
{
	unsigned int i = 0;

	while (i < ndg) {
		unsigned int segs, nvec;
		bool zc = false;
		size_t len;
		int ret;

		if (kthread_should_stop())
			return false;

		if (s->zerocopy) {
			zc = iqnet_zc_usable(s);
			if (unlikely(zc != s->zc_last)) {
				pr_warn_ratelimited("zero-copy %s (route device features%s)\n",
						    zc ? "resumed" : "not possible, copying",
						    s->has_highmem ?
						    ", highmem pages need NETIF_F_HIGHDMA" : "");
				s->zc_last = zc;
			}
			if (!zc)
				atomic64_inc(&iqnet.copy_batches);
		}

		segs = iqnet_build_batch(s, b, zc, i, ndg - i, &nvec, &len);
		ret = iqnet_send_batch(s, b, zc, nvec, len);
		if (ret == -EINTR)
			return false;

		if (ret) {
			atomic64_add(segs, &iqnet.send_errors);
			pr_warn_ratelimited("sendmsg failed: %d\n", ret);
			if (++s->consec_errors >= IQNET_ERR_BACKOFF)
				usleep_range(1000, 2000);
		} else {
			s->consec_errors = 0;
			atomic64_add(segs, &iqnet.datagrams);
			atomic64_add((u64)segs * s->payload, &iqnet.bytes);
		}

		i += segs;
		cond_resched();
	}

	return true;
}

/* Called for a block taken by iqnet_dequeue() (already counted in flight). */
static void iqnet_handle_block(struct iqnet_stream *s,
			       const struct iio_buffer_block *blk)
{
	struct iqnet_block *b;
	unsigned int ndg;
	bool done;

	if (unlikely(blk->id >= s->nblocks)) {
		/* cannot happen while the blocks are claimed; be defensive */
		pr_warn_ratelimited("unexpected block id %u (have %u)\n",
				    blk->id, s->nblocks);
		iqnet_return_block(s, blk->id, blk->size, false);
		return;
	}
	b = &s->blocks[blk->id];

	/*
	 * The DMA was starved when this block was handed back (counted in
	 * iqnet_return_block()): force a gap of one payload before its data.
	 * The real amount lost is unknown; receivers treat any gap as an
	 * overflow. The flag was set under dma_lock before our dequeue took
	 * that lock, and nobody else touches it while we own the block.
	 */
	if (b->gap_before) {
		b->gap_before = false;
		iqnet_skip(s, s->payload);
	}

	/*
	 * Short (0 < bytes_used < size) and aborted (bytes_used == 0) blocks
	 * are dropped whole: the stream offset advances by the full block
	 * size, which receivers see as a gap. Sending the valid prefix would
	 * leave the rest of the block's time span unaccounted for.
	 */
	if (unlikely(blk->bytes_used < b->size)) {
		atomic64_inc(&iqnet.short_blocks);
		pr_warn_ratelimited("block %u short: %u of %u bytes, dropped\n",
				    b->id, blk->bytes_used, b->size);
		iqnet_skip(s, b->size);
		iqnet_return_block(s, b->id, b->size, false);
		return;
	}

	ndg = b->size / s->payload;
	iqnet_fill_headers(s, b, ndg);

	if (!s->zerocopy) {
		done = iqnet_send_block(s, b, ndg);
		iqnet_return_block(s, b->id, b->size, done);
		return;
	}

	/*
	 * The block is not in flight (it was in IIO's outgoing list), so the
	 * previous generation's references are all gone: re-arm the count
	 * with the thread's own reference. Copy-mode batches of this block
	 * take no reference.
	 */
	WRITE_ONCE(b->sent_all, false);
	refcount_set(&b->uarg.refcnt, 1);

	done = iqnet_send_block(s, b, ndg);

	WRITE_ONCE(b->sent_all, done);
	/* Drop our reference; the last skb free hands the block back. */
	net_zcopy_put(&b->uarg);
}

static int iqnet_thread_fn(void *data)
{
	struct iqnet_stream *s = data;
	struct iio_buffer *buf = s->buffer;
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct iio_buffer_block blk;
	int ret;

	/*
	 * Not wait_event_*(buf->pollq, access->data_available(buf)):
	 * iio_dma_buffer_usage() takes queue->lock (a mutex), which must not
	 * be done in a wait_event condition (task state != TASK_RUNNING).
	 * Use the wait_woken() pattern instead, like iio_buffer_read() does,
	 * and simply try to dequeue (-EAGAIN means no completed block).
	 *
	 * The buffer may be enabled after START: until then no block
	 * completes, dequeue_block() returns -EAGAIN and the thread idles on
	 * pollq (re-checking every IQNET_IDLE_TIMEOUT).
	 */
	while (!kthread_should_stop()) {
		ret = iqnet_dequeue(s, &blk);
		if (ret == -EAGAIN) {
			add_wait_queue(&buf->pollq, &wait);
			/* re-check after queueing to not miss a wake-up */
			ret = iqnet_dequeue(s, &blk);
			if (ret == -EAGAIN)
				wait_woken(&wait, TASK_INTERRUPTIBLE,
					   IQNET_IDLE_TIMEOUT);
			remove_wait_queue(&buf->pollq, &wait);
			if (ret == -EAGAIN)
				continue;
		}
		if (ret) {
			pr_warn_ratelimited("dequeue_block failed: %d\n", ret);
			usleep_range(10000, 20000);
			continue;
		}

		iqnet_handle_block(s, &blk);
	}

	return 0;
}

/* ------------------------------------------------------------------------ */
/* Stream setup / teardown                                                   */

/*
 * Page backing one page of a dma_alloc_coherent() buffer. On ARM the
 * coherent mapping of a CMA buffer is either the (re-attributed) linear
 * map (lowmem) or a vmap in the vmalloc area (highmem), so derive the page
 * from the CPU address. Fall back to the bus address, which equals the CPU
 * physical address on Zynq-7000 (no IOMMU, no DMA offset).
 */
static struct page *iqnet_coherent_page(void *vaddr, dma_addr_t dma)
{
	unsigned long pfn = PHYS_PFN((phys_addr_t)dma);
	struct page *page = NULL;

	if (is_vmalloc_addr(vaddr))
		page = vmalloc_to_page(vaddr);
	else if (virt_addr_valid(vaddr))
		page = virt_to_page(vaddr);

	if (!page) {
		if (!pfn_valid(pfn))
			return NULL;
		page = pfn_to_page(pfn);
	} else if (page_to_pfn(page) != pfn) {
		pr_warn_once("bus address %pad is not the CPU physical address\n",
			     &dma);
	}

	/* skb frags take page references: the page must be refcounted. */
	if (!page_ref_count(page))
		return NULL;

	return page;
}

static int iqnet_map_pages(void *vaddr, dma_addr_t dma, size_t size,
			   struct page ***pagesp, unsigned int *npagesp,
			   bool *highmem)
{
	unsigned int i, npages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages;

	pages = kvcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		size_t off = (size_t)i << PAGE_SHIFT;

		pages[i] = iqnet_coherent_page(vaddr + off, dma + off);
		if (!pages[i]) {
			pr_err("cannot resolve page %u of coherent buffer %pad\n",
			       i, &dma);
			kvfree(pages);
			return -EINVAL;
		}
		if (PageHighMem(pages[i]))
			*highmem = true;
	}

	*pagesp = pages;
	*npagesp = npages;
	return 0;
}

/*
 * Frees everything and drops the stream's module reference. Only call once
 * nothing can reference @s any more: no in-flight skb, block work items
 * flushed, synchronize_net() done (or nothing was ever sent).
 */
static void iqnet_stream_free(struct iqnet_stream *s)
{
	unsigned int i;

	if (s->sock)
		sock_release(s->sock);

	if (s->blocks) {
		for (i = 0; i < s->nblocks; i++)
			kvfree(s->blocks[i].pages);
		kfree(s->blocks);
	}

	kvfree(s->ring_pages);
	if (s->ring)
		dma_free_coherent(s->dma_dev, s->ring_size, s->ring,
				  s->ring_dma);
	put_device(s->dma_dev);

	/*
	 * Order matters: the IIO blocks must outlive every skb frag, which
	 * the caller guaranteed. Releasing the claim re-enables the block
	 * ioctls; dropping the file may then run the IIO fd release and free
	 * the blocks.
	 */
	if (s->claimed)
		iio_buffer_release_blocks(s->buffer);
	iio_buffer_put(s->buffer);
	if (s->iio_file)
		fput(s->iio_file);

	kfree(s);

	/* Taken in iqnet_stream_create(); may allow module unload now. */
	module_put(THIS_MODULE);
}

static int iqnet_setup_socket(struct iqnet_stream *s)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_port = s->dst_port,
		.sin_addr.s_addr = s->dst_addr,
	};
	unsigned int dglen = IQNET_HDR_LEN + s->payload;
	struct dst_entry *dst;
	unsigned int mtu;
	struct sock *sk;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP,
			       &s->sock);
	if (ret) {
		s->sock = NULL;
		return ret;
	}
	sk = s->sock->sk;

	lock_sock(sk);
	sk->sk_userlocks |= SOCK_SNDBUF_LOCK;
	WRITE_ONCE(sk->sk_sndbuf, max_t(int, min_t(unsigned int, sndbuf,
						   INT_MAX / 2),
					SOCK_MIN_SNDBUF));
	WRITE_ONCE(sk->sk_sndtimeo, msecs_to_jiffies(IQNET_SNDTIMEO_MS));
	release_sock(sk);

	/* report qdisc drops as -ENOBUFS so the batch can be resent */
	ip_sock_set_recverr(sk);

	/*
	 * Keep DF but ignore the route's learned PMTU (IP_PMTUDISC_PROBE):
	 * the cork uses the device MTU (ip_output.c:1299), so a forged ICMP
	 * "fragmentation needed" cannot shrink the PMTU below our datagram
	 * size and turn every send into -EMSGSIZE.
	 */
	ret = ip_sock_set_mtu_discover(sk, IP_PMTUDISC_PROBE);
	if (ret)
		return ret;

	if (s->src_addr) {
		struct sockaddr_in src = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = s->src_addr,
		};

		ret = kernel_bind(s->sock, (struct sockaddr *)&src,
				  sizeof(src));
		if (ret) {
			pr_err("bind to %pI4 failed: %d\n", &s->src_addr, ret);
			return ret;
		}
	}

	ret = kernel_connect(s->sock, (struct sockaddr *)&sin, sizeof(sin), 0);
	if (ret) {
		pr_err("connect to %pI4:%u failed: %d\n", &s->dst_addr,
		       ntohs(s->dst_port), ret);
		return ret;
	}

	dst = sk_dst_get(sk);
	if (!dst) {
		pr_err("no route to %pI4\n", &s->dst_addr);
		return -EHOSTUNREACH;
	}
	s->ifindex = dst->dev->ifindex;
	dst_release(dst);

	/*
	 * Pin the socket to the START route device, so a later route change
	 * cannot move the stream to a device with different features (the
	 * per-batch zero-copy check still looks at the actual device). The
	 * bind resets the cached route; connect again to re-route with the
	 * device as oif (and the bound source address, if any).
	 */
	ret = sock_bindtoindex(sk, s->ifindex, true);
	if (ret) {
		pr_err("bind to ifindex %d failed: %d\n", s->ifindex, ret);
		return ret;
	}
	ret = kernel_connect(s->sock, (struct sockaddr *)&sin, sizeof(sin), 0);
	if (ret) {
		pr_err("re-connect to %pI4:%u on ifindex %d failed: %d\n",
		       &s->dst_addr, ntohs(s->dst_port), s->ifindex, ret);
		return ret;
	}

	dst = sk_dst_get(sk);
	if (!dst) {
		pr_err("no route to %pI4 on ifindex %d\n", &s->dst_addr,
		       s->ifindex);
		return -EHOSTUNREACH;
	}

	/* PROBE: the device MTU is what the stack enforces */
	mtu = READ_ONCE(dst->dev->mtu);
	if (IQNET_L3L4_OVERHEAD + dglen > mtu) {
		pr_err("datagram of %u bytes exceeds MTU %u of %s\n",
		       (unsigned int)(IQNET_L3L4_OVERHEAD + dglen), mtu,
		       netdev_name(dst->dev));
		dst_release(dst);
		return -EMSGSIZE;
	}
	dst_release(dst);

	if (s->batch > 1) {
		int val = dglen;

		ret = s->sock->ops->setsockopt(s->sock, SOL_UDP, UDP_SEGMENT,
					       KERNEL_SOCKPTR(&val),
					       sizeof(val));
		if (ret) {
			pr_warn("UDP_SEGMENT failed (%d), GSO disabled\n", ret);
			s->batch = 1;
		}
	}

	return 0;
}

static struct iqnet_stream *iqnet_stream_create(const struct iqnet_start *req,
						struct file *owner)
{
	struct iqnet_dma_snapshot {
		void *vaddr;
		dma_addr_t phys;
		u32 size;
	} *snap = NULL;
	struct iio_dma_buffer_queue *queue;
	struct iqnet_stream *s;
	unsigned int i, n;
	int ret;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	/*
	 * Called from our ioctl, so fops->owner already pins the module.
	 * The stream (and a zombie it may become) keeps its own reference,
	 * dropped in iqnet_stream_free(): the module cannot be unloaded while
	 * skb frags may still point at memory it manages.
	 */
	__module_get(THIS_MODULE);

	s->owner = owner;
	s->dst_addr = (__force __be32)req->dst_addr;
	s->dst_port = (__force __be16)req->dst_port;
	s->src_addr = (__force __be32)req->src_addr;
	s->payload = req->payload_len;
	s->zerocopy = !(req->flags & IQNET_F_NO_ZEROCOPY);
	s->zc_flags = SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN;
	if (!(req->flags & IQNET_F_SYNC_CACHE))
		s->zc_flags |= SKBFL_COHERENT_FRAGS;

	s->batch = req->gso_segs > 1 ? req->gso_segs : 1;
	s->batch = min_t(unsigned int, s->batch,
			 IQNET_MAX_SEND_BYTES / (IQNET_HDR_LEN + s->payload));

	s->iio_file = fget(req->buffer_fd);
	if (!s->iio_file) {
		ret = -EBADF;
		goto err;
	}

	/*
	 * Only the /dev/iio:deviceN chardev itself: anonymous buffer fds
	 * (IIO_BUFFER_GET_FD_IOCTL) are rejected here and by
	 * iio_buffer_get_from_file(). The file reference is kept for the
	 * whole stream: its release would free the blocks.
	 */
	if (!S_ISCHR(file_inode(s->iio_file)->i_mode)) {
		pr_err("fd %d is not a character device\n", req->buffer_fd);
		ret = -EINVAL;
		goto err;
	}

	s->buffer = iio_buffer_get_from_file(s->iio_file);
	if (IS_ERR_OR_NULL(s->buffer)) {
		ret = s->buffer ? PTR_ERR(s->buffer) : -EINVAL;
		s->buffer = NULL;
		pr_err("fd %d is not an IIO chardev with a buffer: %d\n",
		       req->buffer_fd, ret);
		goto err;
	}

	/*
	 * container_of() to the DMA queue is only valid for buffers whose
	 * access ops are the generic DMA buffer ones (dmaengine buffer).
	 */
	if (!s->buffer->access ||
	    s->buffer->access->dequeue_block != iio_dma_buffer_dequeue_block ||
	    s->buffer->access->enqueue_block != iio_dma_buffer_enqueue_block) {
		pr_err("fd %d is not a legacy-block IIO DMA buffer\n",
		       req->buffer_fd);
		ret = -EINVAL;
		goto err;
	}
	if (s->buffer->direction != IIO_BUFFER_DIRECTION_IN) {
		pr_err("IIO buffer is not an input buffer\n");
		ret = -EINVAL;
		goto err;
	}

	/*
	 * Take the blocks away from userspace: until released, the block
	 * ioctls (alloc/free/query/enqueue/dequeue) and IIO_BUFFER_GET_FD on
	 * this device fail with -EBUSY, so the block table below cannot
	 * change and nobody else dequeues "our" blocks.
	 */
	ret = iio_buffer_claim_blocks(s->buffer);
	if (ret) {
		pr_err("IIO blocks already claimed: %d\n", ret);
		goto err;
	}
	s->claimed = true;

	queue = container_of(s->buffer, struct iio_dma_buffer_queue, buffer);
	s->queue = queue;
	s->dma_dev = get_device(queue->dev);

	/* Snapshot the block table; blocks do not move while allocated. */
	mutex_lock(&queue->lock);
	n = queue->num_blocks;
	mutex_unlock(&queue->lock);
	if (!n || n > IQNET_MAX_BLOCKS) {
		pr_err("need 1..%u IIO blocks, have %u\n", IQNET_MAX_BLOCKS, n);
		ret = -EINVAL;
		goto err;
	}

	snap = kcalloc(n, sizeof(*snap), GFP_KERNEL);
	if (!snap) {
		ret = -ENOMEM;
		goto err;
	}

	mutex_lock(&queue->lock);
	if (queue->num_blocks != n) {
		mutex_unlock(&queue->lock);
		ret = -EBUSY;
		goto err;
	}
	for (i = 0; i < n; i++) {
		struct iio_dma_buffer_block *db = queue->blocks[i];

		snap[i].vaddr = db->vaddr;
		snap[i].phys = db->phys_addr;
		snap[i].size = db->block.size;
		/*
		 * Blocks still owned by IIO, for the starvation check. Only
		 * the (claimed) block ioctls move blocks in and out of the
		 * DEQUEUED state, so this cannot change under us.
		 */
		if (db->state != IIO_BLOCK_STATE_DEQUEUED &&
		    db->state != IIO_BLOCK_STATE_DEAD)
			s->dma_queued++;
	}
	mutex_unlock(&queue->lock);

	s->nblocks = n;
	for (i = 0; i < n; i++) {
		if (!snap[i].size || snap[i].size % s->payload) {
			pr_err("block %u size %u is not a multiple of payload %u\n",
			       i, snap[i].size, s->payload);
			ret = -EINVAL;
			goto err;
		}
		s->max_dg = max(s->max_dg, snap[i].size / s->payload);
	}

	/* Header ring: one region of max_dg slots per IIO block id. */
	s->ring_size = (size_t)n * s->max_dg * IQNET_HDR_LEN;
	if (s->ring_size > IQNET_MAX_RING_BYTES) {
		pr_err("header ring of %zu bytes too large (payload too small?)\n",
		       s->ring_size);
		ret = -EINVAL;
		goto err;
	}
	s->ring_size = PAGE_ALIGN(s->ring_size);
	s->ring = dma_alloc_coherent(s->dma_dev, s->ring_size, &s->ring_dma,
				     GFP_KERNEL);
	if (!s->ring) {
		ret = -ENOMEM;
		goto err;
	}
	ret = iqnet_map_pages(s->ring, s->ring_dma, s->ring_size,
			      &s->ring_pages, &s->ring_npages,
			      &s->has_highmem);
	if (ret)
		goto err;

	s->blocks = kcalloc(n, sizeof(*s->blocks), GFP_KERNEL);
	if (!s->blocks) {
		ret = -ENOMEM;
		goto err;
	}
	for (i = 0; i < n; i++) {
		struct iqnet_block *b = &s->blocks[i];

		b->s = s;
		b->id = i;
		b->size = snap[i].size;
		b->vaddr = snap[i].vaddr;
		b->hdr_base = i * s->max_dg;
		b->uarg.ops = &iqnet_ubuf_ops;
		b->uarg.flags = s->zc_flags;
		refcount_set(&b->uarg.refcnt, 0);
		INIT_WORK(&b->work, iqnet_block_work);

		ret = iqnet_map_pages(snap[i].vaddr, snap[i].phys, snap[i].size,
				      &b->pages, &b->npages, &s->has_highmem);
		if (ret)
			goto err;
	}
	kfree(snap);
	snap = NULL;

	ret = iqnet_setup_socket(s);
	if (ret)
		goto err;

	if (s->zerocopy) {
		s->zc_last = iqnet_zc_usable(s);
		if (!s->zc_last)
			pr_warn("zero-copy not possible on ifindex %d now (needs SG, csum offload%s): batches will be copied\n",
				s->ifindex, s->has_highmem ?
				" and NETIF_F_HIGHDMA for highmem blocks" : "");
	}

	if (cpu < nr_cpu_ids && cpu_online(cpu)) {
		s->cpu = cpu;
		s->thread = kthread_create_on_cpu(iqnet_thread_fn, s, cpu,
						  "iqnet/%u");
	} else {
		pr_warn("cpu %u not online, streaming thread is unbound\n", cpu);
		s->cpu = -1;
		s->thread = kthread_create(iqnet_thread_fn, s, "iqnet");
	}
	if (IS_ERR(s->thread)) {
		ret = PTR_ERR(s->thread);
		s->thread = NULL;
		goto err;
	}
	get_task_struct(s->thread);

	return s;

err:
	kfree(snap);
	iqnet_stream_free(s);
	return ERR_PTR(ret);
}

/*
 * Final teardown once inflight == 0. Flush the block work items (the last
 * one may still be running its tail after the decrement), then wait for
 * every CPU to leave the BH / RCU section in which the last skbs were
 * freed: skb_release_data() completes the ubuf_info before it drops the
 * frag page references, so a CPU may still be about to put_page() a ring
 * or block page.
 */
static void iqnet_stream_finish(struct iqnet_stream *s)
{
	unsigned int i;

	for (i = 0; i < s->nblocks; i++)
		flush_work(&s->blocks[i].work);

	synchronize_net();

	pr_info("stopped: datagrams=%llu bytes=%llu blocks=%llu send_errors=%llu zc_copied=%llu overflows=%llu short_blocks=%llu copy_batches=%llu\n",
		atomic64_read(&iqnet.datagrams), atomic64_read(&iqnet.bytes),
		atomic64_read(&iqnet.blocks),
		atomic64_read(&iqnet.send_errors),
		atomic64_read(&iqnet.zc_copied),
		atomic64_read(&iqnet.overflows),
		atomic64_read(&iqnet.short_blocks),
		atomic64_read(&iqnet.copy_batches));

	iqnet_stream_free(s);
}

/* Bounded, killable wait for inflight == 0. Returns true when drained. */
static bool iqnet_wait_drain(void)
{
	unsigned long waited = 0;
	long ret;

	for (;;) {
		ret = wait_event_killable_timeout(iqnet.drain_wq,
						  !atomic_read(&iqnet.inflight),
						  IQNET_DRAIN_WARN);
		if (ret > 0)
			return true;
		if (ret < 0) {
			pr_warn("drain interrupted with %d blocks in flight\n",
				atomic_read(&iqnet.inflight));
			return false;
		}
		waited += IQNET_DRAIN_WARN;
		if (waited >= IQNET_DRAIN_TIMEOUT)
			return false;
		pr_warn("still waiting for %d in-flight blocks\n",
			atomic_read(&iqnet.inflight));
	}
}

/*
 * Stop the stream and release it, or detach it as a zombie if its in-flight
 * skbs do not drain in time. Called with iqnet.lock held; the caller clears
 * iqnet.stream in both cases. Returns 0 or -ETIMEDOUT (zombie).
 */
static int iqnet_stream_stop(struct iqnet_stream *s)
{
	atomic_set(&iqnet.running, 0);

	/*
	 * The thread notices kthread_should_stop() within IQNET_SNDTIMEO_MS
	 * (blocked in sendmsg) or IQNET_IDLE_TIMEOUT (waiting for a block).
	 * It drops its reference on a partially sent block before exiting.
	 */
	kthread_stop(s->thread);
	put_task_struct(s->thread);
	s->thread = NULL;

	/*
	 * Closing the socket does not free skbs already handed to the IP
	 * layer; they are released when the GEM has transmitted them (or the
	 * qdisc is reset). Their completions return the remaining blocks.
	 */
	sock_release(s->sock);
	s->sock = NULL;

	if (iqnet_wait_drain()) {
		iqnet_stream_finish(s);
		return 0;
	}

	/*
	 * Never free the ring, the ubuf_infos or the IIO blocks while a frag
	 * may still reference them. Detach instead: the zombie keeps the IIO
	 * file, the block claim, the ring and a module reference; START
	 * returns -EBUSY until iqnet_reap_work() frees it, which the last
	 * block hand-back triggers. If an skb leaked, it stays forever.
	 */
	pr_err("STOP: %d blocks still in flight after %d s; stream detached, IIO blocks stay claimed until they drain (leaked skb or stuck TX queue?)\n",
	       atomic_read(&iqnet.inflight), IQNET_DRAIN_TIMEOUT / HZ);
	WRITE_ONCE(iqnet.zombie, s);
	/* Pairs with atomic_dec_and_test() in iqnet_return_block(). */
	smp_mb();
	if (!atomic_read(&iqnet.inflight))
		queue_work(iqnet.wq, &iqnet.reap_work);
	return -ETIMEDOUT;
}

static void iqnet_reap_work(struct work_struct *work)
{
	struct iqnet_stream *s;

	mutex_lock(&iqnet.lock);
	s = iqnet.zombie;
	if (!s || atomic_read(&iqnet.inflight)) {
		mutex_unlock(&iqnet.lock);
		return;
	}
	pr_warn("detached stream drained, releasing it\n");
	/*
	 * Clear iqnet.zombie before freeing it: iqnet_stream_free() drops the
	 * stream's module reference, after which rmmod may run iqnet_exit(),
	 * which must not see a stale zombie. Both happen under the lock, so
	 * a START (which checks iqnet.zombie under the lock) still finds the
	 * IIO blocks released once it gets in. The lockless readers (START's
	 * pre-wait, iqnet_return_block()) only test the pointer. Module exit
	 * drains iqnet.wq (and so this work item) in destroy_workqueue()
	 * before our text goes away.
	 */
	WRITE_ONCE(iqnet.zombie, NULL);
	iqnet_stream_finish(s);
	mutex_unlock(&iqnet.lock);
	wake_up(&iqnet.drain_wq);
}

/* ------------------------------------------------------------------------ */
/* ioctl interface                                                           */

static long iqnet_ioctl_start(struct file *filp, void __user *argp)
{
	struct iqnet_start req;
	struct iqnet_stream *s;
	long ret = 0;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.reserved[0] || req.reserved[1] ||
	    (req.flags & ~IQNET_KNOWN_FLAGS))
		return -EINVAL;
	if (!req.payload_len || req.payload_len % IQNET_BURST ||
	    req.payload_len > IQNET_MAX_PAYLOAD)
		return -EINVAL;
	if (req.gso_segs > IQNET_MAX_GSO)
		return -EINVAL;
	if (!req.dst_addr || !req.dst_port || req.buffer_fd < 0)
		return -EINVAL;

	/* Give a zombie that is about to drain a moment to go away. */
	if (READ_ONCE(iqnet.zombie))
		wait_event_killable_timeout(iqnet.drain_wq,
					    !READ_ONCE(iqnet.zombie),
					    IQNET_ZOMBIE_WAIT);

	mutex_lock(&iqnet.lock);
	if (iqnet.stream || iqnet.zombie) {
		ret = -EBUSY;
		goto out;
	}

	s = iqnet_stream_create(&req, filp);
	if (IS_ERR(s)) {
		ret = PTR_ERR(s);
		goto out;
	}

	atomic_set(&iqnet.inflight, 0);
	atomic64_set(&iqnet.datagrams, 0);
	atomic64_set(&iqnet.bytes, 0);
	atomic64_set(&iqnet.blocks, 0);
	atomic64_set(&iqnet.send_errors, 0);
	atomic64_set(&iqnet.zc_copied, 0);
	atomic64_set(&iqnet.overflows, 0);
	atomic64_set(&iqnet.short_blocks, 0);
	atomic64_set(&iqnet.copy_batches, 0);
	atomic_set(&iqnet.running, 1);
	iqnet.stream = s;

	pr_info("started: dst=%pI4:%u src=%pI4 ifindex=%d payload=%u blocks=%ux%u batch=%u mode=%s%s cpu=%d sndbuf=%d\n",
		&s->dst_addr, ntohs(s->dst_port), &s->src_addr, s->ifindex,
		s->payload, s->nblocks, s->blocks[0].size, s->batch,
		!s->zerocopy ? "copy" :
		(s->zc_flags & SKBFL_COHERENT_FRAGS) ? "zerocopy-coherent" :
						       "zerocopy-sync",
		s->has_highmem ? " (highmem)" : "",
		s->cpu, READ_ONCE(s->sock->sk->sk_sndbuf));

	wake_up_process(s->thread);
out:
	mutex_unlock(&iqnet.lock);
	return ret;
}

static long iqnet_ioctl_stop(void)
{
	long ret = 0;

	mutex_lock(&iqnet.lock);
	if (iqnet.stream) {
		ret = iqnet_stream_stop(iqnet.stream);
		iqnet.stream = NULL;
	}
	mutex_unlock(&iqnet.lock);
	return ret;
}

static long iqnet_ioctl_stats(void __user *argp)
{
	struct iqnet_stats st = {
		.datagrams = atomic64_read(&iqnet.datagrams),
		.bytes = atomic64_read(&iqnet.bytes),
		.blocks = atomic64_read(&iqnet.blocks),
		.send_errors = atomic64_read(&iqnet.send_errors),
		.zc_copied = atomic64_read(&iqnet.zc_copied),
		.overflows = atomic64_read(&iqnet.overflows),
		.short_blocks = atomic64_read(&iqnet.short_blocks),
		.copy_batches = atomic64_read(&iqnet.copy_batches),
		.blocks_inflight = atomic_read(&iqnet.inflight),
		.running = atomic_read(&iqnet.running),
	};

	return copy_to_user(argp, &st, sizeof(st)) ? -EFAULT : 0;
}

static long iqnet_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case IQNET_IOC_START:
		return iqnet_ioctl_start(filp, argp);
	case IQNET_IOC_STOP:
		return iqnet_ioctl_stop();
	case IQNET_IOC_STATS:
		return iqnet_ioctl_stats(argp);
	default:
		return -ENOTTY;
	}
}

static int iqnet_release(struct inode *inode, struct file *filp)
{
	mutex_lock(&iqnet.lock);
	if (iqnet.stream && iqnet.stream->owner == filp) {
		/* a killed owner detaches at once; the zombie drains async */
		iqnet_stream_stop(iqnet.stream);
		iqnet.stream = NULL;
	}
	mutex_unlock(&iqnet.lock);
	return 0;
}

static const struct file_operations iqnet_fops = {
	.owner = THIS_MODULE,
	.release = iqnet_release,
	.unlocked_ioctl = iqnet_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice iqnet_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "iqnet",
	.fops = &iqnet_fops,
	.mode = 0600,
};

static int __init iqnet_init(void)
{
	int ret;

	init_waitqueue_head(&iqnet.drain_wq);

	/*
	 * Block hand-back latency matters (the DMA overruns when it runs out
	 * of blocks): high priority, unbound so it does not compete with the
	 * streaming thread's CPU.
	 */
	iqnet.wq = alloc_workqueue("iqnet", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!iqnet.wq)
		return -ENOMEM;

	ret = misc_register(&iqnet_misc);
	if (ret) {
		destroy_workqueue(iqnet.wq);
		return ret;
	}

	return 0;
}

static void __exit iqnet_exit(void)
{
	misc_deregister(&iqnet_misc);

	/*
	 * Every stream and zombie holds a module reference, so none can
	 * exist here (a zombie with in-flight frags makes rmmod fail with
	 * -EBUSY instead of freeing memory the frags point at).
	 */
	WARN_ON(iqnet.stream || READ_ONCE(iqnet.zombie));

	/*
	 * A completion that queued the last work item may still be executing
	 * its epilogue in softirq context; let it leave our text. Then drain
	 * the block work items and a reap work item still returning after
	 * its module_put().
	 */
	synchronize_rcu();
	destroy_workqueue(iqnet.wq);
}

module_init(iqnet_init);
module_exit(iqnet_exit);

MODULE_IMPORT_NS(IIO_DMA_BUFFER);
MODULE_AUTHOR("Dmytro-K");
MODULE_DESCRIPTION("Zero-copy UDP streaming of IIO DMA blocks");
MODULE_LICENSE("GPL");
