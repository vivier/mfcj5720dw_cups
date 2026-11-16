/* Factory calibration payload, in the printer's K/C/M/Y channel order. */
#ifndef BROTHER_ENCODER_CALIBRATION_H
#define BROTHER_ENCODER_CALIBRATION_H

#include <stdint.h>

enum calibration_size {
	CALIBRATION_SETS = 3,
	CALIBRATION_CHANNELS = 4,
	CALIBRATION_SAMPLES = 421,
	CALIBRATION_PARAMETERS = 893,
	CALIBRATION_DENSITY_BANKS = 11,
	CALIBRATION_BYTES = 6033,
};

/* Byte arrays keep the cache independent of native integer layout. */
struct printer_calibration {
	uint8_t nozzle[CALIBRATION_SETS][CALIBRATION_CHANNELS][CALIBRATION_SAMPLES];
	uint8_t parameters[CALIBRATION_PARAMETERS];
	uint8_t density[CALIBRATION_DENSITY_BANKS][CALIBRATION_CHANNELS][2];
};

#endif
