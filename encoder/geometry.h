/* Integer unit conversion for printable bounds and paper dimensions. */
#ifndef BROTHER_ENCODER_GEOMETRY_H
#define BROTHER_ENCODER_GEOMETRY_H

/* Signed division truncates toward zero; see geometry.c for arithmetic limits. */
int scale_ratio(int value, int numerator, int denominator);

#endif
