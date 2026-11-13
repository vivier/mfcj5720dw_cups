/* Supply query shared by printing and ReportLevels command jobs. */
#include <cups/sidechannel.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>

#include "ink.h"

#define MAINTENANCE_OID ".1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.8.0"
#define QUERY_TIMEOUT_SECONDS 3.0

/* Match the decoder's order in every CUPS marker attribute. */
enum ink_color {
	INK_BLACK,
	INK_CYAN,
	INK_MAGENTA,
	INK_YELLOW,
};

/* Display and warning policy; these thresholds are not supplied by the printer. */
enum ink_reporting_policy {
	SNMP_RESPONSE_BYTES = 4096,
	CENTIPERCENT_PER_PERCENT = 100,
	FULL_INK_PERCENT = 100,
	LOW_INK_PERCENT = 5,
	LOW_INK_CENTIPERCENT = LOW_INK_PERCENT * CENTIPERCENT_PER_PERCENT,
	MIN_NONEMPTY_PERCENT = 1,
};

static int query_ink_levels(unsigned int centipercent[BROTHER_INK_COLORS])
{
	unsigned char response[SNMP_RESPONSE_BYTES];
	socklen_t type_bytes = sizeof(int);
	int socket_type, response_bytes = sizeof(response);
	cups_sc_status_t status;

	/* The CUPS side channel requires a stream socket, not a file or datagram. */
	if (getsockopt(CUPS_SC_FD, SOL_SOCKET, SO_TYPE, &socket_type, &type_bytes) ||
	    socket_type != SOCK_STREAM) {
		fputs("DEBUG: Ink levels need a CUPS side-channel connection.\n", stderr);
		return 1;
	}
	status = cupsSideChannelSNMPGet(MAINTENANCE_OID, (char *)response, &response_bytes,
					QUERY_TIMEOUT_SECONDS);
	if (status != CUPS_SC_STATUS_OK) {
		fprintf(stderr, "DEBUG: Brother ink query unavailable (CUPS status %d).\n", status);
		return 1;
	}
	if (response_bytes < 0 || response_bytes > (int)sizeof(response) ||
	    brother_decode_ink(response, (size_t)response_bytes, centipercent)) {
		fputs("DEBUG: Invalid Brother ink response; keeping previous levels.\n", stderr);
		return 1;
	}
	return 0;
}

static unsigned int display_percent(unsigned int centipercent)
{
	unsigned int percent =
	    (centipercent + CENTIPERCENT_PER_PERCENT / 2) / CENTIPERCENT_PER_PERCENT;

	/* Round half up, keeping a positive estimate distinguishable from an empty tank. */
	return centipercent && !percent ? MIN_NONEMPTY_PERCENT : percent;
}

static void publish_ink_levels(const unsigned int centipercent[BROTHER_INK_COLORS])
{
	unsigned int percentages[BROTHER_INK_COLORS];
	bool any_low = false, any_empty = false;
	unsigned int color;

	for (color = 0; color < BROTHER_INK_COLORS; color++) {
		percentages[color] = display_percent(centipercent[color]);
		/* Warning states use the raw measurement, before display rounding. */
		any_low |= centipercent[color] > 0 && centipercent[color] <= LOW_INK_CENTIPERCENT;
		any_empty |= centipercent[color] == 0;
	}
	/*
	 * One short record keeps names, colors and levels in the same order.
	 * GNOME 50 recognizes "ink", but not the valid CUPS "ink-cartridge" type.
	 */
	fprintf(stderr,
		"ATTR: marker-names=Black,Cyan,Magenta,Yellow "
		"marker-colors=#000000,#00FFFF,#FF00FF,#FFFF00 "
		"marker-types=ink,ink,ink,ink "
		"marker-high-levels=%d,%d,%d,%d marker-low-levels=%d,%d,%d,%d "
		"marker-levels=%u,%u,%u,%u\n",
		FULL_INK_PERCENT, FULL_INK_PERCENT, FULL_INK_PERCENT, FULL_INK_PERCENT,
		LOW_INK_PERCENT, LOW_INK_PERCENT, LOW_INK_PERCENT, LOW_INK_PERCENT,
		percentages[INK_BLACK], percentages[INK_CYAN], percentages[INK_MAGENTA],
		percentages[INK_YELLOW]);
	fprintf(stderr, "STATE: %cmarker-supply-low-report\n", any_low ? '+' : '-');
	fprintf(stderr, "STATE: %cmarker-supply-empty-warning\n", any_empty ? '+' : '-');
}

int brother_report_ink_levels(void)
{
	unsigned int centipercent[BROTHER_INK_COLORS];

	/* Query failures publish no ATTR/STATE messages, preserving cached levels. */
	if (query_ink_levels(centipercent))
		return 1;
	publish_ink_levels(centipercent);
	return 0;
}
