// SPDX-License-Identifier: GPL-2.0
/*
 * iqnet: control of the PL UDP streamer (path "pl").
 *
 * The streamer (iqnet_pl in the bitstream) takes the packed ADC words
 * before the IIO DMA, builds the UDP datagrams itself and inserts them into
 * the GMII TX path between the GEM and gmii_to_rgmii. The kernel only
 * controls it: this file binds to its DT node, resolves the addresses at
 * START, arms it, keeps CTRL.link_up in step with the carrier and reads its
 * counters. No sample ever passes through the CPU.
 *
 * Presence: the stock bitstream does not decode the streamer's address, so
 * an access there is an external abort. probe() therefore reads the
 * presence GPIO (EMIO, 1 only in the bitstream with the streamer) before it
 * touches the registers.
 *
 * Locking: iqnet_pl.lock (mutex) serialises probe/remove, START, STOP and
 * the counter snapshots. iqnet_pl.ctrl_lock (spinlock) covers every CTRL
 * write and the state the netdev notifier looks at; the notifier runs
 * under RTNL and takes only ctrl_lock. Lock order:
 * iqnet.lock (iqnet_main.c) -> iqnet_pl.lock -> RTNL -> iqnet_pl.ctrl_lock.
 *
 * Kernel APIs relied upon (linux-custom, ADI 6.12.77 tree):
 *   include/linux/gpio/consumer.h:87     devm_gpiod_get()
 *   drivers/base/platform.c:122          devm_platform_ioremap_resource()
 *   net/core/net-sysfs.c:2082            of_find_net_device_by_node()
 *                                        (device reference, put_device())
 *   include/net/route.h:149 / :261       ip_route_output_key() / ip_rt_put()
 *   include/net/dst.h:402                dst_neigh_lookup() (creates the
 *                                        entry for the next hop if missing)
 *   include/net/neighbour.h:471 / :579   neigh_event_send() /
 *                                        neigh_ha_snapshot()
 *   net/ethtool/ioctl.c:437              __ethtool_get_link_ksettings()
 *                                        (needs RTNL)
 *   net/core/dev.c:1841                  register_netdevice_notifier()
 *   include/linux/netdevice.h:4178       netif_carrier_ok()
 *   include/linux/iopoll.h:169           readl_poll_timeout()
 *
 * Provided by tezuka kernel patches (not in the vanilla tree):
 *   iio_buffer_get_from_file()           include/linux/iio/buffer_impl.h
 *   iio_buffer_claim_blocks()            include/linux/iio/buffer_impl.h
 *   iio_buffer_release_blocks()          include/linux/iio/buffer_impl.h
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": pl: " fmt

#include <linux/bits.h>
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/in.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/buffer_impl.h>

#include <net/dst.h>
#include <net/neighbour.h>
#include <net/route.h>

#include "iqnet_pl.h"
#include "iqnet_proto.h"
#include "iqnet_uapi.h"

/* Register map (specs/001-pl-udp-streamer/contracts/registers.md). */
#define IQNET_PL_ID			0x000
#define IQNET_PL_VERSION		0x004
#define IQNET_PL_CTRL			0x008
#define IQNET_PL_CTRL_ENABLE		BIT(0)
#define IQNET_PL_CTRL_LINK_UP		BIT(1)
#define IQNET_PL_CTRL_SNAPSHOT		BIT(2)	/* self-clearing */
#define IQNET_PL_CTRL_CLEAR		BIT(3)	/* self-clearing */
#define IQNET_PL_STATUS			0x00c
#define IQNET_PL_STATUS_ARMED		BIT(0)
#define IQNET_PL_STATUS_STREAMING	BIT(1)
#define IQNET_PL_STATUS_SPEED_1G	BIT(2)
#define IQNET_PL_STATUS_SNAPSHOT_DONE	BIT(3)
#define IQNET_PL_DST_MAC		0x010	/* LO, HI at +4 */
#define IQNET_PL_SRC_MAC		0x018	/* LO, HI at +4 */
#define IQNET_PL_SRC_IP			0x020
#define IQNET_PL_DST_IP			0x024
#define IQNET_PL_PORTS			0x028
#define IQNET_PL_PAYLOAD_LEN		0x02c
#define IQNET_PL_PAYLOAD_LEN_MASK	GENMASK(13, 0)
#define IQNET_PL_DATAGRAMS		0x040	/* 64-bit counters: LO, HI at +4 */
#define IQNET_PL_BYTES			0x048
#define IQNET_PL_OVERFLOWS		0x050
#define IQNET_PL_LINUX_FRAMES		0x058
#define IQNET_PL_LINUX_DROPS		0x060
#define IQNET_PL_FIFO_HWM		0x068	/* in FIFO words */
#define IQNET_PL_FIFO_DEPTH		0x06c	/* in FIFO words */

#define IQNET_PL_ID_VALUE		0x4c505149	/* 'I' 'Q' 'P' 'L' */
#define IQNET_PL_VERSION_MAJOR		1
#define IQNET_PL_WORD_BYTES		8	/* data FIFO word: 64 bits */

/* STATUS poll period and bounds (process context, sleeping polls). */
#define IQNET_PL_POLL_US		10
/* enable 0->1 resets the counters and crosses into the ADC domain */
#define IQNET_PL_ARM_TIMEOUT_MS		10
/*
 * enable 1->0: the framer still sends the chunks it accepted (at most the
 * data FIFO, well under 1 ms at 1 Gb/s even with Linux frames in between).
 */
#define IQNET_PL_DISARM_TIMEOUT_MS	100
/* snapshot handshake into the GMII and ADC domains */
#define IQNET_PL_SNAPSHOT_TIMEOUT_MS	10

static_assert(IQNET_MAX_PAYLOAD <= IQNET_PL_PAYLOAD_LEN_MASK);

struct iqnet_pl_dev {
	struct device *dev;
	void __iomem *regs;
	struct device_node *eth_np;	/* DT "ethernet" phandle (gem0) */
	u32 version;
	u32 fifo_depth;			/* data FIFO, in words */
};

/* Addresses of a stream, fixed at START. */
struct iqnet_pl_cfg {
	__be32 dst_addr;
	__be32 src_addr;
	__be16 dst_port;
	u16 payload;
	u8 dst_mac[ETH_ALEN];
	u8 src_mac[ETH_ALEN];
};

static int iqnet_pl_netdev_event(struct notifier_block *nb,
				 unsigned long event, void *ptr);

static struct {
	struct mutex lock;		/* see the top of the file */
	struct iqnet_pl_dev *pd;	/* bound device; written under lock */

	/* The session: one PL stream at a time. Fields under lock. */
	struct file *owner;		/* /dev/iqnet file that started it */
	struct file *iio_file;		/* pins the IIO chardev */
	struct iio_buffer *buffer;	/* claimed while the stream exists */
	struct iqnet_pl_cfg cfg;
	struct iqnet_pl_counters last;	/* latest successful snapshot */

	spinlock_t ctrl_lock;		/* CTRL writes and the fields below */
	/*
	 * Under ctrl_lock (active is also only changed with lock held, so
	 * holders of lock may read it without ctrl_lock). ctrl_regs is the
	 * register window of the armed device, non-NULL iff active, so the
	 * notifier never needs pd. ndev identifies the netdev of the stream
	 * (no reference held: cleared on NETDEV_UNREGISTER).
	 */
	bool active;
	u32 ctrl;			/* CTRL shadow: enable, link_up */
	void __iomem *ctrl_regs;
	struct net_device *ndev;

	struct notifier_block nb;
} iqnet_pl = {
	.lock = __MUTEX_INITIALIZER(iqnet_pl.lock),
	.ctrl_lock = __SPIN_LOCK_UNLOCKED(iqnet_pl.ctrl_lock),
	.nb = { .notifier_call = iqnet_pl_netdev_event },
};

/* ------------------------------------------------------------------------ */
/* Registers                                                                 */

static u64 iqnet_pl_read64(struct iqnet_pl_dev *pd, unsigned int off)
{
	u32 lo = readl(pd->regs + off);
	u32 hi = readl(pd->regs + off + 4);

	return (u64)hi << 32 | lo;
}

/* LO = MAC bytes 2..5 ([31:24] = byte 2), HI = bytes 0..1 ([15:8] = byte 0). */
static void iqnet_pl_write_mac(struct iqnet_pl_dev *pd, unsigned int off,
			       const u8 *mac)
{
	writel(get_unaligned_be32(&mac[2]), pd->regs + off);
	writel(get_unaligned_be16(&mac[0]), pd->regs + off + 4);
}

/* Only while CTRL.enable = 0: the streamer ignores these writes otherwise. */
static void iqnet_pl_program(struct iqnet_pl_dev *pd,
			     const struct iqnet_pl_cfg *cfg)
{
	iqnet_pl_write_mac(pd, IQNET_PL_DST_MAC, cfg->dst_mac);
	iqnet_pl_write_mac(pd, IQNET_PL_SRC_MAC, cfg->src_mac);
	/* [31:24] is the first octet: host order of the network-order value */
	writel(be32_to_cpu(cfg->src_addr), pd->regs + IQNET_PL_SRC_IP);
	writel(be32_to_cpu(cfg->dst_addr), pd->regs + IQNET_PL_DST_IP);
	writel((u32)IQNET_PL_SRC_PORT << 16 | be16_to_cpu(cfg->dst_port),
	       pd->regs + IQNET_PL_PORTS);
	writel(cfg->payload, pd->regs + IQNET_PL_PAYLOAD_LEN);
}

/*
 * Latch all counters (CTRL.snapshot), wait for STATUS.snapshot_done, then
 * read the LO/HI pairs, which stay stable until the next snapshot. Assumes
 * snapshot_done is cleared by the CTRL write that requests the snapshot.
 */
static int iqnet_pl_snapshot(struct iqnet_pl_dev *pd,
			     struct iqnet_pl_counters *c)
{
	u32 st;
	int ret;

	lockdep_assert_held(&iqnet_pl.lock);

	spin_lock(&iqnet_pl.ctrl_lock);
	writel(iqnet_pl.ctrl | IQNET_PL_CTRL_SNAPSHOT, pd->regs + IQNET_PL_CTRL);
	spin_unlock(&iqnet_pl.ctrl_lock);

	ret = readl_poll_timeout(pd->regs + IQNET_PL_STATUS, st,
				 st & IQNET_PL_STATUS_SNAPSHOT_DONE,
				 IQNET_PL_POLL_US,
				 IQNET_PL_SNAPSHOT_TIMEOUT_MS * 1000);
	if (ret)
		return ret;

	c->datagrams = iqnet_pl_read64(pd, IQNET_PL_DATAGRAMS);
	c->bytes = iqnet_pl_read64(pd, IQNET_PL_BYTES);
	c->overflows = iqnet_pl_read64(pd, IQNET_PL_OVERFLOWS);
	c->linux_frames = iqnet_pl_read64(pd, IQNET_PL_LINUX_FRAMES);
	c->linux_drops = iqnet_pl_read64(pd, IQNET_PL_LINUX_DROPS);
	c->fifo_hwm_bytes = (u64)readl(pd->regs + IQNET_PL_FIFO_HWM) *
			    IQNET_PL_WORD_BYTES;
	return 0;
}

/*
 * Not just the carrier: an administratively downed GEM may leave the PHY
 * link up, but the stream should stop with the interface.
 */
static bool iqnet_pl_link_ok(const struct net_device *ndev)
{
	return netif_running(ndev) && netif_carrier_ok(ndev);
}

/* ------------------------------------------------------------------------ */
/* START / STOP                                                              */

static int iqnet_pl_check_req(const struct iqnet_pl_start *req)
{
	__be32 dst = (__force __be32)req->dst_addr;

	if (req->flags || memchr_inv(req->reserved, 0, sizeof(req->reserved)))
		return -EINVAL;
	if (req->payload_len < IQNET_BURST ||
	    req->payload_len % IQNET_BURST ||
	    req->payload_len > IQNET_MAX_PAYLOAD)
		return -EINVAL;
	if (req->buffer_fd < 0 || !req->dst_port)
		return -EINVAL;
	/* the streamer sends to one unicast host (next-hop MAC from ARP) */
	if (ipv4_is_zeronet(dst) || ipv4_is_loopback(dst) ||
	    ipv4_is_multicast(dst) || ipv4_is_lbcast(dst))
		return -EINVAL;
	return 0;
}

/*
 * Route, addresses, link and next-hop MAC for @cfg->dst_addr. The route must
 * leave through @ndev (the GEM the streamer sits behind): the streamer
 * frames are untagged and go out of that GEM's PHY only.
 */
static int iqnet_pl_resolve(struct net_device *ndev, struct iqnet_pl_cfg *cfg)
{
	struct flowi4 fl4 = {
		.daddr = cfg->dst_addr,
		.flowi4_proto = IPPROTO_UDP,
		.fl4_sport = htons(IQNET_PL_SRC_PORT),
		.fl4_dport = cfg->dst_port,
	};
	struct ethtool_link_ksettings ks;
	struct neighbour *n;
	struct rtable *rt;
	int ret;

	ASSERT_RTNL();

	rt = ip_route_output_key(&init_net, &fl4);
	if (IS_ERR(rt)) {
		pr_err("no route to %pI4: %ld\n", &cfg->dst_addr, PTR_ERR(rt));
		return -ENETUNREACH;
	}
	if (rt->rt_type != RTN_UNICAST || rt->dst.dev != ndev) {
		pr_err("route to %pI4 is not a unicast route via %s (type %u, dev %s)\n",
		       &cfg->dst_addr, netdev_name(ndev), rt->rt_type,
		       netdev_name(rt->dst.dev));
		ret = -ENETUNREACH;
		goto out;
	}
	cfg->src_addr = fl4.saddr;
	ether_addr_copy(cfg->src_mac, ndev->dev_addr);

	/*
	 * Checked before the neighbour: at 100 Mb/s the caller would
	 * otherwise retry -EAGAIN for nothing.
	 */
	ret = __ethtool_get_link_ksettings(ndev, &ks);
	if (ret || ks.base.speed != SPEED_1000 ||
	    ks.base.duplex != DUPLEX_FULL) {
		pr_err("link of %s is not 1000/full (speed %u, duplex %u, err %d)\n",
		       netdev_name(ndev), ret ? 0 : ks.base.speed,
		       ret ? DUPLEX_UNKNOWN : ks.base.duplex, ret);
		ret = -ENETDOWN;
		goto out;
	}

	/* the next hop: the host itself or the gateway */
	n = dst_neigh_lookup(&rt->dst, &fl4.daddr);
	if (!n) {
		ret = -ENOMEM;
		goto out;
	}
	if (!(READ_ONCE(n->nud_state) & NUD_VALID)) {
		/* start (or continue) resolution; the caller retries */
		neigh_event_send(n, NULL);
		pr_info("next hop of %pI4 not resolved yet, probing\n",
			&cfg->dst_addr);
		ret = -EAGAIN;
	} else {
		neigh_ha_snapshot(cfg->dst_mac, n, ndev);
		if (!is_valid_ether_addr(cfg->dst_mac)) {
			pr_err("next hop of %pI4 has MAC %pM\n",
			       &cfg->dst_addr, cfg->dst_mac);
			ret = -ENETUNREACH;
		}
	}
	neigh_release(n);
out:
	ip_rt_put(rt);
	return ret;
}

/* Claim the IIO buffer behind @fd, like the kernel path does. */
static int iqnet_pl_claim(int fd)
{
	struct iio_buffer *buffer;
	struct file *file;
	int ret;

	file = fget(fd);
	if (!file)
		return -EBADF;

	/* the chardev itself; its release would free the blocks */
	if (!S_ISCHR(file_inode(file)->i_mode)) {
		pr_err("fd %d is not a character device\n", fd);
		ret = -EINVAL;
		goto err_file;
	}

	buffer = iio_buffer_get_from_file(file);
	if (IS_ERR_OR_NULL(buffer)) {
		ret = buffer ? PTR_ERR(buffer) : -EINVAL;
		pr_err("fd %d is not an IIO chardev with a buffer: %d\n", fd,
		       ret);
		goto err_file;
	}
	if (buffer->direction != IIO_BUFFER_DIRECTION_IN) {
		pr_err("IIO buffer is not an input buffer\n");
		ret = -EINVAL;
		goto err_buffer;
	}

	/*
	 * The DMA never gets the samples while the streamer is armed, but
	 * the claim still matters: it keeps scan elements, length and
	 * buffer/enable=0 away from other users (-EBUSY) for the whole
	 * stream, so the format the streamer sends cannot change under it.
	 */
	ret = iio_buffer_claim_blocks(buffer);
	if (ret) {
		pr_err("IIO blocks already claimed: %d\n", ret);
		goto err_buffer;
	}

	iqnet_pl.iio_file = file;
	iqnet_pl.buffer = buffer;
	return 0;

err_buffer:
	iio_buffer_put(buffer);
err_file:
	fput(file);
	return ret;
}

static void iqnet_pl_release(void)
{
	if (iqnet_pl.buffer) {
		iio_buffer_release_blocks(iqnet_pl.buffer);
		iio_buffer_put(iqnet_pl.buffer);
		iqnet_pl.buffer = NULL;
	}
	if (iqnet_pl.iio_file) {
		fput(iqnet_pl.iio_file);
		iqnet_pl.iio_file = NULL;
	}
	iqnet_pl.owner = NULL;
}

/*
 * enable = 0 and wait until the streamer has finished the chunks it
 * accepted (STATUS.armed drops). link_up is kept during the drain so the
 * framer is not cut off; CTRL is all zero afterwards either way.
 */
static int iqnet_pl_disarm(struct iqnet_pl_dev *pd)
{
	u32 st;
	int ret;

	lockdep_assert_held(&iqnet_pl.lock);

	spin_lock(&iqnet_pl.ctrl_lock);
	WRITE_ONCE(iqnet_pl.active, false);
	iqnet_pl.ctrl_regs = NULL;
	iqnet_pl.ndev = NULL;
	iqnet_pl.ctrl &= ~IQNET_PL_CTRL_ENABLE;
	writel(iqnet_pl.ctrl, pd->regs + IQNET_PL_CTRL);
	spin_unlock(&iqnet_pl.ctrl_lock);

	ret = readl_poll_timeout(pd->regs + IQNET_PL_STATUS, st,
				 !(st & IQNET_PL_STATUS_ARMED),
				 IQNET_PL_POLL_US,
				 IQNET_PL_DISARM_TIMEOUT_MS * 1000);
	if (ret)
		dev_err(pd->dev, "streamer still armed %u ms after enable=0 (STATUS %#x)\n",
			IQNET_PL_DISARM_TIMEOUT_MS, st);

	spin_lock(&iqnet_pl.ctrl_lock);
	iqnet_pl.ctrl = 0;
	writel(0, pd->regs + IQNET_PL_CTRL);
	spin_unlock(&iqnet_pl.ctrl_lock);

	return ret;
}

int iqnet_pl_start(const struct iqnet_pl_start *req, struct file *owner)
{
	struct iqnet_pl_cfg cfg = {
		.dst_addr = (__force __be32)req->dst_addr,
		.dst_port = (__force __be16)req->dst_port,
		.payload = req->payload_len,
	};
	struct net_device *ndev = NULL;
	struct iqnet_pl_dev *pd;
	bool link;
	u32 st;
	int ret;

	ret = iqnet_pl_check_req(req);
	if (ret)
		return ret;

	mutex_lock(&iqnet_pl.lock);
	pd = iqnet_pl.pd;
	if (!pd) {
		ret = -ENODEV;
		goto out;
	}
	if (iqnet_pl.active) {
		ret = -EBUSY;
		goto out;
	}

	/* looked up per START: the GEM may have been re-bound since probe */
	ndev = of_find_net_device_by_node(pd->eth_np);
	if (!ndev) {
		pr_err("no netdev for %pOF\n", pd->eth_np);
		ret = -ENETUNREACH;
		goto out;
	}

	rtnl_lock();
	ret = iqnet_pl_resolve(ndev, &cfg);
	rtnl_unlock();
	if (ret)
		goto out;

	ret = iqnet_pl_claim(req->buffer_fd);
	if (ret)
		goto out;

	st = readl(pd->regs + IQNET_PL_STATUS);
	if (st & IQNET_PL_STATUS_ARMED) {
		dev_err(pd->dev, "streamer still armed from the previous stream (STATUS %#x)\n",
			st);
		ret = -EBUSY;
		goto out_release;
	}

	/* enable is 0 here, so the streamer takes these */
	iqnet_pl_program(pd, &cfg);
	/* before arming: the notifier logs cfg.src_mac once active */
	iqnet_pl.cfg = cfg;

	/*
	 * Under RTNL, so no carrier change (notifier) can fall between
	 * reading the carrier and arming: link_up is right from the first
	 * chunk. Without link_up the streamer drops every chunk.
	 */
	rtnl_lock();
	if (ndev->reg_state != NETREG_REGISTERED) {
		rtnl_unlock();
		pr_err("%s went away during START\n", netdev_name(ndev));
		ret = -ENETUNREACH;
		goto out_release;
	}
	link = iqnet_pl_link_ok(ndev);
	spin_lock(&iqnet_pl.ctrl_lock);
	iqnet_pl.ctrl = IQNET_PL_CTRL_ENABLE |
			(link ? IQNET_PL_CTRL_LINK_UP : 0);
	iqnet_pl.ctrl_regs = pd->regs;
	iqnet_pl.ndev = ndev;
	WRITE_ONCE(iqnet_pl.active, true);
	writel(iqnet_pl.ctrl, pd->regs + IQNET_PL_CTRL);
	spin_unlock(&iqnet_pl.ctrl_lock);
	rtnl_unlock();

	/*
	 * Userspace enables the IIO buffer right after we return; the
	 * streamer must already own the packer output then, or the first
	 * words go to the DMA and the CS12 burst phase is lost.
	 */
	ret = readl_poll_timeout(pd->regs + IQNET_PL_STATUS, st,
				 st & IQNET_PL_STATUS_ARMED,
				 IQNET_PL_POLL_US,
				 IQNET_PL_ARM_TIMEOUT_MS * 1000);
	if (ret) {
		dev_err(pd->dev, "streamer did not arm within %u ms (STATUS %#x)\n",
			IQNET_PL_ARM_TIMEOUT_MS, st);
		iqnet_pl_disarm(pd);
		ret = -EIO;
		goto out_release;
	}

	iqnet_pl.owner = owner;
	memset(&iqnet_pl.last, 0, sizeof(iqnet_pl.last));

	pr_info("started: dst=%pI4:%u (%pM) src=%pI4:%u (%pM) dev=%s payload=%u link_up=%d status=%#x\n",
		&cfg.dst_addr, be16_to_cpu(cfg.dst_port), cfg.dst_mac,
		&cfg.src_addr, IQNET_PL_SRC_PORT, cfg.src_mac,
		netdev_name(ndev), cfg.payload, link, st);
	goto out;

out_release:
	iqnet_pl_release();
out:
	if (ndev)
		put_device(&ndev->dev);
	mutex_unlock(&iqnet_pl.lock);
	return ret;
}

/* Called with iqnet_pl.lock held and a stream active. */
static int iqnet_pl_stop_locked(void)
{
	struct iqnet_pl_dev *pd = iqnet_pl.pd;
	struct iqnet_pl_counters *c = &iqnet_pl.last;
	int ret;

	lockdep_assert_held(&iqnet_pl.lock);

	ret = iqnet_pl_disarm(pd);

	/* after the drain, so the last datagrams are counted */
	if (iqnet_pl_snapshot(pd, c))
		dev_warn(pd->dev, "final counter snapshot timed out, STATS keeps the previous one\n");

	iqnet_pl_release();

	pr_info("stopped: datagrams=%llu bytes=%llu overflows=%llu linux_frames=%llu linux_drops=%llu fifo_hwm=%llu\n",
		c->datagrams, c->bytes, c->overflows, c->linux_frames,
		c->linux_drops, c->fifo_hwm_bytes);

	return ret ? -ETIMEDOUT : 0;
}

int iqnet_pl_stop(struct file *owner)
{
	int ret = 0;

	mutex_lock(&iqnet_pl.lock);
	if (iqnet_pl.active && (!owner || iqnet_pl.owner == owner))
		ret = iqnet_pl_stop_locked();
	mutex_unlock(&iqnet_pl.lock);
	return ret;
}

int iqnet_pl_read_counters(struct iqnet_pl_counters *c, bool *running)
{
	int ret = 0;

	mutex_lock(&iqnet_pl.lock);
	*running = iqnet_pl.active;
	if (iqnet_pl.active) {
		ret = iqnet_pl_snapshot(iqnet_pl.pd, &iqnet_pl.last);
		if (ret)
			pr_warn_ratelimited("counter snapshot timed out\n");
	}
	*c = iqnet_pl.last;
	mutex_unlock(&iqnet_pl.lock);
	return ret;
}

bool iqnet_pl_present(void)
{
	return !!READ_ONCE(iqnet_pl.pd);
}

bool iqnet_pl_active(void)
{
	return READ_ONCE(iqnet_pl.active);
}

/* ------------------------------------------------------------------------ */
/* Link tracking                                                             */

/*
 * Keep CTRL.link_up equal to the carrier of the stream's netdev. Addresses
 * are fixed at START (data-model.md): a MAC change is only logged and
 * applies to the next START.
 */
static int iqnet_pl_netdev_event(struct notifier_block *nb,
				 unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	u32 ctrl;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEADDR:
	case NETDEV_UNREGISTER:
		break;
	default:
		return NOTIFY_DONE;
	}

	spin_lock(&iqnet_pl.ctrl_lock);
	if (!iqnet_pl.active || dev != iqnet_pl.ndev)
		goto out;

	switch (event) {
	case NETDEV_CHANGEADDR:
		netdev_info(dev, "MAC address is now %pM; the PL stream keeps sending from %pM until the next START\n",
			    dev->dev_addr, iqnet_pl.cfg.src_mac);
		goto out;
	case NETDEV_UNREGISTER:
		/* we hold no reference: forget it, link_up stays 0 */
		iqnet_pl.ndev = NULL;
		ctrl = iqnet_pl.ctrl & ~IQNET_PL_CTRL_LINK_UP;
		netdev_warn(dev, "unregistered during the PL stream; payloads are dropped until STOP\n");
		break;
	default:
		ctrl = iqnet_pl.ctrl & ~IQNET_PL_CTRL_LINK_UP;
		if (iqnet_pl_link_ok(dev))
			ctrl |= IQNET_PL_CTRL_LINK_UP;
		break;
	}

	if (ctrl != iqnet_pl.ctrl) {
		iqnet_pl.ctrl = ctrl;
		writel(ctrl, iqnet_pl.ctrl_regs + IQNET_PL_CTRL);
		netdev_info(dev, "PL streamer link_up=%d\n",
			    !!(ctrl & IQNET_PL_CTRL_LINK_UP));
	}
out:
	spin_unlock(&iqnet_pl.ctrl_lock);
	return NOTIFY_DONE;
}

/* ------------------------------------------------------------------------ */
/* Platform driver                                                           */

static void iqnet_pl_put_node(void *np)
{
	of_node_put(np);
}

static int iqnet_pl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_desc *presence;
	struct iqnet_pl_dev *pd;
	u32 id, st;
	int ret;

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pd->dev = dev;

	/*
	 * First the presence GPIO: with the stock bitstream nothing decodes
	 * our reg window, and any access to it is an external abort.
	 */
	presence = devm_gpiod_get(dev, "presence", GPIOD_IN);
	if (IS_ERR(presence))
		return dev_err_probe(dev, PTR_ERR(presence),
				     "cannot get presence-gpios\n");
	ret = gpiod_get_value_cansleep(presence);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot read presence-gpios\n");
	if (!ret) {
		dev_info(dev, "no PL streamer in the loaded bitstream\n");
		return -ENODEV;
	}

	pd->eth_np = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!pd->eth_np)
		return dev_err_probe(dev, -EINVAL, "no ethernet phandle\n");
	ret = devm_add_action_or_reset(dev, iqnet_pl_put_node, pd->eth_np);
	if (ret)
		return ret;

	pd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pd->regs))
		return PTR_ERR(pd->regs);

	id = readl(pd->regs + IQNET_PL_ID);
	if (id != IQNET_PL_ID_VALUE) {
		dev_err(dev, "presence GPIO is set but ID is %#010x, not %#010x\n",
			id, IQNET_PL_ID_VALUE);
		return -ENODEV;
	}
	pd->version = readl(pd->regs + IQNET_PL_VERSION);
	if (pd->version >> 16 != IQNET_PL_VERSION_MAJOR) {
		dev_err(dev, "unsupported streamer version %u.%u\n",
			pd->version >> 16, pd->version & 0xffff);
		return -ENODEV;
	}
	pd->fifo_depth = readl(pd->regs + IQNET_PL_FIFO_DEPTH);

	/* nothing can be armed by us yet: a leftover from an earlier load */
	st = readl(pd->regs + IQNET_PL_STATUS);
	if (st & IQNET_PL_STATUS_ARMED)
		dev_warn(dev, "streamer was armed at probe, disarming\n");
	writel(0, pd->regs + IQNET_PL_CTRL);

	mutex_lock(&iqnet_pl.lock);
	if (iqnet_pl.pd) {
		mutex_unlock(&iqnet_pl.lock);
		dev_err(dev, "a PL streamer is already bound\n");
		return -EBUSY;
	}
	WRITE_ONCE(iqnet_pl.pd, pd);
	mutex_unlock(&iqnet_pl.lock);

	platform_set_drvdata(pdev, pd);
	dev_info(dev, "PL streamer %u.%u, data FIFO %u words (%u bytes), via %pOF\n",
		 pd->version >> 16, pd->version & 0xffff, pd->fifo_depth,
		 pd->fifo_depth * IQNET_PL_WORD_BYTES, pd->eth_np);
	return 0;
}

static void iqnet_pl_remove(struct platform_device *pdev)
{
	struct iqnet_pl_dev *pd = platform_get_drvdata(pdev);

	mutex_lock(&iqnet_pl.lock);
	if (iqnet_pl.active) {
		dev_warn(pd->dev, "unbound during a PL stream, stopping it\n");
		iqnet_pl_stop_locked();
	}
	/* the reg window is unmapped after we return */
	WRITE_ONCE(iqnet_pl.pd, NULL);
	mutex_unlock(&iqnet_pl.lock);
}

static const struct of_device_id iqnet_pl_of_match[] = {
	{ .compatible = "plutosdr-learn,iqnet-pl-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, iqnet_pl_of_match);

static struct platform_driver iqnet_pl_driver = {
	.probe = iqnet_pl_probe,
	.remove = iqnet_pl_remove,
	.driver = {
		.name = "iqnet-pl",
		.of_match_table = iqnet_pl_of_match,
	},
};

int iqnet_pl_init(void)
{
	int ret;

	ret = register_netdevice_notifier(&iqnet_pl.nb);
	if (ret)
		return ret;

	/* a missing streamer is not an error: probe() returns -ENODEV */
	ret = platform_driver_register(&iqnet_pl_driver);
	if (ret)
		unregister_netdevice_notifier(&iqnet_pl.nb);
	return ret;
}

void iqnet_pl_exit(void)
{
	/* remove() disarms the streamer if a stream is still active */
	platform_driver_unregister(&iqnet_pl_driver);
	unregister_netdevice_notifier(&iqnet_pl.nb);
}
