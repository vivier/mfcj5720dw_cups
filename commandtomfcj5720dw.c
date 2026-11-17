// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 Laurent Vivier */
/* CUPS maintenance jobs; the backend owns the printer connection. */
#include <ctype.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ink.h"

#define MAX_COMMAND_BYTES 65536
#define MAX_LINE_BYTES 255
#define MAX_COMMANDS 256
#define PRINTER_PREAMBLE_BYTES 4096
#define PJL_UEL "\033%-12345X"
#define CLEAN_STRONG "CleanStrong"
#define CLEAN_STRONGEST "CleanStrongest"

enum maintenance_command {
	COMMAND_INVALID,
	COMMAND_EMPTY,
	COMMAND_REPORT_LEVELS,
	COMMAND_CLEAN_ALL,
	COMMAND_CLEAN_BLACK,
	COMMAND_CLEAN_COLOR,
	COMMAND_CLEAN_STRONG_ALL,
	COMMAND_CLEAN_STRONG_BLACK,
	COMMAND_CLEAN_STRONG_COLOR,
	COMMAND_CLEAN_STRONGEST_ALL,
	COMMAND_CLEAN_STRONGEST_BLACK,
	COMMAND_CLEAN_STRONGEST_COLOR,
	COMMAND_PRINT_SELF_TEST,
};

static void usage(FILE *stream)
{
	fputs("Usage: commandtomfcj5720dw job-id user title copies options [file]\n"
	      "       commandtomfcj5720dw -h | --help\n", stream);
}

static char *trim(char *text)
{
	size_t length;

	while (isspace((unsigned char)*text))
		text++;
	length = strlen(text);
	while (length && isspace((unsigned char)text[length - 1]))
		text[--length] = '\0';
	return text;
}

static enum maintenance_command parse_command(char *line)
{
	static const struct {
		const char *name;
		enum maintenance_command all;
		enum maintenance_command black;
		enum maintenance_command color;
	} cleaning_modes[] = {
		{ "Clean", COMMAND_CLEAN_ALL, COMMAND_CLEAN_BLACK,
		  COMMAND_CLEAN_COLOR },
		{ CLEAN_STRONG, COMMAND_CLEAN_STRONG_ALL,
		  COMMAND_CLEAN_STRONG_BLACK, COMMAND_CLEAN_STRONG_COLOR },
		{ CLEAN_STRONGEST, COMMAND_CLEAN_STRONGEST_ALL,
		  COMMAND_CLEAN_STRONGEST_BLACK, COMMAND_CLEAN_STRONGEST_COLOR },
	};
	char *comment = strchr(line, '#');
	char *command, *argument;
	size_t i;

	if (comment)
		*comment = '\0';
	command = trim(line);
	if (!*command)
		return COMMAND_EMPTY;
	if (!strcasecmp(command, "ReportLevels"))
		return COMMAND_REPORT_LEVELS;
	if (!strcasecmp(command, "PrintSelfTestPage"))
		return COMMAND_PRINT_SELF_TEST;

	argument = strpbrk(command, " \t\r");
	if (!argument)
		return COMMAND_INVALID;
	*argument++ = '\0';
	argument = trim(argument);
	/* Strong cleaning is an explicit extension; standard Clean stays normal. */
	for (i = 0; i < sizeof(cleaning_modes) / sizeof(cleaning_modes[0]); i++) {
		if (strcasecmp(command, cleaning_modes[i].name))
			continue;
		if (!strcasecmp(argument, "all"))
			return cleaning_modes[i].all;
		if (!strcasecmp(argument, "black"))
			return cleaning_modes[i].black;
		if (!strcasecmp(argument, "color"))
			return cleaning_modes[i].color;
		return COMMAND_INVALID;
	}
	return COMMAND_INVALID;
}

static int read_commands(FILE *input, enum maintenance_command *commands,
			 size_t *count)
{
	char line[MAX_LINE_BYTES + 1];
	size_t total = 0, length = 0;
	bool header = true;
	int byte;

	*count = 0;
	do {
		enum maintenance_command command;

		byte = fgetc(input);
		if (byte != EOF && ++total > MAX_COMMAND_BYTES)
			goto invalid;
		if (byte != EOF && byte != '\n') {
			/* Reject NULs and non-ASCII data, even inside comments. */
			if (((byte < ' ' || byte > '~') &&
			     byte != '\t' && byte != '\r') ||
			    length == MAX_LINE_BYTES)
				goto invalid;
			line[length++] = byte;
			continue;
		}
		line[length] = '\0';
		length = 0;
		if (header) {
			if (strcmp(trim(line), "#CUPS-COMMAND"))
				goto invalid;
			header = false;
			continue;
		}
		command = parse_command(line);
		if (command == COMMAND_INVALID)
			goto invalid;
		if (command == COMMAND_EMPTY)
			continue;
		if (*count == MAX_COMMANDS)
			goto invalid;
		commands[(*count)++] = command;
	} while (byte != EOF);
	if (!ferror(input))
		return 0;

invalid:
	fprintf(stderr,
		"ERROR: Invalid CUPS command file; see commandtomfcj5720dw(1) "
		"for supported commands (at most %d bytes, %d bytes per line "
		"and %d commands).\n",
		MAX_COMMAND_BYTES, MAX_LINE_BYTES, MAX_COMMANDS);
	return 1;
}

static int write_printer_command(const char *command)
{
	static const unsigned char preamble[PRINTER_PREAMBLE_BYTES];

	/* Inkjet framing: zero preamble, UEL, one PJL line, UEL. */
	if (fwrite(preamble, 1, sizeof(preamble), stdout) != sizeof(preamble) ||
	    fputs(PJL_UEL, stdout) == EOF ||
	    fputs(command, stdout) == EOF ||
	    fputs(PJL_UEL, stdout) == EOF || fflush(stdout) == EOF) {
		fputs("ERROR: Cannot write printer maintenance command.\n", stderr);
		return 1;
	}
	return 0;
}

static int run_commands(const enum maintenance_command *commands, size_t count)
{
	bool levels_reported = false;
	unsigned int page = 0;
	size_t i;

	for (i = 0; i < count; i++) {
		const char *pjl;

		switch (commands[i]) {
		case COMMAND_REPORT_LEVELS:
			/* Preserve the ink filter's single query per command job. */
			if (!levels_reported && brother_report_ink_levels())
				return 1;
			levels_reported = true;
			continue;
		case COMMAND_CLEAN_ALL:
			pjl = "@PJL EXECUTE NORMALPURGE=ALL\r\n";
			break;
		case COMMAND_CLEAN_BLACK:
			pjl = "@PJL EXECUTE NORMALPURGE=BLACK\r\n";
			break;
		case COMMAND_CLEAN_COLOR:
			pjl = "@PJL EXECUTE NORMALPURGE=COLOR\r\n";
			break;
		case COMMAND_CLEAN_STRONG_ALL:
			pjl = "@PJL EXECUTE POWERPURGE=ALL\r\n";
			break;
		case COMMAND_CLEAN_STRONG_BLACK:
			pjl = "@PJL EXECUTE POWERPURGE=BLACK\r\n";
			break;
		case COMMAND_CLEAN_STRONG_COLOR:
			pjl = "@PJL EXECUTE POWERPURGE=COLOR\r\n";
			break;
		case COMMAND_CLEAN_STRONGEST_ALL:
			pjl = "@PJL EXECUTE SUPERPOWERPURGE=ALL\r\n";
			break;
		case COMMAND_CLEAN_STRONGEST_BLACK:
			pjl = "@PJL EXECUTE SUPERPOWERPURGE=BLACK\r\n";
			break;
		case COMMAND_CLEAN_STRONGEST_COLOR:
			pjl = "@PJL EXECUTE SUPERPOWERPURGE=COLOR\r\n";
			break;
		case COMMAND_PRINT_SELF_TEST:
			pjl = "@PJL EXECUTE PINCHKPRINT\r\n";
			break;
		default:
			return 1;
		}
		if (write_printer_command(pjl))
			return 1;
		if (commands[i] == COMMAND_PRINT_SELF_TEST)
			fprintf(stderr, "PAGE: %u 1\n", ++page);
	}
	return 0;
}

int main(int argc, char **argv)
{
	enum maintenance_command commands[MAX_COMMANDS];
	FILE *input = stdin;
	size_t count;
	int result;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout);
		puts("\nCUPS command jobs:\n"
		     "  ReportLevels\n"
		     "  PrintSelfTestPage\n"
		     "  Clean all|black|color (normal cleaning)\n"
		     "  " CLEAN_STRONG " all|black|color (strong cleaning)\n"
		     "  " CLEAN_STRONGEST " all|black|color (strongest cleaning)\n"
		     "Submit through CUPS; see commandtomfcj5720dw(1).");
		return 0;
	}
	if (argc != 6 && argc != 7) {
		usage(stderr);
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	if (argc == 7) {
		input = fopen(argv[6], "r");
		if (!input) {
			perror("ERROR: Cannot open CUPS command file");
			return 1;
		}
	}
	/* No query or printer byte may escape before the entire job is valid. */
	result = read_commands(input, commands, &count);
	if (input != stdin)
		fclose(input);
	if (result)
		return result;
	return run_commands(commands, count);
}
