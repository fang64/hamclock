/* a few handy spherical trig functions.
 */


#include "HamClock.h"



/* solve a spherical triangle:
 *           A
 *          /  \
 *         /    \
 *      c /      \ b
 *       /        \
 *      /          \
 *    B ____________ C
 *           a
 *
 * given A, b, c find B and a in range -PI..B..PI and 0..a..PI, respectively..
 * cap and Bp may be NULL if not interested in either one.
 * N.B. we pass in cos(c) and sin(c) because in many problems one of the sides
 *   remains constant for many values of A and b.
 */
void solveSphere (float A, float b, float cc, float sc, float *cap, float *Bp)
{
        float cb = cosf(b);
        float sb = sinf(b);
        float cA = cosf(A);
        float ca = CLAMPF (cb*cc + sb*sc*cA, -1, 1);

        if (cap)
            *cap = ca;

        if (Bp) {
            float sA = sinf(A);
            float y = sA*sb*sc;
            float x = cb - ca*cc;
            *Bp = atan2f (y,x);
        }
}


/* simple angular separation between two LatLong, all in rads.
 * to get path length in miles, multiply result by ERAD_M.
 */
float simpleSphereDist (const LatLong &ll1, const LatLong &ll2)
{
        float ca;
        solveSphere (ll1.lng - ll2.lng, M_PI_2F-ll2.lat, cosf (M_PI_2F-ll1.lat), sinf (M_PI_2F-ll1.lat),
                        &ca, NULL);
        return (acosf(ca));
}
