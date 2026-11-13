/* Integer unit conversion for printable bounds and paper dimensions. */
#include "geometry.h"

/*
 * Multiply before dividing; signed division truncates toward zero.
 * Keep the intermediate as int: -fwrapv preserves the encoder's 32-bit wrap.
 * Callers must provide a nonzero denominator and a quotient that fits in int.
 */
int scale_ratio(int value, int numerator, int denominator)
{
	return (numerator * value) / denominator;
}
