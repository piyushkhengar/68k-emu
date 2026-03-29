/* bare-metal/include/math.h — shadow header.
 * Provides math constants and function declarations needed by ym2612.c.
 * Implementations are in bare.h / libbm.c.
 */
#ifndef _BM_MATH_H
#define _BM_MATH_H

#include "bare.h"

#ifndef M_PI
#define M_PI  3.14159265358979323846
#endif
#ifndef M_E
#define M_E   2.71828182845904523536
#endif

/* These are declared/defined in bare.h already: fabs, floor, ceil, pow.
 * Add the ones bare.h doesn't yet cover: */
double bm_sin(double x);
double bm_cos(double x);
double bm_log(double x);
double bm_log2(double x);
double bm_sqrt(double x);
double bm_exp(double x);

#define sin(x)   bm_sin(x)
#define cos(x)   bm_cos(x)
#define log(x)   bm_log(x)
#define log2(x)  bm_log2(x)
#define sqrt(x)  bm_sqrt(x)
#define exp(x)   bm_exp(x)

#endif /* _BM_MATH_H */
