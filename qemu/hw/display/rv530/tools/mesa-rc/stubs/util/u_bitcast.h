/* Minimal replacement for Mesa's util/u_bitcast.h (absent from this Mesa
 * checkout).  Only the helpers used by the r300 compiler are provided. */
#ifndef RC_COMPILE_U_BITCAST_H
#define RC_COMPILE_U_BITCAST_H
#include <stdint.h>
#include <string.h>
static inline uint32_t u_bitcast_f2u(float f)
{
   uint32_t u; memcpy(&u, &f, sizeof(u)); return u;
}
static inline float u_bitcast_u2f(uint32_t u)
{
   float f; memcpy(&f, &u, sizeof(f)); return f;
}
static inline uint64_t u_bitcast_d2u(double d)
{
   uint64_t u; memcpy(&u, &d, sizeof(u)); return u;
}
static inline double u_bitcast_u2d(uint64_t u)
{
   double d; memcpy(&d, &u, sizeof(d)); return d;
}
#endif
