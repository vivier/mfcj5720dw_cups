/* Choice selection and lookup in the current job configuration. */
#ifndef BROTHER_ENCODER_CONFIG_H
#define BROTHER_ENCODER_CONFIG_H

struct configuration;

/* Returned option text is borrowed; the caller must not free it. */
char *current_option_text(const char *name);
int set_choice_option(int option_index, const char *choice_text, struct configuration *config,
		      int validate_choice);
int get_option_code(int option_index, struct configuration *config);
char *get_option_text(const char *name, struct configuration *config);

#endif
