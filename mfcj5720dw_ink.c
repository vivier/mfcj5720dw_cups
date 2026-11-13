/* Report ink estimates through the CUPS side channel, without printing. */
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ink.h"

#define MAX_COMMAND_BYTES 65536

static void usage(FILE *stream)
{
	fputs("Usage: mfcj5720dw_ink --report-levels\n"
	      "       mfcj5720dw_ink job-id user title copies options [file]\n"
	      "       mfcj5720dw_ink -h | --help\n",
	      stream);
}

static char *trim(char *line)
{
	size_t length;

	while (isspace((unsigned char)*line))
		line++;
	length = strlen(line);
	while (length && isspace((unsigned char)line[length - 1]))
		line[--length] = '\0';
	return line;
}

static int command_file(FILE *input)
{
	char line[256];
	size_t total = 0;
	bool requested = false;

	if (!fgets(line, sizeof(line), input) ||
	    strcmp(trim(line), "#CUPS-COMMAND"))
		goto invalid;
	while (fgets(line, sizeof(line), input)) {
		char *command;
		char *comment;
		size_t length = strlen(line);

		total += length;
		if (length == sizeof(line) - 1 || total > MAX_COMMAND_BYTES)
			goto invalid;
		comment = strchr(line, '#');
		if (comment)
			*comment = '\0';
		command = trim(line);
		if (!*command)
			continue;
		if (strcasecmp(command, "ReportLevels"))
			goto invalid;
		requested = true;
	}
	if (ferror(input))
		goto invalid;
	/* Validate the whole file before issuing at most one read-only query. */
	return requested ? brother_report_ink_levels() : 0;

invalid:
	fputs("ERROR: Invalid CUPS command file; only ReportLevels is supported.\n",
	      stderr);
	return 1;
}

int main(int argc, char **argv)
{
	FILE *input = stdin;
	int result;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout);
		puts("\nRead ink estimates through the CUPS backend and report marker attributes.\n"
		     "Requires CUPS side-channel fd 4 and a network backend supporting SNMP.\n"
		     "ReportLevels command jobs query supplies without printing a page.\n"
		     "No printer data is written to stdout; see mfcj5720dw_ink(1).");
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && !strcmp(argv[1], "--report-levels"))
		return brother_report_ink_levels();
	if (argc != 6 && argc != 7) {
		usage(stderr);
		return 2;
	}
	if (argc == 7) {
		input = fopen(argv[6], "r");
		if (!input) {
			perror("ERROR: Cannot open CUPS command file");
			return 1;
		}
	}
	result = command_file(input);
	if (input != stdin)
		fclose(input);
	return result;
}
