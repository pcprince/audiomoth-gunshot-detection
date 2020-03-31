/****************************************************************************
 * hmm.h
 * openacousticdevices.info
 * March 2018
 *****************************************************************************/

#include <stdint.h>

float FastExp(float x);
float lognormalpdf(float X, float mu, float p1, float variance);
int16_t calculate(float freq1[], float freq2[], float freq3[], int16_t T);
