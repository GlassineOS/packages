// SPDX-License-Identifier: GPL-2.0-or-later
/* qrtr-diag-sink: a stand-in for the application processor's side of Qualcomm
 * DIAG over QRTR.
 *
 * Some modems' diag task waits for the application processor's diag servers:
 * service 0x1001, instance = peripheral base (modem: 0) + CNTL 0, DATA 2, DCI 4.
 * The modem serves CMD 1 and DCI_CMD 3 itself. A vendor kernel's diag driver
 * provides those servers; mainline Linux has no diag driver. Without them, on
 * an SDM636 with a WCN3990 run by the modem, the modem's watchdog kills it
 * some 10 to 20 seconds after the WLAN firmware starts ("Task starvation:
 * diag"), and every recovery crashes again. Publishing the servers and
 * discarding what arrives is enough to keep it up.
 *
 * The modem only connects to servers that appear while it is running (servers
 * published before it boots are ignored). So the default mode publishes the
 * servers on a fresh socket every REPUBLISH_S seconds until the modem's own CMD
 * server (0x1001:1, published once it has connected) appears, and again
 * whenever that server goes away (a modem restart or crash recovery).
 *
 *   qrtr-diag-sink                     publish 0x1001:0,2,4 and discard what arrives
 *   qrtr-diag-sink lookup              list the QRTR services (kernel name service)
 *   qrtr-diag-sink serve SVC:INST...   publish any servers and print what arrives
 */
#include <errno.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DIAG_SERVICE	0x1001
#define DIAG_CMD	1	/* the modem's own server: it has connected */
#define REPUBLISH_S	10

static const unsigned int diag_instances[] = { 0, 2, 4 };	/* CNTL, DATA, DCI */

/* What the default mode does next. */
struct sink {
	int up;		/* the modem's CMD server is there */
	time_t since;	/* when the servers were last published (0: never) */
};

/* Publish again: never published, or the modem hasn't connected for REPUBLISH_S. */
static int sink_due(const struct sink *s, time_t now)
{
	return !s->since || (!s->up && now - s->since >= REPUBLISH_S);
}

static void sink_published(struct sink *s, time_t now)
{
	s->since = now;
}

/* A name service notice. Only the modem's CMD server counts, never one of this
 * node's own servers. Returns 1 when it changes the state: the modem connected,
 * or it went away (then the servers are published again at once). */
static int sink_notice(struct sink *s, const struct qrtr_ctrl_pkt *p, unsigned int my_node)
{
	if (p->server.service != DIAG_SERVICE || p->server.instance != DIAG_CMD ||
	    p->server.node == my_node)
		return 0;
	if (p->cmd == QRTR_TYPE_NEW_SERVER && !s->up) {
		s->up = 1;
		return 1;
	}
	if (p->cmd == QRTR_TYPE_DEL_SERVER && s->up) {
		s->up = 0;
		s->since = 0;
		return 1;
	}
	return 0;
}

/* "SVC:INST", each a number in C notation. Returns 0 on success. */
static int parse_server(const char *arg, unsigned int *svc, unsigned int *inst)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(arg, &end, 0);
	if (errno || end == arg || *end != ':' || v > 0xffffffffUL)
		return -1;
	*svc = v;
	arg = end + 1;
	v = strtoul(arg, &end, 0);
	if (errno || end == arg || *end || v > 0xffffffffUL)
		return -1;
	*inst = v;
	return 0;
}

#ifndef SINK_TEST
static int ctrl_send(int fd, const struct qrtr_ctrl_pkt *p)
{
	struct sockaddr_qrtr sq;
	socklen_t l = sizeof(sq);

	if (getsockname(fd, (void *)&sq, &l))
		return -1;
	sq.sq_port = QRTR_PORT_CTRL;
	return sendto(fd, p, sizeof(*p), 0, (void *)&sq, sizeof(sq));
}

static int qrtr_socket(void)
{
	int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);

	if (fd < 0)
		perror("qrtr-diag-sink: socket (is the qrtr module loaded?)");
	return fd;
}

static int publish(int fd, unsigned int svc, unsigned int inst)
{
	struct qrtr_ctrl_pkt p;

	memset(&p, 0, sizeof(p));
	p.cmd = QRTR_TYPE_NEW_SERVER;
	p.server.service = svc;
	p.server.instance = inst;
	return ctrl_send(fd, &p);
}

static int lookup(void)
{
	struct qrtr_ctrl_pkt p, r;
	struct pollfd pf;
	int fd = qrtr_socket();

	if (fd < 0)
		return 1;
	memset(&p, 0, sizeof(p));
	p.cmd = QRTR_TYPE_NEW_LOOKUP;
	if (ctrl_send(fd, &p) < 0) {
		perror("qrtr-diag-sink: lookup");
		return 1;
	}
	pf.fd = fd;
	pf.events = POLLIN;
	while (poll(&pf, 1, 1000) > 0) {
		if (recv(fd, &r, sizeof(r), 0) < (ssize_t)sizeof(r) || r.cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		if (!r.server.service && !r.server.node)
			break;	/* the end of the list */
		printf("svc %5u (0x%04x) inst 0x%08x (ver %u ins %u) node %u port %u\n",
		       r.server.service, r.server.service, r.server.instance,
		       r.server.instance & 0xff, r.server.instance >> 8, r.server.node, r.server.port);
	}
	return 0;
}

static int serve(int argc, char **argv)
{
	static unsigned char b[65536];
	unsigned long n = 0, bytes = 0;
	time_t last = 0;
	unsigned int svc, inst;
	int fd, i;

	for (i = 0; i < argc; i++) {
		if (parse_server(argv[i], &svc, &inst)) {
			fprintf(stderr, "qrtr-diag-sink: not SVC:INST: %s\n", argv[i]);
			return 2;
		}
	}
	fd = qrtr_socket();
	if (fd < 0)
		return 1;
	for (i = 0; i < argc; i++) {
		parse_server(argv[i], &svc, &inst);
		if (publish(fd, svc, inst) < 0)
			perror("qrtr-diag-sink: publish");
		else
			printf("published %u:0x%x\n", svc, inst);
	}
	fflush(stdout);
	for (;;) {
		struct sockaddr_qrtr from;
		socklen_t fl = sizeof(from);
		ssize_t r = recvfrom(fd, b, sizeof(b), 0, (void *)&from, &fl);

		if (r < 0) {
			perror("qrtr-diag-sink: recv");
			return 1;
		}
		n++;
		bytes += r;
		/* the first 20 packets, then one line a second */
		if (n <= 20 || time(NULL) != last) {
			last = time(NULL);
			printf("rx %zd B from %u:%u (total %lu pkts %lu B):", r, from.sq_node,
			       from.sq_port, n, bytes);
			for (i = 0; i < r && i < 24; i++)
				printf(" %02x", b[i]);
			printf("\n");
			fflush(stdout);
		}
	}
}

static int sink(void)
{
	static unsigned char b[65536];
	struct sink s = { 0, 0 };
	struct sockaddr_qrtr me;
	socklen_t ml = sizeof(me);
	struct qrtr_ctrl_pkt p;
	int fd, srv = -1;
	size_t i;

	fd = qrtr_socket();
	if (fd < 0)
		return 1;
	/* a lookup subscription: NEW_SERVER and DEL_SERVER for the diag service keep coming */
	memset(&p, 0, sizeof(p));
	p.cmd = QRTR_TYPE_NEW_LOOKUP;
	p.server.service = DIAG_SERVICE;
	if (ctrl_send(fd, &p) < 0) {
		perror("qrtr-diag-sink: lookup");
		return 1;
	}
	if (getsockname(fd, (void *)&me, &ml)) {
		perror("qrtr-diag-sink: getsockname");
		return 1;
	}
	for (;;) {
		struct pollfd pf[2];

		if (srv < 0 || sink_due(&s, time(NULL))) {
			/* closing the old socket withdraws its servers */
			if (srv >= 0)
				close(srv);
			srv = qrtr_socket();
			for (i = 0; srv >= 0 && i < sizeof(diag_instances) / sizeof(*diag_instances); i++)
				if (publish(srv, DIAG_SERVICE, diag_instances[i]) < 0)
					perror("qrtr-diag-sink: publish");
			sink_published(&s, time(NULL));
			printf("published the diag servers, waiting for the modem\n");
			fflush(stdout);
		}
		pf[0].fd = fd;
		pf[0].events = POLLIN;
		pf[1].fd = srv;
		pf[1].events = POLLIN;
		if (poll(pf, 2, 1000) < 0) {
			if (errno == EINTR)
				continue;
			perror("qrtr-diag-sink: poll");
			return 1;
		}
		if (pf[1].revents & POLLIN)
			recv(srv, b, sizeof(b), 0);	/* discarded */
		if (pf[0].revents & POLLIN) {
			struct qrtr_ctrl_pkt r;

			if (recv(fd, &r, sizeof(r), 0) < (ssize_t)sizeof(r))
				continue;
			if (sink_notice(&s, &r, me.sq_node)) {
				printf(s.up ? "the modem's diag connected\n"
					    : "the modem's diag went away, publishing again\n");
				fflush(stdout);
			}
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return sink();
	if (argc == 2 && !strcmp(argv[1], "lookup"))
		return lookup();
	if (argc > 2 && !strcmp(argv[1], "serve"))
		return serve(argc - 2, argv + 2);
	fprintf(stderr, "usage: qrtr-diag-sink [lookup | serve SVC:INST...]\n");
	return 2;
}
#endif
