/*
 * Printer option names, labels and configuration values.
 *
 * These values feed job_settings; they are not necessarily wire commands or
 * renderer IDs. Preserve spelling, order and defaults for existing callers.
 */
#include "settings.h"

#include <stdint.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* DEFAULT_* selections are choice indices, not configuration codes. */
enum option_defaults {
	NO_DEFAULT_CHOICE = -1,
	CHOICE_AVAILABLE = 1,
	DEFAULT_PLAIN_NORMAL = 1,
	DEFAULT_TOGGLE_ON = 1,
	DEFAULT_MONOCHROME = 1,
	DEFAULT_CONSTRAINT_COUNT = 2,
	CONSTRAINT_CAPACITY = 104,
};

enum media_config_value {
	CONFIG_MEDIA_PLAIN_DUPLEX = 9,
	CONFIG_MEDIA_PLAIN = 0,
	CONFIG_MEDIA_INKJET = 1,
	CONFIG_MEDIA_BROTHER_BP71 = 8,
	CONFIG_MEDIA_BROTHER_BP60 = 12,
	CONFIG_MEDIA_OTHER_PHOTO = 2,
	CONFIG_MEDIA_HAGAKI_ADDRESS = 6,
	CONFIG_MEDIA_HAGAKI_REVERSE = 7,
};

static char *const media_labels[] = {
	"Plain Duplex Paper",
	"Plain Paper",
	"Inkjet Paper",
	"Brother BP71 Photo Paper",
	"Brother BP60 Matte Paper",
	"Other Photo Paper",
	"HagakiAtena",
	"HagakiUra",
};

static char *const media_choices[] = {
	"PlainDuplex",
	"Plain",
	"Inkjet",
	"BrotherGlossyR",
	"BrotherBP60Matte",
	"Glossy",
	"HagakiAtena",
	"HagakiUra",
};

static const int16_t media_codes[] = {
	CONFIG_MEDIA_PLAIN_DUPLEX,
	CONFIG_MEDIA_PLAIN,
	CONFIG_MEDIA_INKJET,
	CONFIG_MEDIA_BROTHER_BP71,
	CONFIG_MEDIA_BROTHER_BP60,
	CONFIG_MEDIA_OTHER_PHOTO,
	CONFIG_MEDIA_HAGAKI_ADDRESS,
	CONFIG_MEDIA_HAGAKI_REVERSE,
};

/* Normal and Best intentionally share the same configuration code. */
enum quality_config_value {
	CONFIG_QUALITY_PLAIN_FAST = 0,
	CONFIG_QUALITY_PLAIN_NORMAL = 1,
	CONFIG_QUALITY_FAST = 2,
	CONFIG_QUALITY_NORMAL = 3,
	CONFIG_QUALITY_PHOTO = 4,
	CONFIG_QUALITY_BEST = 3,
};

static char *const resolution_labels[] = {
	"Plain Fast",
	"Plain Normal",
	"Fast",
	"Normal",
	"Photo",
	"Best",
};

static char *const resolution_choices[] = {
	"PlainFast",
	"PlainNormal",
	"Fast",
	"Normal",
	"Photo",
	"Best",
};

static const int16_t resolution_codes[] = {
	CONFIG_QUALITY_PLAIN_FAST,
	CONFIG_QUALITY_PLAIN_NORMAL,
	CONFIG_QUALITY_FAST,
	CONFIG_QUALITY_NORMAL,
	CONFIG_QUALITY_PHOTO,
	CONFIG_QUALITY_BEST,
};

enum toggle_config_value {
	CONFIG_TOGGLE_OFF = 0,
	CONFIG_TOGGLE_ON = 1,
};

static char *const toggle_labels[] = {
	"Off",
	"On",
};

static char *const toggle_choices[] = {
	"OFF",
	"ON",
};

static const int16_t toggle_codes[] = {
	CONFIG_TOGGLE_OFF,
	CONFIG_TOGGLE_ON,
};

/* Some paper names share a code; these are not protocol.h paper IDs. */
enum paper_config_value {
	CONFIG_PAPER_A3 = 8,
	CONFIG_PAPER_A3_BORDERLESS = 290,
	CONFIG_PAPER_JIS_B4 = 12,
	CONFIG_PAPER_TABLOID = 3,
	CONFIG_PAPER_TABLOID_BORDERLESS = 285,
	CONFIG_PAPER_A4 = 9,
	CONFIG_PAPER_LETTER = 1,
	CONFIG_PAPER_LEGAL = 4,
	CONFIG_PAPER_FOLIO = 14,
	CONFIG_PAPER_EXECUTIVE = 7,
	CONFIG_PAPER_JIS_B5 = 13,
	CONFIG_PAPER_A5 = 11,
	CONFIG_PAPER_A6 = 70,
	CONFIG_PAPER_PHOTO_4X6 = 265,
	CONFIG_PAPER_PHOTO_5X8 = 266,
	CONFIG_PAPER_PHOTO_L = 272,
	CONFIG_PAPER_PHOTO_2L = 273,
	CONFIG_PAPER_HAGAKI = 43,
	CONFIG_PAPER_DOUBLE_HAGAKI = 82,
	CONFIG_PAPER_ENVELOPE_C5 = 28,
	CONFIG_PAPER_ENVELOPE_DL = 27,
	CONFIG_PAPER_ENVELOPE_10 = 20,
	CONFIG_PAPER_ENVELOPE_MONARCH = 37,
	CONFIG_PAPER_ENVELOPE_C6 = 261,
	CONFIG_PAPER_ENVELOPE_YOU4 = 91,
	CONFIG_PAPER_ENVELOPE_CHOU3 = 73,
	CONFIG_PAPER_ENVELOPE_CHOU4 = 74,
	CONFIG_PAPER_ENVELOPE_KAKU2 = 71,
	CONFIG_PAPER_CUSTOM = 256,
};

static char *const paper_size_labels[] = {
	"A3",
	"A3 (Borderless)",
	"JB4",
	"Tabloid",
	"Tabloid (Borderless)",
	"A4",
	"A4 (Borderless)",
	"Letter",
	"US Letter (Borderless)",
	"Legal",
	"Folio",
	"Executive",
	"JIS B5",
	"A5",
	"A6",
	"A6 (Borderless)",
	"4 x 6 in",
	"4 x 6 (Borderless)",
	"5 x 8 in",
	"5 x 8 in (Borderless)",
	"3.5 x 5 in",
	"3.5 x 5 in (Borderless)",
	"5 x 7 in",
	"5 x 7 in (Borderless)",
	"Postcard 1",
	"Postcard 1 (Borderless)",
	"Postcard 2 (Double)",
	"C5 Envelope",
	"DL Envelope",
	"Com-10",
	"Monarch",
	"C6 Envelope",
	"Y4 Envelope",
	"N3 Envelope",
	"N4 Envelope",
	"K2 Envelope",
	"Custom",
};

static char *const paper_size_choices[] = {
	"A3",
	"BrA3_B_S",
	"B4_S",
	"Tabloid_S",
	"BrLedger_B_B",
	"A4",
	"BrA4_B",
	"Letter",
	"BrLetter_B",
	"Legal",
	"Folio",
	"Executive",
	"B5",
	"A5",
	"A6",
	"BrA6_B",
	"BrPostC4x6_S",
	"BrPostC4x6_B",
	"BrIndexC5x8_S",
	"BrIndexC5x8_B",
	"BrPhotoL_S",
	"BrPhotoL_B",
	"BrPhoto2L_S",
	"BrPhoto2L_B",
	"Postcard",
	"BrHagaki_B",
	"DoublePostcardRotated",
	"EnvC5",
	"EnvDL",
	"Env10",
	"EnvMonarch",
	"EnvC6",
	"EnvYou4",
	"EnvChou3",
	"EnvChou4",
	"EnvKaku2",
	"Custom",
};

static const int16_t paper_size_codes[] = {
	CONFIG_PAPER_A3,
	CONFIG_PAPER_A3_BORDERLESS,
	CONFIG_PAPER_JIS_B4,
	CONFIG_PAPER_TABLOID,
	CONFIG_PAPER_TABLOID_BORDERLESS,
	CONFIG_PAPER_A4,
	CONFIG_PAPER_A4,
	CONFIG_PAPER_LETTER,
	CONFIG_PAPER_LETTER,
	CONFIG_PAPER_LEGAL,
	CONFIG_PAPER_FOLIO,
	CONFIG_PAPER_EXECUTIVE,
	CONFIG_PAPER_JIS_B5,
	CONFIG_PAPER_A5,
	CONFIG_PAPER_A6,
	CONFIG_PAPER_A6,
	CONFIG_PAPER_PHOTO_4X6,
	CONFIG_PAPER_PHOTO_4X6,
	CONFIG_PAPER_PHOTO_5X8,
	CONFIG_PAPER_PHOTO_5X8,
	CONFIG_PAPER_PHOTO_L,
	CONFIG_PAPER_PHOTO_L,
	CONFIG_PAPER_PHOTO_2L,
	CONFIG_PAPER_PHOTO_2L,
	CONFIG_PAPER_HAGAKI,
	CONFIG_PAPER_HAGAKI,
	CONFIG_PAPER_DOUBLE_HAGAKI,
	CONFIG_PAPER_ENVELOPE_C5,
	CONFIG_PAPER_ENVELOPE_DL,
	CONFIG_PAPER_ENVELOPE_10,
	CONFIG_PAPER_ENVELOPE_MONARCH,
	CONFIG_PAPER_ENVELOPE_C6,
	CONFIG_PAPER_ENVELOPE_YOU4,
	CONFIG_PAPER_ENVELOPE_CHOU3,
	CONFIG_PAPER_ENVELOPE_CHOU4,
	CONFIG_PAPER_ENVELOPE_KAKU2,
	CONFIG_PAPER_CUSTOM,
};

enum thickness_config_value {
	CONFIG_THICKNESS_REGULAR = 0,
	CONFIG_THICKNESS_THICK = 2,
	CONFIG_THICKNESS_ENVELOPE = 4,
};

static char *const paper_thickness_labels[] = {
	"Regular",
	"Thick",
	"Envelope",
};

static char *const paper_thickness_choices[] = {
	"Regular",
	"Thick",
	"Env",
};

static const int16_t paper_thickness_codes[] = {
	CONFIG_THICKNESS_REGULAR,
	CONFIG_THICKNESS_THICK,
	CONFIG_THICKNESS_ENVELOPE,
};

enum source_config_value {
	CONFIG_SOURCE_AUTOMATIC = 0,
	CONFIG_SOURCE_TRAY_1 = 1,
	CONFIG_SOURCE_TRAY_2 = 2,
	CONFIG_SOURCE_MULTIPURPOSE = 3,
};

static char *const paper_source_labels[] = {
	"Auto Select",
	"Tray1",
	"Tray2",
	"MP Tray",
};

static char *const paper_source_choices[] = {
	"AutoSelect",
	"Tray1",
	"Tray2",
	"MPTray",
};

static const int16_t paper_source_codes[] = {
	CONFIG_SOURCE_AUTOMATIC,
	CONFIG_SOURCE_TRAY_1,
	CONFIG_SOURCE_TRAY_2,
	CONFIG_SOURCE_MULTIPURPOSE,
};

enum color_config_value {
	CONFIG_COLOR_COLOR = 0,
	CONFIG_COLOR_MONO = 1,
};

static char *const color_mode_labels[] = {
	"Color",
	"Grayscale",
};

static char *const color_mode_choices[] = {
	"Color",
	"Mono",
};

static const int16_t color_mode_codes[] = {
	CONFIG_COLOR_COLOR,
	CONFIG_COLOR_MONO,
};

enum matching_config_value {
	CONFIG_MATCHING_NATURAL = 1,
	CONFIG_MATCHING_VIVID = 2,
	CONFIG_MATCHING_NONE = 0,
};

static char *const color_matching_labels[] = {
	"Natural",
	"Vivid",
	"None",
};

static char *const color_matching_choices[] = {
	"Natural",
	"Vivid",
	"None",
};

static const int16_t color_matching_codes[] = {
	CONFIG_MATCHING_NATURAL,
	CONFIG_MATCHING_VIVID,
	CONFIG_MATCHING_NONE,
};

enum jpeg_config_value {
	CONFIG_JPEG_RECOMMENDED = 2,
	CONFIG_JPEG_QUALITY = 0,
	CONFIG_JPEG_SPEED = 3,
};

static char *const jpeg_priority_labels[] = {
	"Recommended Setting",
	"Improved Print Quality",
	"Improved Print Speed",
};

static char *const jpeg_priority_choices[] = {
	"Recommended",
	"QualityPrior",
	"SpeedPrior",
};

static const int16_t jpeg_priority_codes[] = {
	CONFIG_JPEG_RECOMMENDED,
	CONFIG_JPEG_QUALITY,
	CONFIG_JPEG_SPEED,
};

enum halftone_config_value {
	CONFIG_HALFTONE_DIFFUSION = 1,
	CONFIG_HALFTONE_DITHER = 0,
};

static char *const halftone_labels[] = {
	"Diffusion",
	"Dither",
};

static char *const halftone_choices[] = {
	"Diffusion",
	"Dither",
};

static const int16_t halftone_codes[] = {
	CONFIG_HALFTONE_DIFFUSION,
	CONFIG_HALFTONE_DITHER,
};

enum duplex_config_value {
	CONFIG_DUPLEX_OFF = 0,
	CONFIG_DUPLEX_LONG_EDGE = 1,
	CONFIG_DUPLEX_SHORT_EDGE = 2,
};

static char *const duplex_binding_labels[] = {
	"Off",
	"Long-Edge Binding",
	"Short-Edge Binding",
};

static char *const duplex_binding_choices[] = {
	"None",
	"DuplexNoTumble",
	"DuplexTumble",
};

static const int16_t duplex_binding_codes[] = {
	CONFIG_DUPLEX_OFF,
	CONFIG_DUPLEX_LONG_EDGE,
	CONFIG_DUPLEX_SHORT_EDGE,
};

enum drying_config_value {
	CONFIG_DRYING_NORMAL = 0,
	CONFIG_DRYING_SLOW = 1,
	CONFIG_DRYING_REDUCED_INK = 2,
};

static char *const duplex_drying_labels[] = {
	"Normal",
	"Duplex 1 (Slower print speed)",
	"Duplex 2 (Slower print speed & less ink)",
};

static char *const duplex_drying_choices[] = {
	"Normal",
	"Duplex1",
	"Duplex2",
};

static const int16_t duplex_drying_codes[] = {
	CONFIG_DRYING_NORMAL,
	CONFIG_DRYING_SLOW,
	CONFIG_DRYING_REDUCED_INK,
};

/*
 * Selections and enable flags belong to each option, even for shared tables.
 * Zero-initialized fields retain the original defaults. Only the two duplex
 * options initially enable their choices; CUPS selects choices without this
 * legacy availability check.
 */
static struct config_option options[] = {
	[OPTION_MEDIA] = {
		.name = "Media",
		.alias = "-md",
		.choice_count = ARRAY_SIZE(media_choices),
		.labels = media_labels,
		.choices = media_choices,
		.codes = media_codes,
	},
	[OPTION_RESOLUTION] = {
		.name = "BResolution",
		.alias = "-reso",
		.choice_count = ARRAY_SIZE(resolution_choices),
		.labels = resolution_labels,
		.choices = resolution_choices,
		.codes = resolution_codes,
		.selected = DEFAULT_PLAIN_NORMAL,
	},
	[OPTION_BIDIRECTIONAL] = {
		.name = "BiDirPrint",
		.alias = "-bidir",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
		.selected = DEFAULT_TOGGLE_ON,
	},
	[OPTION_PAPER_SIZE] = {
		.name = "PaperType",
		.alias = "-pt",
		.choice_count = ARRAY_SIZE(paper_size_choices),
		.labels = paper_size_labels,
		.choices = paper_size_choices,
		.codes = paper_size_codes,
	},
	[OPTION_PAPER_THICKNESS] = {
		.name = "PaperThick",
		.alias = "-thick",
		.choice_count = ARRAY_SIZE(paper_thickness_choices),
		.labels = paper_thickness_labels,
		.choices = paper_thickness_choices,
		.codes = paper_thickness_codes,
	},
	[OPTION_PAPER_SOURCE] = {
		.name = "PaperSource",
		.alias = "-feed",
		.choice_count = ARRAY_SIZE(paper_source_choices),
		.labels = paper_source_labels,
		.choices = paper_source_choices,
		.codes = paper_source_codes,
	},
	[OPTION_COLOR_MODE] = {
		.name = "ColorOrMono",
		.alias = "-corm",
		.choice_count = ARRAY_SIZE(color_mode_choices),
		.labels = color_mode_labels,
		.choices = color_mode_choices,
		.codes = color_mode_codes,
		.selected = DEFAULT_MONOCHROME,
	},
	[OPTION_COLOR_MATCHING] = {
		.name = "ColorMatch",
		.alias = "-cm",
		.choice_count = ARRAY_SIZE(color_matching_choices),
		.labels = color_matching_labels,
		.choices = color_matching_choices,
		.codes = color_matching_codes,
	},
	[OPTION_JPEG_PRIORITY] = {
		.name = "JpegPrinting",
		.alias = "-jpeg",
		.choice_count = ARRAY_SIZE(jpeg_priority_choices),
		.labels = jpeg_priority_labels,
		.choices = jpeg_priority_choices,
		.codes = jpeg_priority_codes,
	},
	[OPTION_HALFTONE] = {
		.name = "HalfTone",
		.alias = "-ht",
		.choice_count = ARRAY_SIZE(halftone_choices),
		.labels = halftone_labels,
		.choices = halftone_choices,
		.codes = halftone_codes,
	},
	[OPTION_COLOR_ENHANCEMENT] = {
		.name = "ColorEnhance",
		.alias = "-ce",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_SLOW_DRYING] = {
		.name = "SlowDrying",
		.alias = "-sd",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_ALIGN_OTHER] = {
		.name = "BRVertLineAlignOther",
		.alias = "-vlother",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_ALIGN_A4_LETTER] = {
		.name = "BRVertLineAlignA4Letter",
		.alias = "-vla4letter",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_ALIGN_A3_B4_LEDGER] = {
		.name = "BRVertLineAlignA3B4Ledger",
		.alias = "-vla3b4ledger",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_MIRROR] = {
		.name = "BRMirror",
		.alias = "-mirro",
		.choice_count = ARRAY_SIZE(toggle_choices),
		.labels = toggle_labels,
		.choices = toggle_choices,
		.codes = toggle_codes,
	},
	[OPTION_DUPLEX_BINDING] = {
		.name = "BRDuplex",
		.alias = "-duplex",
		.choice_count = ARRAY_SIZE(duplex_binding_choices),
		.labels = duplex_binding_labels,
		.choices = duplex_binding_choices,
		.codes = duplex_binding_codes,
		.enabled = { CHOICE_AVAILABLE, CHOICE_AVAILABLE, CHOICE_AVAILABLE },
	},
	[OPTION_DUPLEX_DRYING] = {
		.name = "BRDuplexMode",
		.alias = "-duplexmode",
		.choice_count = ARRAY_SIZE(duplex_drying_choices),
		.labels = duplex_drying_labels,
		.choices = duplex_drying_choices,
		.codes = duplex_drying_codes,
		.enabled = { CHOICE_AVAILABLE, CHOICE_AVAILABLE, CHOICE_AVAILABLE },
	},
	[OPTION_BRIGHTNESS] = {
		.name = "Brightness",
		.alias = "-brit",
		.choice_count = 0,
		.selected = NO_DEFAULT_CHOICE,
	},
	[OPTION_CONTRAST] = {
		.name = "Contrast",
		.alias = "-cont",
		.choice_count = 0,
		.selected = NO_DEFAULT_CHOICE,
	},
	[OPTION_RED] = {
		.name = "RedKey",
		.alias = "-red",
		.choice_count = 0,
		.selected = NO_DEFAULT_CHOICE,
	},
	[OPTION_GREEN] = {
		.name = "GreenKey",
		.alias = "-green",
		.choice_count = 0,
		.selected = NO_DEFAULT_CHOICE,
	},
	[OPTION_BLUE] = {
		.name = "BlueKey",
		.alias = "-blue",
		.choice_count = 0,
		.selected = NO_DEFAULT_CHOICE,
	},
};

/* Retained legacy constraint syntax; the direct CUPS path uses PPD checks. */
static char borderless_resolution_constraint[] =
	"PaperType={A4_B,Letter_B,A6_B,Photo_B,IndexCard_B,PhotoL_B,Photo2L_B,Hagaki_B}"
	"*RResolution={300,600,750,1200,1200x2400}";

static char borderless_media_constraint[] =
	"PaperType={A4_B,Letter_B,A6_B,Photo_B,IndexCard_B,PhotoL_B,Photo2L_B,Hagaki_B}"
	"*Media={PlainPaper,InkjetPaper,GlossyPaper,SlowDryingPaper}";

static char *constraints[CONSTRAINT_CAPACITY] = {
	borderless_resolution_constraint,
	borderless_media_constraint,
};

struct configuration lpr_configuration = {
	.option_count = OPTION_COUNT,
	.options = options,
	.default_constraints = DEFAULT_CONSTRAINT_COUNT,
	.constraint_count = DEFAULT_CONSTRAINT_COUNT,
	.constraints = constraints,
};
