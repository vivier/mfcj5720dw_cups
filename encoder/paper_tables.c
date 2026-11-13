/*
 * Printer paper identifiers and custom-paper margin defaults.
 * Numeric data transcribed from the Brother MFC-J5720DW LPR driver.
 * See PROVENANCE.md for origin and retained notices.
 */

#include "data.h"

const struct paper_id_entry lpr_paper_id_table[42] = {
	{8, "A3"},
	{283, "BrA3_B_S"},
	{3, "Tabloid_S"},
	{285, "BrLedger_B_B"},
	{12, "B4_S"},
	{284, "BrB4_B_B"},
	{9, "A4"},
	{9, "BrA4_B"},
	{1, "Letter"},
	{1, "BrLetter_B"},
	{5, "Legal"},
	{14, "Folio"},
	{7, "Executive"},
	{13, "B5"},
	{11, "A5"},
	{70, "A6"},
	{70, "BrA6_B"},
	{265, "BrPostC4x6_S"},
	{265, "BrPostC4x6_B"},
	{266, "BrIndexC5x8_S"},
	{266, "BrIndexC5x8_B"},
	{272, "BrPhotoL_S"},
	{272, "BrPhotoL_B"},
	{273, "BrPhoto2L_S"},
	{273, "BrPhoto2L_B"},
	{43, "Postcard"},
	{43, "BrHagaki_B"},
	{82, "DoublePostcardRotated"},
	{28, "EnvC5"},
	{27, "EnvDL"},
	{20, "Env10"},
	{37, "EnvMonarch"},
	{91, "EnvYou4"},
	{73, "EnvChou3"},
	{74, "EnvChou4"},
	{71, "EnvKaku2"},
	{308, "Br12cmDisc_B"},
	{34, "ISOB5"},
	{52, "TabloidExtra"},
	{58, "SuperB"},
	{88, "B6"},
	{256, "EnvC6"},
};

const struct custom_paper_record lpr_custom_paper_records[1] = {
	{"Custom", 0x1.8000000000000p-1, 0x1.8000000000000p-1},
};
