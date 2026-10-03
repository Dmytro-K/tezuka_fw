// SPDX-License-Identifier: MIT
/*
 * iqnetd - control daemon for zero-copy UDP IQ streaming (iqnet).
 *
 * Listens on TCP port IQNET_CTRL_PORT and implements the control protocol
 * described in iqnet_proto.h (one client at a time). On START it prepares
 * the RX IIO buffer of "cf-ad9361-lpc" (scan elements, legacy mmap block
 * API: alloc + enqueue every block), hands it over to the iqnet kernel
 * module via IQNET_IOC_START (iqnet_uapi.h) and only then sets
 * buffer/enable=1, so the first DMA block already goes to the network. The
 * module streams the DMA blocks to <TCP peer>:<udp_port> without any CPU
 * copy, from the local address the control connection arrived on. STOP,
 * closing the TCP connection or SIGTERM tear the stream down again.
 *
 * Usage: iqnetd [-f] [-p ctrl_port] [-F iqnet_flags]
 *   -f  stay in foreground (also log to stderr)
 *   -p  TCP control port (default IQNET_CTRL_PORT)
 *   -F  debug: IQNET_F_* flags passed to IQNET_IOC_START
 *       (1 = IQNET_F_NO_ZEROCOPY, 2 = IQNET_F_SYNC_CACHE)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "iqnet_uapi.h"
#include "iqnet_proto.h"

/*
 * Legacy IIO block API (CONFIG_IIO_DMA_BUF_MMAP_LEGACY). Not exported in
 * the kernel uapi headers, copied from include/linux/iio/buffer_impl.h.
 * Only the struct sizes enter the ioctl numbers, so the names are local.
 */
struct legacy_block_alloc_req {
	uint32_t type;
	uint32_t size;
	uint32_t count;
	uint32_t id;
};

struct legacy_block {
	uint32_t id;
	uint32_t size;
	uint32_t bytes_used;
	uint32_t type;
	uint32_t flags;
	union {
		uint32_t offset;
	} data;
	uint64_t timestamp;
};

_Static_assert(sizeof(struct legacy_block_alloc_req) == 16, "alloc_req ABI");
_Static_assert(sizeof(struct legacy_block) == 32, "iio_buffer_block ABI");

#define IIO_BLOCK_ALLOC_IOCTL	_IOWR('i', 0xa0, struct legacy_block_alloc_req)
#define IIO_BLOCK_FREE_IOCTL	_IO('i', 0xa1)
#define IIO_BLOCK_QUERY_IOCTL	_IOWR('i', 0xa2, struct legacy_block)
#define IIO_BLOCK_ENQUEUE_IOCTL	_IOWR('i', 0xa3, struct legacy_block)
#define IIO_BLOCK_DEQUEUE_IOCTL	_IOWR('i', 0xa4, struct legacy_block)

#define IIO_SYSFS_DIR	"/sys/bus/iio/devices"
#define IIO_RX_NAME	"cf-ad9361-lpc"

#define MAX_BLOCK_SIZE	(16u << 20)	/* iio_dma_buffer_max_block_size default */

/* one UDP_SEGMENT super-datagram must fit the IPv4 total length limit */
_Static_assert(IQNET_MAX_GSO * (IQNET_HDR_LEN + IQNET_DEFAULT_PAYLOAD) <=
	       65535 - 20 - 8, "IQNET_MAX_GSO too large for UDP GSO");

#define LINE_MAX_LEN	256
#define ERR_LEN		256

struct mode_desc {
	const char *name;
	const char *enable[2];	/* scan elements to enable, NULL terminated */
	unsigned int bytes_per_datum;
};

static const struct mode_desc modes[] = {
	{ "cs12", { "in_voltage1_en", NULL }, 2 },
	{ "cs8",  { "in_voltage0_en", NULL }, 2 },
	{ "cs16", { "in_voltage0_en", "in_voltage1_en" }, 4 },
};

struct start_req {
	unsigned int udp_port;
	const struct mode_desc *mode;
	unsigned int blocks;
	unsigned int block_size;
	unsigned int gso;
};

struct stream {
	int active;		/* IQNET_IOC_START succeeded */
	int iio_fd;
	int iqnet_fd;
	int buf_enabled;	/* we wrote buffer/enable=1 */
	int blocks_alloc;	/* IIO_BLOCK_ALLOC_IOCTL succeeded */
	char devdir[128];	/* /sys/bus/iio/devices/iio:deviceN */
	char devnode[64];	/* /dev/iio:deviceN */
	struct start_req req;
	struct in_addr dst;
	struct in_addr src;
};

struct client {
	int fd;			/* -1 = no client */
	struct in_addr peer;
	char line[LINE_MAX_LEN];
	size_t line_len;
	int line_overflow;
};

static volatile sig_atomic_t g_quit;
static int g_foreground;
static unsigned int g_iqnet_flags;

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

/* ---------------------------------------------------------------- sysfs */

static int sysfs_write(const char *path, const char *val)
{
	size_t len = strlen(val);
	ssize_t n;
	int fd, err = 0;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		n = write(fd, val, len);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		err = -errno;
	else if ((size_t)n != len)
		err = -EIO;
	close(fd);
	return err;
}

static int sysfs_read(const char *path, char *buf, size_t len)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	do {
		n = read(fd, buf, len - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		int err = -errno;

		close(fd);
		return err;
	}
	close(fd);
	buf[n] = '\0';
	while (n > 0 && isspace((unsigned char)buf[n - 1]))
		buf[--n] = '\0';
	return 0;
}

static int devattr_write(const struct stream *s, const char *attr,
			 const char *val)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", s->devdir, attr);
	return sysfs_write(path, val);
}

/* find iio:deviceN whose name is IIO_RX_NAME; returns N or -errno */
static int find_rx_device(void)
{
	struct dirent *de;
	DIR *d;
	int found = -ENODEV;

	d = opendir(IIO_SYSFS_DIR);
	if (!d)
		return -errno;
	while ((de = readdir(d)) != NULL) {
		char path[300], name[64];
		char *end;
		long n;

		if (strncmp(de->d_name, "iio:device", 10) != 0)
			continue;
		n = strtol(de->d_name + 10, &end, 10);
		if (*end != '\0' || end == de->d_name + 10 || n < 0 ||
		    n > INT_MAX)
			continue;
		snprintf(path, sizeof(path), IIO_SYSFS_DIR "/%s/name",
			 de->d_name);
		if (sysfs_read(path, name, sizeof(name)) != 0)
			continue;
		if (strcmp(name, IIO_RX_NAME) == 0) {
			found = (int)n;
			break;
		}
	}
	closedir(d);
	return found;
}

/* disable every scan element, then enable the ones of the mode */
static int set_scan_elements(const struct stream *s,
			     const struct mode_desc *m, char *err, size_t errlen)
{
	char dir[200];
	struct dirent *de;
	unsigned int i;
	DIR *d;
	int ret;

	snprintf(dir, sizeof(dir), "%s/scan_elements", s->devdir);
	d = opendir(dir);
	if (!d) {
		snprintf(err, errlen, "cannot open %s/scan_elements: %s",
			 s->devnode, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		char path[460];

		if (len < 4 || strcmp(de->d_name + len - 3, "_en") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		ret = sysfs_write(path, "0");
		if (ret) {
			snprintf(err, errlen, "cannot clear %s: %s",
				 de->d_name, strerror(-ret));
			closedir(d);
			return -1;
		}
	}
	closedir(d);

	for (i = 0; i < 2 && m->enable[i]; i++) {
		char path[460];

		snprintf(path, sizeof(path), "%s/%s", dir, m->enable[i]);
		ret = sysfs_write(path, "1");
		if (ret) {
			snprintf(err, errlen, "cannot set %s: %s",
				 m->enable[i], strerror(-ret));
			return -1;
		}
	}
	return 0;
}

/* --------------------------------------------------------------- stream */

static void stream_init(struct stream *s)
{
	memset(s, 0, sizeof(*s));
	s->iio_fd = -1;
	s->iqnet_fd = -1;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, req, arg);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

/*
 * Error text for a failed IIO block ioctl. EBUSY means the blocks of the
 * buffer are claimed (iio_buffer_claim_blocks(): an iqnet stream owns
 * them) or the buffer is in use/enabled by someone else (iiod or another
 * libiio client), never a transient condition worth retrying blindly.
 */
static void block_ioctl_err(const struct stream *s, const char *what, int e,
			    char *err, size_t errlen)
{
	if (e == EBUSY)
		snprintf(err, errlen,
			 "busy: %s on %s: IIO blocks in use (claimed by a running iqnet stream, or iiod/libiio client holding the buffer)",
			 what, s->devnode);
	else
		snprintf(err, errlen, "%s on %s: %s", what, s->devnode,
			 strerror(e));
}

/*
 * Tear down whatever stream_start() set up. Safe on a partial setup.
 * IQNET_IOC_STOP comes first: until it returns the module owns the blocks
 * and buffer/enable must not be touched (iqnet_uapi.h).
 *
 * Returns 0, or -errno if IQNET_IOC_STOP failed. -ETIMEDOUT means the
 * in-flight skbs did not drain in time (IQNET_DRAIN_TIMEOUT): the module
 * detached the stream, which keeps the IIO blocks claimed and the IIO
 * chardev open until they drain. The rest of the teardown still runs
 * (buffer/enable=0 stops the DMA; closing our fds is harmless, the module
 * holds its own file reference).
 */
static int stream_stop(struct stream *s)
{
	int ret, stop_err = 0;

	if (s->active) {
		syslog(LOG_INFO, "stopping stream to %s:%u",
		       inet_ntoa(s->dst), s->req.udp_port);
		/* blocks until every in-flight block is back in IIO */
		if (xioctl(s->iqnet_fd, IQNET_IOC_STOP, NULL) < 0) {
			stop_err = -errno;
			if (errno == ETIMEDOUT)
				syslog(LOG_ERR,
				       "IQNET_IOC_STOP timed out: stream detached, IIO blocks stay claimed until in-flight skbs drain (see dmesg)");
			else
				syslog(LOG_ERR, "IQNET_IOC_STOP: %s",
				       strerror(errno));
		}
		s->active = 0;
	}
	if (s->iqnet_fd >= 0) {
		close(s->iqnet_fd);
		s->iqnet_fd = -1;
	}
	if (s->buf_enabled) {
		ret = devattr_write(s, "buffer/enable", "0");
		if (ret)
			syslog(LOG_ERR, "%s/buffer/enable=0: %s", s->devdir,
			       strerror(-ret));
		s->buf_enabled = 0;
	}
	/* a detached stream still claims the blocks: FREE would be -EBUSY */
	if (s->blocks_alloc && !stop_err) {
		if (xioctl(s->iio_fd, IIO_BLOCK_FREE_IOCTL, NULL) < 0)
			syslog(LOG_ERR, "IIO_BLOCK_FREE_IOCTL: %s",
			       strerror(errno));
	}
	s->blocks_alloc = 0;
	if (s->iio_fd >= 0) {
		/* release also frees the blocks in the kernel */
		close(s->iio_fd);
		s->iio_fd = -1;
	}
	return stop_err;
}

static int stream_stats(const struct stream *s, struct iqnet_stats *st,
			char *err, size_t errlen)
{
	int fd = s->iqnet_fd, ret = 0;

	if (fd < 0) {
		fd = open(IQNET_DEV, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			snprintf(err, errlen,
				 "cannot open %s: %s%s", IQNET_DEV,
				 strerror(errno),
				 errno == ENOENT ? " (iqnet module not loaded?)" : "");
			return -1;
		}
	}
	memset(st, 0, sizeof(*st));
	if (xioctl(fd, IQNET_IOC_STATS, st) < 0) {
		snprintf(err, errlen, "IQNET_IOC_STATS: %s", strerror(errno));
		ret = -1;
	}
	if (fd != s->iqnet_fd)
		close(fd);
	return ret;
}

/*
 * Is a detached (zombie) iqnet stream still draining? It keeps the IIO
 * chardev open, so open() of it fails with EBUSY until its in-flight skbs
 * are freed. Uses a temporary /dev/iqnet fd; false if that is not possible.
 */
static int iqnet_draining(void)
{
	struct stream tmp;
	struct iqnet_stats st;
	char err[ERR_LEN];

	stream_init(&tmp);
	if (stream_stats(&tmp, &st, err, sizeof(err)))
		return 0;
	return !st.running && st.blocks_inflight > 0;
}

static int stream_start(struct stream *s, const struct start_req *rq,
			struct in_addr dst, struct in_addr src, char *err,
			size_t errlen)
{
	struct legacy_block_alloc_req areq;
	struct iqnet_start st;
	char path[200];
	char val[64];
	unsigned int i;
	int dev, ret;

	stream_init(s);
	s->req = *rq;
	s->dst = dst;
	s->src = src;

	dev = find_rx_device();
	if (dev < 0) {
		snprintf(err, errlen, "IIO device %s not found: %s",
			 IIO_RX_NAME, strerror(-dev));
		syslog(LOG_ERR, "START failed: %s", err);
		return -1;
	}
	snprintf(s->devdir, sizeof(s->devdir), IIO_SYSFS_DIR "/iio:device%d",
		 dev);
	snprintf(s->devnode, sizeof(s->devnode), "/dev/iio:device%d", dev);

	/*
	 * Open the char device first: it is exclusive (EBUSY while iiod or
	 * another libiio user streams), so we never disturb a running stream
	 * of someone else by touching buffer/enable or the scan elements.
	 */
	s->iio_fd = open(s->devnode, O_RDWR | O_CLOEXEC);
	if (s->iio_fd < 0) {
		int e = errno;

		if (e == EBUSY && iqnet_draining())
			snprintf(err, errlen,
				 "busy: previous iqnet stream still draining (see dmesg)");
		else if (e == EBUSY)
			snprintf(err, errlen,
				 "busy: %s is in use by another process (iiod/libiio client streaming?)",
				 s->devnode);
		else
			snprintf(err, errlen, "cannot open %s: %s",
				 s->devnode, strerror(e));
		goto fail;
	}

	snprintf(path, sizeof(path), "%s/buffer/enable", s->devdir);
	if (sysfs_read(path, val, sizeof(val)) == 0 && strcmp(val, "0") != 0) {
		syslog(LOG_WARNING, "%s: buffer was enabled, disabling",
		       s->devdir);
		ret = devattr_write(s, "buffer/enable", "0");
		if (ret) {
			snprintf(err, errlen, "cannot disable buffer: %s",
				 strerror(-ret));
			goto fail;
		}
	}

	if (set_scan_elements(s, rq->mode, err, errlen))
		goto fail;

	/* not used by the block API, but keep it consistent like libiio */
	snprintf(val, sizeof(val), "%u",
		 rq->block_size / rq->mode->bytes_per_datum);
	ret = devattr_write(s, "buffer/length", val);
	if (ret)
		syslog(LOG_WARNING, "buffer/length=%s: %s", val,
		       strerror(-ret));

	/* drop stale blocks (none expected: release frees them) */
	if (xioctl(s->iio_fd, IIO_BLOCK_FREE_IOCTL, NULL) < 0) {
		block_ioctl_err(s, "IIO_BLOCK_FREE_IOCTL", errno, err, errlen);
		goto fail;
	}

	memset(&areq, 0, sizeof(areq));
	areq.size = rq->block_size;
	areq.count = rq->blocks;
	if (xioctl(s->iio_fd, IIO_BLOCK_ALLOC_IOCTL, &areq) < 0) {
		if (errno == ENOTTY || errno == ENOSYS || errno == ENODEV)
			snprintf(err, errlen,
				 "IIO_BLOCK_ALLOC_IOCTL unsupported (%s): kernel without CONFIG_IIO_DMA_BUF_MMAP_LEGACY?",
				 strerror(errno));
		else
			block_ioctl_err(s, "IIO_BLOCK_ALLOC_IOCTL", errno, err,
					errlen);
		goto fail;
	}
	s->blocks_alloc = 1;
	if (areq.count < rq->blocks || areq.size != rq->block_size) {
		snprintf(err, errlen,
			 "only %u of %u blocks of %u bytes allocated (got size %u; CMA exhausted?)",
			 areq.count, rq->blocks, rq->block_size, areq.size);
		goto fail;
	}

	for (i = 0; i < areq.count; i++) {
		struct legacy_block b;

		memset(&b, 0, sizeof(b));
		b.id = areq.id + i;
		b.size = rq->block_size;
		b.bytes_used = rq->block_size;
		if (xioctl(s->iio_fd, IIO_BLOCK_ENQUEUE_IOCTL, &b) < 0) {
			char what[48];

			snprintf(what, sizeof(what),
				 "IIO_BLOCK_ENQUEUE_IOCTL(%u)", b.id);
			block_ioctl_err(s, what, errno, err, errlen);
			goto fail;
		}
	}

	s->iqnet_fd = open(IQNET_DEV, O_RDWR | O_CLOEXEC);
	if (s->iqnet_fd < 0) {
		if (errno == ENOENT || errno == ENXIO || errno == ENODEV)
			snprintf(err, errlen,
				 "cannot open %s: %s (iqnet module not loaded?)",
				 IQNET_DEV, strerror(errno));
		else
			snprintf(err, errlen, "cannot open %s: %s", IQNET_DEV,
				 strerror(errno));
		goto fail;
	}

	memset(&st, 0, sizeof(st));
	st.buffer_fd = s->iio_fd;
	st.dst_addr = dst.s_addr;
	st.dst_port = htons((uint16_t)rq->udp_port);
	st.payload_len = IQNET_DEFAULT_PAYLOAD;
	st.gso_segs = rq->gso;
	st.flags = g_iqnet_flags;
	st.src_addr = src.s_addr;
	/* st.reserved[] stays zero (memset) */
	if (xioctl(s->iqnet_fd, IQNET_IOC_START, &st) < 0) {
		int e = errno;
		struct iqnet_stats sst;

		memset(&sst, 0, sizeof(sst));
		if (e == EBUSY && xioctl(s->iqnet_fd, IQNET_IOC_STATS, &sst) == 0 &&
		    sst.running)
			snprintf(err, errlen,
				 "busy: iqnet module already streaming (another iqnetd/client?)");
		else if (e == EBUSY && sst.blocks_inflight > 0)
			snprintf(err, errlen,
				 "busy: previous iqnet stream still draining (see dmesg)");
		else if (e == EBUSY)
			snprintf(err, errlen,
				 "busy: IIO blocks of %s are already claimed (iiod/libiio client or stale iqnet user?)",
				 s->devnode);
		else if (e == EADDRNOTAVAIL)
			snprintf(err, errlen,
				 "IQNET_IOC_START: source address %s not usable: %s",
				 inet_ntoa(src), strerror(e));
		else
			snprintf(err, errlen, "IQNET_IOC_START: %s",
				 strerror(e));
		goto fail;
	}
	s->active = 1;

	/* enable only now: no DMA block completes before the module owns them */
	ret = devattr_write(s, "buffer/enable", "1");
	if (ret) {
		snprintf(err, errlen, "cannot enable buffer: %s",
			 strerror(-ret));
		goto fail;
	}
	s->buf_enabled = 1;

	{
		char sa[INET_ADDRSTRLEN], da[INET_ADDRSTRLEN];

		inet_ntop(AF_INET, &src, sa, sizeof(sa));
		inet_ntop(AF_INET, &dst, da, sizeof(da));
		syslog(LOG_INFO,
		       "streaming %s from %s, %s -> %s:%u: %u blocks x %u B, payload %u, gso %u, flags 0x%x",
		       rq->mode->name, s->devnode, sa, da, rq->udp_port,
		       rq->blocks, rq->block_size, IQNET_DEFAULT_PAYLOAD,
		       rq->gso, g_iqnet_flags);
	}
	return 0;

fail:
	syslog(LOG_ERR, "START failed: %s", err);
	stream_stop(s);
	return -1;
}

/* ------------------------------------------------------------- protocol */

static void reply(int fd, const char *fmt, ...)
{
	char buf[512];
	const char *p = buf;
	va_list ap;
	size_t len;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 2;
	buf[len++] = '\n';
	buf[len] = '\0';

	while (len > 0) {
		ssize_t w = send(fd, p, len, MSG_NOSIGNAL);

		if (w < 0) {
			if (errno == EINTR && !g_quit)
				continue;
			/*
			 * Peer gone, or not reading (EAGAIN after SO_SNDTIMEO):
			 * drop the reply, the next recv()/poll() reports it.
			 */
			return;
		}
		p += w;
		len -= (size_t)w;
	}
}

static int parse_uint(const char *str, unsigned int min, unsigned int max,
		      unsigned int *out)
{
	unsigned long v;
	char *end;

	if (!str || !*str || !isdigit((unsigned char)*str))
		return -1;
	errno = 0;
	v = strtoul(str, &end, 10);
	if (errno || *end != '\0' || v < min || v > max)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

/* START <udp_port> <mode> [blocks=] [block_size=] [gso=] */
static int parse_start(char *args, struct start_req *rq, char *err,
		       size_t errlen)
{
	char *save = NULL, *tok;
	unsigned int i;

	memset(rq, 0, sizeof(*rq));
	rq->blocks = IQNET_DEFAULT_BLOCKS;
	rq->block_size = IQNET_DEFAULT_BLOCK_SIZE;
	rq->gso = 0;

	tok = strtok_r(args, " \t", &save);
	if (parse_uint(tok, 1, 65535, &rq->udp_port)) {
		snprintf(err, errlen, "bad or missing udp_port");
		return -1;
	}
	tok = strtok_r(NULL, " \t", &save);
	if (!tok) {
		snprintf(err, errlen, "missing mode (cs12|cs8|cs16)");
		return -1;
	}
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		if (strcasecmp(tok, modes[i].name) == 0)
			rq->mode = &modes[i];
	if (!rq->mode) {
		snprintf(err, errlen, "bad mode '%s' (cs12|cs8|cs16)", tok);
		return -1;
	}

	while ((tok = strtok_r(NULL, " \t", &save)) != NULL) {
		char *eq = strchr(tok, '=');
		const char *key = tok, *val;

		if (!eq) {
			snprintf(err, errlen, "bad option '%s'", tok);
			return -1;
		}
		*eq = '\0';
		val = eq + 1;
		if (strcmp(key, "blocks") == 0) {
			if (parse_uint(val, 1, IQNET_MAX_BLOCKS,
				       &rq->blocks)) {
				snprintf(err, errlen, "bad blocks (1..%u)",
					 IQNET_MAX_BLOCKS);
				return -1;
			}
		} else if (strcmp(key, "block_size") == 0) {
			if (parse_uint(val, 1, MAX_BLOCK_SIZE,
				       &rq->block_size)) {
				snprintf(err, errlen,
					 "bad block_size (1..%u)",
					 MAX_BLOCK_SIZE);
				return -1;
			}
		} else if (strcmp(key, "gso") == 0) {
			if (parse_uint(val, 0, IQNET_MAX_GSO, &rq->gso)) {
				snprintf(err, errlen, "bad gso (0..%u)",
					 IQNET_MAX_GSO);
				return -1;
			}
		} else {
			snprintf(err, errlen, "unknown option '%s'", key);
			return -1;
		}
	}

	if (rq->block_size % IQNET_DEFAULT_PAYLOAD) {
		snprintf(err, errlen,
			 "block_size %u is not a multiple of payload_len %u",
			 rq->block_size, IQNET_DEFAULT_PAYLOAD);
		return -1;
	}
	if (rq->block_size % rq->mode->bytes_per_datum) {
		snprintf(err, errlen,
			 "block_size %u is not a multiple of %u (sample size)",
			 rq->block_size, rq->mode->bytes_per_datum);
		return -1;
	}
	return 0;
}

static void handle_line(int cfd, char *line, struct stream *s,
			struct in_addr peer)
{
	char err[ERR_LEN];
	char *cmd, *args;

	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '\0')
		return;
	cmd = line;
	args = cmd + strcspn(cmd, " \t");
	if (*args)
		*args++ = '\0';

	if (strcasecmp(cmd, "START") == 0) {
		struct sockaddr_in la;
		socklen_t lalen = sizeof(la);
		struct in_addr src = { 0 };
		struct start_req rq;

		if (s->active) {
			reply(cfd, "ERR already running (send STOP first)");
			return;
		}
		if (parse_start(args, &rq, err, sizeof(err))) {
			reply(cfd, "ERR %s", err);
			return;
		}
		/*
		 * Send from the address the host reached us on, so the UDP
		 * stream leaves the same interface as the control link.
		 */
		memset(&la, 0, sizeof(la));
		if (getsockname(cfd, (struct sockaddr *)&la, &lalen) == 0 &&
		    la.sin_family == AF_INET)
			src = la.sin_addr;
		if (stream_start(s, &rq, peer, src, err, sizeof(err))) {
			reply(cfd, "ERR %s", err);
			return;
		}
		reply(cfd, "OK %u %u", IQNET_DEFAULT_PAYLOAD, rq.block_size);
	} else if (strcasecmp(cmd, "STOP") == 0) {
		int ret = stream_stop(s);

		if (ret == -ETIMEDOUT)
			reply(cfd,
			      "ERR stop timed out: stream detached, blocks stay claimed until in-flight skbs drain");
		else if (ret)
			reply(cfd, "ERR stop failed: %s", strerror(-ret));
		else
			reply(cfd, "OK");
	} else if (strcasecmp(cmd, "STATS") == 0) {
		struct iqnet_stats st;

		if (stream_stats(s, &st, err, sizeof(err))) {
			reply(cfd, "ERR %s", err);
			return;
		}
		reply(cfd,
		      "STATS datagrams=%llu bytes=%llu blocks=%llu send_errors=%llu zc_copied=%llu overflows=%llu short_blocks=%llu copy_batches=%llu inflight=%u running=%u",
		      (unsigned long long)st.datagrams,
		      (unsigned long long)st.bytes,
		      (unsigned long long)st.blocks,
		      (unsigned long long)st.send_errors,
		      (unsigned long long)st.zc_copied,
		      (unsigned long long)st.overflows,
		      (unsigned long long)st.short_blocks,
		      (unsigned long long)st.copy_batches,
		      (unsigned int)st.blocks_inflight,
		      st.running ? 1u : 0u);
	} else {
		reply(cfd, "ERR unknown command '%.32s'", cmd);
	}
}

/* ---------------------------------------------------------------- main */

static int listen_socket(unsigned int port)
{
	struct sockaddr_in sa;
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		syslog(LOG_ERR, "socket: %s", strerror(errno));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		syslog(LOG_ERR, "bind port %u: %s", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 4) < 0) {
		syslog(LOG_ERR, "listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void tune_client(int fd)
{
	int one = 1, idle = 5, intvl = 2, cnt = 3;
	unsigned int user_timeout = 11000;	/* ms */
	struct timeval sndtimeo = { .tv_sec = 2 };

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	/*
	 * A crashed/unplugged host must stop the stream within ~11 s:
	 * keepalive covers an idle connection, TCP_USER_TIMEOUT covers one
	 * with unacknowledged data (keepalive is suspended then).
	 */
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout,
		   sizeof(user_timeout));
	/* a client that stops reading replies must not block the daemon */
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));
}

/* stop the client's stream and drop the connection */
static void client_close(struct client *c, struct stream *s, const char *why)
{
	syslog(LOG_INFO, "client %s disconnected%s%s", inet_ntoa(c->peer),
	       why ? ": " : "", why ? why : "");
	stream_stop(s);
	close(c->fd);
	c->fd = -1;
}

/* read and process whatever the client sent; closes it on EOF/error */
static void client_read(struct client *c, struct stream *s)
{
	char buf[512];
	ssize_t n;

	n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (n < 0 && (errno == EINTR || errno == EAGAIN ||
		      errno == EWOULDBLOCK))
		return;
	if (n <= 0) {
		client_close(c, s, n < 0 ? strerror(errno) : NULL);
		return;
	}
	for (ssize_t i = 0; i < n && c->fd >= 0; i++) {
		char ch = buf[i];

		if (ch == '\n') {
			if (c->line_overflow) {
				reply(c->fd, "ERR line too long");
			} else {
				if (c->line_len &&
				    c->line[c->line_len - 1] == '\r')
					c->line_len--;
				c->line[c->line_len] = '\0';
				handle_line(c->fd, c->line, s, c->peer);
			}
			c->line_len = 0;
			c->line_overflow = 0;
		} else if (c->line_len < sizeof(c->line) - 1) {
			c->line[c->line_len++] = ch;
		} else {
			c->line_overflow = 1;
		}
	}
}

/*
 * Is the current client gone? Non-destructive probe: 0 = orderly EOF,
 * hard error = reset/unreachable. Pending data or EAGAIN = still alive.
 * Returns a static description of why it is dead, or NULL if alive.
 */
static const char *client_dead(int fd)
{
	char ch;
	ssize_t n;

	do {
		n = recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
	} while (n < 0 && errno == EINTR);
	if (n == 0)
		return "EOF";
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return strerror(errno);
	return NULL;
}

static void client_accept(int lfd, struct client *c, struct stream *s)
{
	struct sockaddr_in ca;
	socklen_t calen = sizeof(ca);
	int nfd;

	nfd = accept4(lfd, (struct sockaddr *)&ca, &calen, SOCK_CLOEXEC);
	if (nfd < 0) {
		if (errno != EINTR && errno != EAGAIN &&
		    errno != EWOULDBLOCK && errno != ECONNABORTED)
			syslog(LOG_ERR, "accept: %s", strerror(errno));
		return;
	}
	if (calen < sizeof(ca) || ca.sin_family != AF_INET) {
		reply(nfd, "ERR IPv4 peer required");
		close(nfd);
		return;
	}
	if (c->fd >= 0) {
		const char *why = client_dead(c->fd);
		char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];

		inet_ntop(AF_INET, &ca.sin_addr, a, sizeof(a));
		inet_ntop(AF_INET, &c->peer, b, sizeof(b));
		if (!why) {
			syslog(LOG_NOTICE,
			       "rejecting %s: client %s already connected",
			       a, b);
			reply(nfd, "ERR busy");
			close(nfd);
			return;
		}
		syslog(LOG_NOTICE, "client %s is gone (%s), taking %s", b,
		       why, a);
		client_close(c, s, why);
	}
	c->fd = nfd;
	c->peer = ca.sin_addr;
	c->line_len = 0;
	c->line_overflow = 0;
	tune_client(c->fd);
	syslog(LOG_INFO, "client %s connected", inet_ntoa(c->peer));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-f] [-p ctrl_port] [-F iqnet_flags]\n"
		"  -f  run in foreground, log to stderr too\n"
		"  -p  TCP control port (default %u)\n"
		"  -F  debug IQNET_F_* flags (1=no zerocopy, 2=sync cache)\n",
		prog, IQNET_CTRL_PORT);
}

int main(int argc, char **argv)
{
	unsigned int port = IQNET_CTRL_PORT;
	struct client c = { .fd = -1 };
	struct sigaction sa;
	sigset_t quitmask, waitmask;
	struct stream s;
	int lfd, opt;

	while ((opt = getopt(argc, argv, "fp:F:h")) != -1) {
		switch (opt) {
		case 'f':
			g_foreground = 1;
			break;
		case 'p':
			if (parse_uint(optarg, 1, 65535, &port)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'F': {
			char *end;
			unsigned long v;

			errno = 0;
			v = strtoul(optarg, &end, 0);
			if (errno || *end || v > UINT32_MAX) {
				usage(argv[0]);
				return 1;
			}
			g_iqnet_flags = (unsigned int)v;
			break;
		}
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	openlog("iqnetd", LOG_PID | (g_foreground ? LOG_PERROR : 0),
		LOG_DAEMON);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;	/* no SA_RESTART: interrupt ppoll() */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	lfd = listen_socket(port);
	if (lfd < 0)
		return 1;

	if (!g_foreground && daemon(0, 0) < 0) {
		syslog(LOG_ERR, "daemon: %s", strerror(errno));
		return 1;
	}

	stream_init(&s);
	syslog(LOG_INFO, "listening on TCP port %u", port);

	/*
	 * SIGTERM/SIGINT stay blocked except inside ppoll(), so a signal can
	 * not slip in between the g_quit check and the wait.
	 */
	sigemptyset(&quitmask);
	sigaddset(&quitmask, SIGTERM);
	sigaddset(&quitmask, SIGINT);
	sigprocmask(SIG_BLOCK, &quitmask, &waitmask);
	sigdelset(&waitmask, SIGTERM);
	sigdelset(&waitmask, SIGINT);

	while (!g_quit) {
		struct pollfd pfd[2];
		nfds_t nfds = 1;

		pfd[0].fd = lfd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		if (c.fd >= 0) {
			pfd[1].fd = c.fd;
			pfd[1].events = POLLIN;
			pfd[1].revents = 0;
			nfds = 2;
		}
		if (ppoll(pfd, nfds, NULL, &waitmask) < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "poll: %s", strerror(errno));
			break;
		}

		/*
		 * Existing client first: its EOF (e.g. a host that reconnects
		 * right after closing) must be seen before the new connection
		 * is judged, or the reconnect would get "ERR busy".
		 */
		if (nfds == 2 && c.fd >= 0 &&
		    (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))
			client_read(&c, &s);

		if (pfd[0].revents & POLLIN)
			client_accept(lfd, &c, &s);
	}

	syslog(LOG_INFO, "exiting");
	stream_stop(&s);
	if (c.fd >= 0)
		close(c.fd);
	close(lfd);
	closelog();
	return 0;
}
