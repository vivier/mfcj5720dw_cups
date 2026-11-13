/* Select named printer choices and translate them to encoder values. */
#include "config.h"
#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum choice_result {
	CHOICE_SELECTED = 0,
	CHOICE_UNKNOWN = -1,
	CHOICE_DISABLED = -2,
};

enum choice_state {
	NO_CHOICE_SELECTED = -1,
	CHOICE_ENABLED = 1,
};

static struct config_option *option_by_index(struct configuration *config, int option_index)
{
	if (!config || !config->options || option_index < 0 || option_index >= config->option_count)
		return NULL;
	return &config->options[option_index];
}

static bool has_selected_choice(const struct config_option *option)
{
	return option && option->selected >= 0 && option->selected < option->choice_count;
}

char *current_option_text(const char *name)
{
	return get_option_text(name, &lpr_configuration);
}

int set_choice_option(int option_index, const char *choice_text, struct configuration *config,
		      int validate_choice)
{
	struct config_option *option = option_by_index(config, option_index);
	int choice_index;

	if (!option || !choice_text || !option->choices)
		return CHOICE_UNKNOWN;
	for (choice_index = 0; choice_index < option->choice_count; choice_index++) {
		if (strcmp(choice_text, option->choices[choice_index]))
			continue;
		/* Only the value 1 enables a choice; missing enable flags also reject it. */
		if (validate_choice &&
		    (choice_index >= (int)(sizeof(option->enabled) / sizeof(option->enabled[0])) ||
		     option->enabled[choice_index] != CHOICE_ENABLED)) {
			option->selected = NO_CHOICE_SELECTED;
			return CHOICE_DISABLED;
		}
		option->selected = choice_index;
		return CHOICE_SELECTED;
	}
	/* An unknown name leaves the previous selection unchanged. */
	return CHOICE_UNKNOWN;
}

int get_option_code(int option_index, struct configuration *config)
{
	struct config_option *option = option_by_index(config, option_index);

	/* Numeric lookup requires a selected choice; no code can represent failure. */
	if (!has_selected_choice(option) || !option->codes) {
		fputs("ERROR: Printer option has no selected numeric code.\n", stderr);
		abort();
	}
	return option->codes[option->selected];
}

char *get_option_text(const char *name, struct configuration *config)
{
	int option_index;

	if (!name || !config || !config->options)
		return NULL;
	for (option_index = 0; option_index < config->option_count; option_index++) {
		struct config_option *option = &config->options[option_index];

		if (strcmp(name, option->name))
			continue;
		/* Numeric-only options have no choice text, nor do rejected selections. */
		if (!has_selected_choice(option) || !option->choices)
			return NULL;
		return option->choices[option->selected];
	}
	return NULL;
}
