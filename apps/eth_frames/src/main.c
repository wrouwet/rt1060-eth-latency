/*
 * Sends hand-built Ethernet frames from the Zephyr shell: raw frames with any
 * EtherType, or IPv4/UDP, each optionally with an 802.1Q VLAN tag.
 *
 * Frames go out through a packet socket exactly as built here; there is no IP
 * stack, so the board sends nothing else. Run "eth" on the console for usage.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmsis_core.h>
#include <soc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(eth_frames, LOG_LEVEL_INF);

#define ETH_HDR_LEN     14
#define VLAN_TAG_LEN    4
#define IPV4_HDR_LEN    20
#define UDP_HDR_LEN     8
#define ETH_MIN_FRAME   60   /* without the FCS, which the MAC appends */
#define ETH_MAX_PAYLOAD 1500 /* the MTU: everything after the EtherType */
#define FRAME_BUF_LEN   (ETH_HDR_LEN + VLAN_TAG_LEN + ETH_MAX_PAYLOAD)

/* IEEE 802 "local experimental" EtherType, safe to use on a private link. */
#define DEFAULT_ETHERTYPE 0x88B5
#define DEFAULT_SRC_IP    "192.168.200.2"
#define DEFAULT_UDP_PORT  5000
#define SEQ_LEN           4 /* each payload starts with a big-endian sequence number */

#define DEFAULT_PING_INTERVAL_MS 100
#define DEFAULT_PING_TIMEOUT_MS  100

/* Consecutive send failures, 1 ms apart, before the sender gives up. */
#define MAX_FAILURES      1000

#define SENDER_STACK_SIZE 2048
#define SENDER_PRIORITY   7

struct tx_job {
	uint8_t frame[FRAME_BUF_LEN];
	size_t len;
	size_t payload_off; /* where the sequence number goes */
	size_t udp_off;     /* UDP header, or 0 for a raw frame */
	size_t type_off;    /* the EtherType, after any VLAN tag */
	uint32_t count;     /* 0 = until "eth stop" */
	uint32_t interval_ms;
	bool ping;          /* wait for each frame to come back and time it */
	uint32_t timeout_ms;
};

static struct tx_job job;
static atomic_t busy;
static atomic_t stop_requested;
static K_SEM_DEFINE(job_ready, 0, 1);
static const struct shell *job_shell;

static struct net_mgmt_event_callback if_cb;

/* Hardware (IEEE 1588) transmit timestamps arrive through this callback for
 * frames sent on the ping socket, which asks for them.
 */
static struct net_if_timestamp_cb tx_ts_cb;
static struct net_ptp_time tx_ts;
static K_SEM_DEFINE(tx_ts_ready, 0, 1);

static void tx_ts_handler(struct net_pkt *pkt)
{
	tx_ts = *net_pkt_timestamp(pkt);
	k_sem_give(&tx_ts_ready);
}

/* Transmit marker on Arduino header D6: high from the instant the transmit
 * DMA is started until the MAC reports the frame transmitted. The hooks are
 * called by the NXP ENET library and driver (see patches/), run from ITCM
 * (CMakeLists.txt) and write the GPIO set/clear registers directly, to keep
 * the edges close to the frame on the wire.
 */
#define MARKER_NODE DT_PATH(zephyr_user)
#define MARKER_GPIO ((GPIO_Type *)DT_REG_ADDR(DT_GPIO_CTLR(MARKER_NODE, tx_gpios)))
#define MARKER_BIT  BIT(DT_GPIO_PIN(MARKER_NODE, tx_gpios))

static const struct gpio_dt_spec tx_marker = GPIO_DT_SPEC_GET(MARKER_NODE, tx_gpios);
static volatile uint32_t marker_rise_cycles;
static volatile uint32_t marker_width_cycles;
static volatile uint32_t marker_frame_len;

void ENET_TxActivateHook(ENET_Type *base)
{
	ARG_UNUSED(base);
	MARKER_GPIO->DR_SET = MARKER_BIT;
	marker_rise_cycles = DWT->CYCCNT;
}

void eth_nxp_enet_tx_done_hook(void)
{
	MARKER_GPIO->DR_CLEAR = MARKER_BIT;
	marker_width_cycles = DWT->CYCCNT - marker_rise_cycles;
}

/* --- frame building ---------------------------------------------------- */

static int parse_mac(const char *s, uint8_t mac[6])
{
	if (strcmp(s, "bcast") == 0) {
		memset(mac, 0xff, 6);
		return 0;
	}
	for (int i = 0; i < 6; i++) {
		char *end;
		unsigned long b = strtoul(s, &end, 16);

		if (end == s || b > 0xff || (i < 5 && *end != ':') || (i == 5 && *end != '\0')) {
			return -EINVAL;
		}
		mac[i] = (uint8_t)b;
		s = end + 1;
	}
	return 0;
}

static int parse_ipv4(const char *s, uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		char *end;
		unsigned long b = strtoul(s, &end, 10);

		if (end == s || b > 255 || (i < 3 && *end != '.') || (i == 3 && *end != '\0')) {
			return -EINVAL;
		}
		ip[i] = (uint8_t)b;
		s = end + 1;
	}
	return 0;
}

static int parse_u32(const char *s, uint32_t max, uint32_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (end == s || *end != '\0' || v > max) {
		return -EINVAL;
	}
	*out = v;
	return 0;
}

/* Ones' complement sum over data, folded into 16 bits but not inverted. */
static uint32_t csum_add(uint32_t sum, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i + 1 < len; i += 2) {
		sum += (uint32_t)(data[i] << 8 | data[i + 1]);
	}
	if (len & 1) {
		sum += (uint32_t)data[len - 1] << 8;
	}
	return sum;
}

static uint16_t csum_finish(uint32_t sum)
{
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

/* Recomputes the UDP checksum, which covers the sequence number. */
static void update_udp_checksum(struct tx_job *j)
{
	uint8_t *ip = &j->frame[j->udp_off - IPV4_HDR_LEN];
	uint8_t *udp = &j->frame[j->udp_off];
	uint16_t udp_len = sys_get_be16(&udp[4]);
	uint8_t pseudo[4] = {0, 17, udp_len >> 8, udp_len & 0xff};
	uint32_t sum;
	uint16_t csum;

	sys_put_be16(0, &udp[6]);
	sum = csum_add(0, &ip[12], 8); /* source and destination addresses */
	sum = csum_add(sum, pseudo, sizeof(pseudo));
	sum = csum_add(sum, udp, udp_len);
	csum = csum_finish(sum);
	sys_put_be16(csum == 0 ? 0xffff : csum, &udp[6]);
}

struct frame_opts {
	uint8_t dst_mac[6];
	bool vlan;
	uint16_t vid;
	uint8_t pcp;
	uint16_t ethertype;
	uint32_t payload_len; /* bytes after the UDP header, or after the EtherType */
	bool udp;
	uint8_t src_ip[4];
	uint8_t dst_ip[4];
	uint16_t dst_port;
};

/* Builds the frame into job; returns 0 or a negative errno with a message printed. */
static int build_frame(const struct shell *sh, const struct frame_opts *o, struct net_if *iface)
{
	uint8_t *f = job.frame;
	size_t pos = 0;

	if (o->payload_len < SEQ_LEN ||
	    o->payload_len + (o->udp ? IPV4_HDR_LEN + UDP_HDR_LEN : 0) > ETH_MAX_PAYLOAD) {
		shell_error(sh, "payload must be %d to %d bytes", SEQ_LEN,
			    ETH_MAX_PAYLOAD - (o->udp ? IPV4_HDR_LEN + UDP_HDR_LEN : 0));
		return -EINVAL;
	}

	memcpy(&f[pos], o->dst_mac, 6);
	memcpy(&f[pos + 6], net_if_get_link_addr(iface)->addr, 6);
	pos += 12;
	if (o->vlan) {
		sys_put_be16(NET_ETH_PTYPE_VLAN, &f[pos]);
		sys_put_be16((uint16_t)(o->pcp << 13 | o->vid), &f[pos + 2]);
		pos += VLAN_TAG_LEN;
	}
	job.type_off = pos;
	sys_put_be16(o->udp ? NET_ETH_PTYPE_IP : o->ethertype, &f[pos]);
	pos += 2;

	job.udp_off = 0;
	if (o->udp) {
		uint8_t *ip = &f[pos];
		uint16_t total = IPV4_HDR_LEN + UDP_HDR_LEN + o->payload_len;

		memset(ip, 0, IPV4_HDR_LEN);
		ip[0] = 0x45; /* version 4, 5-word header */
		sys_put_be16(total, &ip[2]);
		sys_put_be16(0x4000, &ip[6]); /* don't fragment */
		ip[8] = 64;                   /* TTL */
		ip[9] = 17;                   /* UDP */
		memcpy(&ip[12], o->src_ip, 4);
		memcpy(&ip[16], o->dst_ip, 4);
		sys_put_be16(csum_finish(csum_add(0, ip, IPV4_HDR_LEN)), &ip[10]);
		pos += IPV4_HDR_LEN;

		job.udp_off = pos;
		sys_put_be16(DEFAULT_UDP_PORT, &f[pos]);
		sys_put_be16(o->dst_port, &f[pos + 2]);
		sys_put_be16(UDP_HDR_LEN + o->payload_len, &f[pos + 4]);
		pos += UDP_HDR_LEN;
	}

	job.payload_off = pos;
	for (uint32_t i = 0; i < o->payload_len; i++) {
		f[pos + i] = (uint8_t)i;
	}
	pos += o->payload_len;

	/* Pad short frames to the Ethernet minimum; IP and UDP lengths exclude it. */
	if (pos < ETH_MIN_FRAME) {
		memset(&f[pos], 0, ETH_MIN_FRAME - pos);
		pos = ETH_MIN_FRAME;
	}
	job.len = pos;
	return 0;
}

/* --- sending ----------------------------------------------------------- */

static int64_t ptp_ns(const struct net_ptp_time *t)
{
	return (int64_t)t->second * NSEC_PER_SEC + t->nanosecond;
}

static void print_us(const char *prefix, int64_t ns)
{
	shell_print(job_shell, "%s%lld.%03lld us", prefix, ns / 1000, ns % 1000);
}

/* Reads frames until the reflected copy of sequence number seq arrives or the
 * timeout passes. Returns its hardware receive timestamp in ns, or -1.
 */
static int64_t wait_for_reply(int sock, uint32_t seq, uint8_t *peer, bool *peer_known)
{
	static uint8_t rx[FRAME_BUF_LEN];
	uint8_t ctrl[NET_CMSG_SPACE(sizeof(struct net_ptp_time))];
	const uint8_t *own = job.frame + 6;
	uint16_t type = sys_get_be16(&job.frame[job.type_off]);
	int64_t deadline = k_uptime_get() + job.timeout_ms;
	int64_t left;

	while ((left = deadline - k_uptime_get()) > 0) {
		struct zsock_pollfd pfd = {.fd = sock, .events = ZSOCK_POLLIN};
		struct net_iovec iov = {.iov_base = rx, .iov_len = sizeof(rx)};
		struct net_msghdr msg = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = ctrl,
			.msg_controllen = sizeof(ctrl),
		};

		if (zsock_poll(&pfd, 1, (int)left) <= 0) {
			break;
		}
		ssize_t len = zsock_recvmsg(sock, &msg, 0);
		size_t off = 12;

		if (len < ETH_HDR_LEN + SEQ_LEN) {
			continue;
		}
		/* Ignore our own frames; accept a reply with or without a VLAN tag. */
		if (memcmp(&rx[0], own, 6) != 0 || memcmp(&rx[6], own, 6) == 0) {
			continue;
		}
		if (sys_get_be16(&rx[off]) == NET_ETH_PTYPE_VLAN) {
			off += VLAN_TAG_LEN;
		}
		if (sys_get_be16(&rx[off]) != type || (size_t)len < off + 2 + SEQ_LEN ||
		    sys_get_be32(&rx[off + 2]) != seq) {
			continue;
		}
		if (!*peer_known) {
			memcpy(peer, &rx[6], 6);
			*peer_known = true;
			shell_print(job_shell, "replies come from %02x:%02x:%02x:%02x:%02x:%02x",
				    peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
		}
		for (struct net_cmsghdr *c = NET_CMSG_FIRSTHDR(&msg); c != NULL;
		     c = NET_CMSG_NXTHDR(&msg, c)) {
			if (c->cmsg_level == ZSOCK_SOL_SOCKET && c->cmsg_type == ZSOCK_SO_TIMESTAMPING) {
				struct net_ptp_time ts;

				memcpy(&ts, NET_CMSG_DATA(c), sizeof(ts));
				return ptp_ns(&ts);
			}
		}
		shell_error(job_shell, "seq %u: reply had no hardware timestamp", seq);
		return -1;
	}
	return -1;
}

/* Sends job.count frames one at a time, each after the previous came back or
 * timed out, and prints the round trip between the hardware timestamps of the
 * frame leaving and its reflection arriving.
 */
static void run_ping(int sock, struct net_if *iface, struct net_sockaddr_ll *ll)
{
	uint8_t flags = ZSOCK_SOF_TIMESTAMPING_RX_HARDWARE | ZSOCK_SOF_TIMESTAMPING_TX_HARDWARE;
	struct net_sockaddr_ll bind_addr = {
		.sll_family = NET_AF_PACKET,
		.sll_protocol = net_htons(ETH_P_ALL),
		.sll_ifindex = ll->sll_ifindex,
	};
	uint8_t peer[6];
	bool peer_known = false;
	uint32_t sent = 0, received = 0;
	int64_t min = INT64_MAX, max = 0, sum = 0;

	if (zsock_bind(sock, (struct net_sockaddr *)&bind_addr, sizeof(bind_addr)) < 0 ||
	    zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_TIMESTAMPING, &flags,
			     sizeof(flags)) < 0) {
		shell_error(job_shell, "ping socket setup failed: %d", errno);
		return;
	}

	while (!atomic_get(&stop_requested) && (job.count == 0 || sent < job.count)) {
		uint32_t seq = sent;
		int64_t t_tx, t_rx;

		sys_put_be32(seq, &job.frame[job.payload_off]);
		marker_frame_len = job.len;
		k_sem_reset(&tx_ts_ready);
		if (zsock_sendto(sock, job.frame, job.len, 0, (struct net_sockaddr *)ll,
				 sizeof(*ll)) < 0) {
			shell_error(job_shell, "send failed (%d)%s, stopping", errno,
				    net_if_is_carrier_ok(iface) ? "" : ": no link");
			break;
		}
		sent++;
		if (k_sem_take(&tx_ts_ready, K_MSEC(job.timeout_ms)) != 0) {
			shell_error(job_shell, "seq %u: no transmit timestamp", seq);
			continue;
		}
		t_tx = ptp_ns(&tx_ts);

		t_rx = wait_for_reply(sock, seq, peer, &peer_known);
		if (t_rx < 0) {
			shell_print(job_shell, "seq %u: no reply within %u ms", seq, job.timeout_ms);
		} else {
			int64_t rtt = t_rx - t_tx;
			char prefix[32];

			received++;
			sum += rtt;
			min = MIN(min, rtt);
			max = MAX(max, rtt);
			snprintk(prefix, sizeof(prefix), "seq %u: round trip ", seq);
			print_us(prefix, rtt);
		}
		if (job.interval_ms != 0 && !atomic_get(&stop_requested)) {
			k_sleep(K_MSEC(job.interval_ms));
		}
	}

	shell_print(job_shell, "%u sent, %u returned (%u lost)", sent, received, sent - received);
	if (received != 0) {
		print_us("round trip min ", min);
		print_us("           avg ", sum / received);
		print_us("           max ", max);
	}
}

static void sender_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		k_sem_take(&job_ready, K_FOREVER);

		struct net_if *iface = net_if_get_default();
		struct net_sockaddr_ll ll = {
			.sll_family = NET_AF_PACKET,
			.sll_ifindex = net_if_get_by_iface(iface),
			.sll_halen = 6,
		};
		uint32_t sent = 0, errors = 0, failures = 0;
		int64_t start = k_uptime_get();
		int sock = zsock_socket(NET_AF_PACKET, NET_SOCK_RAW, net_htons(ETH_P_ALL));

		memcpy(ll.sll_addr, job.frame, 6);
		if (sock < 0) {
			shell_error(job_shell, "packet socket failed: %d", errno);
			atomic_clear(&busy);
			continue;
		}
		if (job.ping) {
			run_ping(sock, iface, &ll);
			zsock_close(sock);
			atomic_clear(&busy);
			continue;
		}

		while (!atomic_get(&stop_requested) && (job.count == 0 || sent < job.count)) {
			sys_put_be32(sent, &job.frame[job.payload_off]);
			if (job.udp_off != 0) {
				update_udp_checksum(&job);
			}
			marker_frame_len = job.len;
			if (zsock_sendto(sock, job.frame, job.len, 0, (struct net_sockaddr *)&ll,
					 sizeof(ll)) < 0) {
				/* Out of buffers at full speed: back off and retry, unless
				 * the error is permanent, the link is gone or nothing gets
				 * through.
				 */
				int err = errno;
				bool transient = err == ENOMEM || err == ENOBUFS || err == EAGAIN;

				errors++;
				if (!transient || !net_if_is_carrier_ok(iface) ||
				    ++failures > MAX_FAILURES) {
					shell_error(job_shell, "send failed (%d)%s, stopping", err,
						    net_if_is_carrier_ok(iface) ? "" : ": no link");
					break;
				}
				k_sleep(K_MSEC(1));
				continue;
			}
			failures = 0;
			sent++;
			if (job.interval_ms != 0) {
				k_sleep(K_MSEC(job.interval_ms));
			} else if ((sent & 0x3f) == 0) {
				k_yield();
			}
		}

		int64_t ms = k_uptime_get() - start;

		zsock_close(sock);
		shell_print(job_shell, "sent %u frames of %u bytes in %lld ms (%u retries)", sent,
			    (unsigned int)job.len, ms, errors);
		atomic_clear(&busy);
	}
}

K_THREAD_DEFINE(sender, SENDER_STACK_SIZE, sender_thread, NULL, NULL, NULL, SENDER_PRIORITY, 0,
		0);

/* --- shell ------------------------------------------------------------- */

#define USAGE_OPTS                                                                         \
	"  -c <count>     frames to send (default 1, 0 = until \"eth stop\")\n"              \
	"  -i <ms>        gap between frames (default 0 = as fast as possible)\n"           \
	"  -l <bytes>     payload length (default 64)\n"                                     \
	"  -v <vid>[:pcp] add an 802.1Q VLAN tag\n"                                          \
	"  -x             hex dump the first frame\n"

enum send_mode {
	MODE_RAW,
	MODE_UDP,
	MODE_PING,
};

/* Parses "<dst-mac> [<dst-ip>] [options]" and starts the sender. */
static int cmd_send(const struct shell *sh, size_t argc, char **argv, enum send_mode mode)
{
	bool udp = mode == MODE_UDP;
	bool ping = mode == MODE_PING;
	struct net_if *iface = net_if_get_default();
	struct frame_opts o = {
		.ethertype = DEFAULT_ETHERTYPE,
		.payload_len = 64,
		.udp = udp,
		.dst_port = DEFAULT_UDP_PORT,
	};
	uint32_t count = 1, v;
	uint32_t interval = ping ? DEFAULT_PING_INTERVAL_MS : 0;
	uint32_t timeout = DEFAULT_PING_TIMEOUT_MS;
	bool dump = false;
	int arg = 1;

	if (argc < (udp ? 3 : 2) || parse_mac(argv[arg++], o.dst_mac) != 0) {
		shell_error(sh, "first argument must be a MAC address like 02:00:00:00:00:01 or bcast");
		return -EINVAL;
	}
	if (udp && parse_ipv4(argv[arg++], o.dst_ip) != 0) {
		shell_error(sh, "second argument must be the destination IPv4 address");
		return -EINVAL;
	}
	parse_ipv4(DEFAULT_SRC_IP, o.src_ip);

	for (; arg < argc; arg++) {
		const char *opt = argv[arg];
		const char *val = (arg + 1 < argc) ? argv[arg + 1] : NULL;
		int err = 0;

		if (strcmp(opt, "-x") == 0) {
			dump = true;
			continue;
		}
		if (val == NULL) {
			shell_error(sh, "%s needs a value", opt);
			return -EINVAL;
		}
		arg++;
		if (strcmp(opt, "-c") == 0) {
			err = parse_u32(val, UINT32_MAX, &count);
		} else if (strcmp(opt, "-i") == 0) {
			err = parse_u32(val, 3600000, &interval);
		} else if (strcmp(opt, "-l") == 0) {
			err = parse_u32(val, ETH_MAX_PAYLOAD, &o.payload_len);
		} else if (strcmp(opt, "-v") == 0) {
			char vid_str[8];
			const char *colon = strchr(val, ':');
			size_t n = colon ? (size_t)(colon - val) : strlen(val);
			uint32_t pcp = 0;

			if (n >= sizeof(vid_str)) {
				err = -EINVAL;
			} else {
				memcpy(vid_str, val, n);
				vid_str[n] = '\0';
				err = parse_u32(vid_str, 4094, &v);
				if (err == 0 && colon != NULL) {
					err = parse_u32(colon + 1, 7, &pcp);
				}
			}
			o.vlan = true;
			o.vid = v;
			o.pcp = pcp;
		} else if (!udp && strcmp(opt, "-t") == 0) {
			err = parse_u32(val, 0xffff, &v);
			if (err == 0 && v < 0x0600) {
				err = -EINVAL; /* below 0x600 the field is a length, not a type */
			}
			o.ethertype = v;
		} else if (udp && strcmp(opt, "-s") == 0) {
			err = parse_ipv4(val, o.src_ip);
		} else if (udp && strcmp(opt, "-p") == 0) {
			err = parse_u32(val, 0xffff, &v);
			o.dst_port = v;
		} else if (ping && strcmp(opt, "-w") == 0) {
			err = parse_u32(val, 60000, &timeout);
			if (err == 0 && timeout == 0) {
				err = -EINVAL;
			}
		} else {
			shell_error(sh, "unknown option %s", opt);
			return -EINVAL;
		}
		if (err != 0) {
			shell_error(sh, "bad value for %s: %s", opt, val);
			return -EINVAL;
		}
	}

	if (!net_if_is_carrier_ok(iface)) {
		shell_warn(sh, "no Ethernet link; frames will be dropped");
	}
	if (!atomic_cas(&busy, 0, 1)) {
		shell_error(sh, "already sending; use \"eth stop\" first");
		return -EBUSY;
	}
	if (build_frame(sh, &o, iface) != 0) {
		atomic_clear(&busy);
		return -EINVAL;
	}
	job.count = count;
	job.interval_ms = interval;
	job.ping = ping;
	job.timeout_ms = timeout;

	if (dump) {
		sys_put_be32(0, &job.frame[job.payload_off]);
		if (job.udp_off != 0) {
			update_udp_checksum(&job);
		}
		shell_hexdump(sh, job.frame, job.len);
	}
	atomic_clear(&stop_requested);
	job_shell = sh;
	k_sem_give(&job_ready);
	return 0;
}

static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_send(sh, argc, argv, MODE_RAW);
}

static int cmd_udp(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_send(sh, argc, argv, MODE_UDP);
}

static int cmd_ping(const struct shell *sh, size_t argc, char **argv)
{
	return cmd_send(sh, argc, argv, MODE_PING);
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!atomic_get(&busy)) {
		shell_print(sh, "not sending");
		return 0;
	}
	atomic_set(&stop_requested, 1);
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *mac = net_if_get_link_addr(iface);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "interface %d, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s%s",
		    net_if_get_by_iface(iface), mac->addr[0], mac->addr[1], mac->addr[2],
		    mac->addr[3], mac->addr[4], mac->addr[5],
		    net_if_is_carrier_ok(iface) ? "up" : "down",
		    atomic_get(&busy) ? ", sending" : "");

	if (marker_width_cycles != 0) {
		/* At 100 Mbit/s: preamble, frame and FCS at 80 ns a byte. */
		uint32_t wire_ns = (marker_frame_len + 8 + 4) * 80;
		uint32_t pulse_ns = (uint32_t)((uint64_t)marker_width_cycles * NSEC_PER_SEC /
					       SystemCoreClock);

		shell_print(sh, "last D6 pulse %u ns; that frame's %u bytes take %u ns on the wire",
			    pulse_ns, marker_frame_len, wire_ns);
	}
	return 0;
}

static int cmd_examples(const struct shell *sh, size_t argc, char **argv)
{
	/* Broadcast destinations, so every example works as typed. */
	static const char *const lines[] = {
		"Basic VLAN frame: one broadcast frame tagged VLAN 100, priority 0,",
		"with its bytes dumped so you can see the 81 00 00 64 tag:",
		"  eth raw bcast -v 100 -x",
		"",
		"VLAN 100 with priority (PCP) 5, e.g. to test QoS queues:",
		"  eth raw bcast -v 100:5",
		"",
		"Untagged frame with your own EtherType and a 100-byte payload:",
		"  eth raw bcast -t 0x88b6 -l 100",
		"",
		"Largest VLAN frame, 1518 bytes (1500-byte payload plus tag):",
		"  eth raw bcast -v 10 -l 1500",
		"",
		"IPv4/UDP inside VLAN 200, to port 6000 on the subnet broadcast:",
		"  eth udp bcast 192.168.200.255 -v 200 -p 6000 -x",
		"",
		"UDP to one host, from a chosen source address:",
		"  eth udp 02:00:00:00:00:01 192.168.200.1 -s 192.168.200.50",
		"",
		"Steady stream: 100 frames a second until stopped:",
		"  eth raw bcast -v 100 -c 0 -i 10",
		"  eth stop",
		"",
		"Burst at full speed: 1000 full-size frames, then a summary:",
		"  eth raw bcast -c 1000 -l 1500",
		"",
		"Round trip to a frame reflector: 10 frames, one at a time, timed",
		"by the MAC's hardware timestamps (the first reply shows its MAC):",
		"  eth ping bcast -c 10",
		"",
		"Watch from a Linux machine on the cable (MAC from \"eth info\"):",
		"  sudo tcpdump -i <iface> -n -e -XX ether src <board-mac>",
		"  sudo tcpdump -i <iface> -n -e vlan 100",
	};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
		shell_print(sh, "%s", lines[i]);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	eth_cmds,
	SHELL_CMD(info, NULL, "Show the MAC address and link state", cmd_info),
	SHELL_CMD(examples, NULL, "Show example commands, e.g. a VLAN frame", cmd_examples),
	SHELL_CMD_ARG(raw, NULL,
		      "Send raw frames: eth raw <dst-mac> [options]\n" USAGE_OPTS
		      "  -t <ethertype> EtherType (default 0x88b5)",
		      cmd_raw, 2, 12),
	SHELL_CMD_ARG(udp, NULL,
		      "Send IPv4/UDP frames: eth udp <dst-mac> <dst-ip> [options]\n" USAGE_OPTS
		      "  -s <src-ip>    source address (default " DEFAULT_SRC_IP ")\n"
		      "  -p <port>      destination port (default 5000; source is 5000)",
		      cmd_udp, 3, 14),
	SHELL_CMD_ARG(ping, NULL,
		      "Time round trips to a frame reflector: eth ping <dst-mac> [options]\n"
		      "Sends one frame at a time and waits for it to come back; the round\n"
		      "trip is measured between the MAC's hardware timestamps.\n"
		      "  -c <count>     frames to send (default 1, 0 = until \"eth stop\")\n"
		      "  -i <ms>        gap after each reply (default 100)\n"
		      "  -w <ms>        how long to wait for each reply (default 100)\n"
		      "  -l <bytes>     payload length (default 64)\n"
		      "  -v <vid>[:pcp] add an 802.1Q VLAN tag\n"
		      "  -t <ethertype> EtherType (default 0x88b5)\n"
		      "  -x             hex dump the first frame",
		      cmd_ping, 2, 14),
	SHELL_CMD(stop, NULL, "Stop sending", cmd_stop),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(eth, &eth_cmds, "Send hand-built Ethernet frames", NULL);

/* --- link events ------------------------------------------------------- */

static void if_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_IF_UP) {
		LOG_INF("Ethernet link up");
	} else if (event == NET_EVENT_IF_DOWN) {
		LOG_INF("Ethernet link down");
	}
}

int main(void)
{
	if (!gpio_is_ready_dt(&tx_marker) ||
	    gpio_pin_configure_dt(&tx_marker, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_ERR("transmit marker pin D6 unavailable");
	}
	/* Start sending once 64 bytes are in the transmit FIFO instead of after
	 * the whole frame (store and forward), so the marker's rising edge is
	 * close to the start of the frame on the wire. The memory DMA is far
	 * faster than the 100 Mbit/s line, so the FIFO cannot run dry.
	 */
	ENET->TFWR = ENET_TFWR_TFWR(1);

	/* The cycle counter times the marker pulse for "eth info". */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->LAR = 0xC5ACCE55;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	net_mgmt_init_event_callback(&if_cb, if_event_handler, NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&if_cb);
	net_if_register_timestamp_cb(&tx_ts_cb, NULL, net_if_get_default(), tx_ts_handler);
	LOG_INF("Ready. Type \"eth\" for usage, \"eth examples\" for examples.");
	return 0;
}
