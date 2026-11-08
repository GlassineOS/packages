// SPDX-License-Identifier: GPL-2.0-or-later
/* Host test of qrtr-diag-sink's decisions (when to publish, which notices
 * count) and its argument parsing, without a QRTR socket. */
#define SINK_TEST
#include "qrtr-diag-sink.c"

static int fails;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static struct qrtr_ctrl_pkt notice(int cmd, unsigned int svc, unsigned int inst, unsigned int node)
{
	struct qrtr_ctrl_pkt p;

	memset(&p, 0, sizeof(p));
	p.cmd = cmd;
	p.server.service = svc;
	p.server.instance = inst;
	p.server.node = node;
	return p;
}

int main(void)
{
	struct sink s = { 0, 0 };
	struct qrtr_ctrl_pkt p;
	unsigned int svc, inst;
	const unsigned int me = 1, modem = 0;

	CHECK(sink_due(&s, 1000), "never published: due");
	sink_published(&s, 1000);
	CHECK(!sink_due(&s, 1000 + REPUBLISH_S - 1), "published just now: not due");
	CHECK(sink_due(&s, 1000 + REPUBLISH_S), "no modem after REPUBLISH_S: due again");

	/* notices that don't count */
	p = notice(QRTR_TYPE_NEW_SERVER, DIAG_SERVICE, DIAG_CMD, me);
	CHECK(!sink_notice(&s, &p, me) && !s.up, "this node's own CMD server ignored");
	p = notice(QRTR_TYPE_NEW_SERVER, DIAG_SERVICE, 3, modem);
	CHECK(!sink_notice(&s, &p, me) && !s.up, "the modem's DCI_CMD server ignored");
	p = notice(QRTR_TYPE_NEW_SERVER, 0x42, DIAG_CMD, modem);
	CHECK(!sink_notice(&s, &p, me) && !s.up, "another service ignored");
	p = notice(QRTR_TYPE_DEL_SERVER, DIAG_SERVICE, DIAG_CMD, modem);
	CHECK(!sink_notice(&s, &p, me) && !s.up, "a removal while not connected changes nothing");

	/* the modem connects: no more publishing, however long */
	p = notice(QRTR_TYPE_NEW_SERVER, DIAG_SERVICE, DIAG_CMD, modem);
	CHECK(sink_notice(&s, &p, me) && s.up, "the modem's CMD server: connected");
	CHECK(!sink_notice(&s, &p, me), "the same notice again: no change");
	CHECK(!sink_due(&s, 1000 + 100 * REPUBLISH_S), "connected: never due");

	/* it goes away: publish at once, then wait REPUBLISH_S again */
	p = notice(QRTR_TYPE_DEL_SERVER, DIAG_SERVICE, DIAG_CMD, modem);
	CHECK(sink_notice(&s, &p, me) && !s.up, "the modem's CMD server removed: not connected");
	CHECK(sink_due(&s, 5000), "after a removal: due at once");
	sink_published(&s, 5000);
	CHECK(!sink_due(&s, 5001), "published again: not due");

	/* SVC:INST */
	CHECK(!parse_server("0x1001:2", &svc, &inst) && svc == 0x1001 && inst == 2, "0x1001:2");
	CHECK(!parse_server("15:0x10", &svc, &inst) && svc == 15 && inst == 16, "15:0x10");
	CHECK(parse_server("4097", &svc, &inst), "no instance refused");
	CHECK(parse_server("4097:", &svc, &inst), "empty instance refused");
	CHECK(parse_server(":2", &svc, &inst), "empty service refused");
	CHECK(parse_server("4097:2x", &svc, &inst), "trailing characters refused");
	CHECK(parse_server("x:2", &svc, &inst), "not a number refused");

	if (fails)
		return 1;
	printf("sink-test: all checks passed\n");
	return 0;
}
