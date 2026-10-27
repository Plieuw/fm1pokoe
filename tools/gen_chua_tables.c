/* SPDX-License-Identifier: GPL-3.0-only */
/* Generates firmware/src/chua_tables.h: where the Chua oscillator's regimes lie, and how fast and how loud it is in
 * each, measured by simulating the circuit (RK4, small steps, long runs) on a grid of SHAPE (beta) x CHAOS (knob).
 *   cc -O2 -o build/gen_chua_tables tools/gen_chua_tables.c -lm && build/gen_chua_tables > firmware/src/chua_tables.h
 *
 * The circuit, dimensionless (x: the voltage over C1, y: over C2, z: the current in L):
 *   dx/dt = alpha (y - x - g(x))      dy/dt = x - y + z      dz/dt = -beta y
 * g: the Chua diode, piecewise linear: slope M0 inside |x| < 1, M1 out to |x| < E2, M2 beyond (the op-amps saturate:
 * that keeps every setting bounded, as in a real circuit; past the double scroll it settles on a large outer cycle).
 *
 * Per SHAPE row, the regimes along alpha are found first (the landmarks): the Hopf point where the equilibrium starts
 * to oscillate, period 2, the spiral (single-scroll chaos), the double scroll, the outer cycle. The CHAOS knob is laid
 * on them (CHAOS_KNOB), so a knob position means the same regime whatever SHAPE is. Then, at each grid point: alpha,
 * the rotation frequency (cycles per unit time: upward zero crossings of y), and the RMS of x, y, z (the output gains). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define M0 (-8.0 / 7.0)
#define M1 (-5.0 / 7.0)
#define M2 2.0
#define E2 3.0
#define NB 7                     /* SHAPE rows: beta 10, 14, .. 34 */
#define NK 17                    /* CHAOS columns: knob 0, 8, .. 128 */
static const double KNOB[6] = {0, 32, 48, 60, 112, 128};   /* knob at each landmark (see chaos_alpha) */

static double g(double x)
{
    double ax = fabs(x), s = x < 0 ? -1 : 1;
    if (ax < 1) return M0 * x;
    if (ax < E2) return s * (M0 + M1 * (ax - 1));
    return s * (M0 + M1 * (E2 - 1) + M2 * (ax - E2));
}
static void deriv(double a, double b, const double *s, double *o)
{
    o[0] = a * (s[1] - s[0] - g(s[0]));
    o[1] = s[0] - s[1] + s[2];
    o[2] = -b * s[1];
}

typedef struct { int regime; double f, rx, ry, rz; } meas_t;
/* regime: 0 rest, 1 cycle, 2 period 2, 3 higher period, 4 spiral (chaos, one scroll), 5 double scroll, 6 outer cycle */
static meas_t sim(double a, double b)
{
    double s[3] = {0.1, 0, 0}, h = 0.002, k1[3], k2[3], k3[3], k4[3], t[3], ymx = 0, sx = 0, sxx = 0, sy = 0, sz = 0,
           szz = 0, py = 0, p1 = 0, p2 = 0, pk[3000], u[16];
    int n = (int)(800 / h), i, j, up = 0, xpos = 0, xneg = 0, np = 0, m = 0, nd = 0;
    meas_t r;
    for (i = 0; i < n; i++) {
        deriv(a, b, s, k1);
        for (j = 0; j < 3; j++) t[j] = s[j] + h / 2 * k1[j];
        deriv(a, b, t, k2);
        for (j = 0; j < 3; j++) t[j] = s[j] + h / 2 * k2[j];
        deriv(a, b, t, k3);
        for (j = 0; j < 3; j++) t[j] = s[j] + h * k3[j];
        deriv(a, b, t, k4);
        for (j = 0; j < 3; j++) s[j] += h / 6 * (k1[j] + 2 * k2[j] + 2 * k3[j] + k4[j]);
        if (i > n / 2) {
            m++;
            sx += s[0]; sxx += s[0] * s[0]; sy += s[1] * s[1]; sz += s[2]; szz += s[2] * s[2];
            if (fabs(s[1]) > ymx) ymx = fabs(s[1]);
            if (py < 0 && s[1] >= 0) up++;
            if (s[0] > 0.3) xpos = 1;
            if (s[0] < -0.3) xneg = 1;
            if (p1 > p2 && p1 > s[0] && np < 3000) pk[np++] = p1;
        }
        py = s[1]; p2 = p1; p1 = s[0];
    }
    r.f = up / (m * h);
    r.rx = sqrt(fmax(sxx / m - (sx / m) * (sx / m), 0));
    r.ry = sqrt(sy / m);
    r.rz = sqrt(fmax(szz / m - (sz / m) * (sz / m), 0));
    for (i = 0; i < np && nd < 16; i++) {
        int k, seen = 0;
        for (k = 0; k < nd; k++) seen |= fabs(u[k] - pk[i]) < 2e-3;
        if (!seen) u[nd++] = pk[i];
    }
    r.regime = ymx < 0.05 ? 0 : ymx > 2 ? 6 : xpos && xneg ? 5 : nd <= 1 ? 1 : nd == 2 ? 2 : nd < 16 ? 3 : 4;
    return r;
}

int main(void)
{
    double alpha[NB][NK], f[NB][NK], gx[NB][NK], gy[NB][NK], gz[NB][NK], land[NB][6];
    int bi, k, i;
    for (bi = 0; bi < NB; bi++) {
        double b = 10 + 4 * bi, a, l[5] = {0, 0, 0, 0, 0};
        int stage = 0;
        for (a = 4; a < 40 && stage < 5; a += 0.025) {       /* the landmarks, in order (a skipped one: the same alpha) */
            static const int T[5] = {1, 2, 4, 5, 6};
            meas_t m = sim(a, b);
            int r = m.regime;
            if (stage == 0 && (r != 1 || m.ry < 0.1))           /* (just past the Hopf point the cycle grows slowly:
                                                                 * not settled, its peaks still differ) */
                continue;
            if (stage == 1 && a < l[0] + 0.5)
                continue;
            while (stage < 5 && r >= T[stage])
                l[stage++] = a;
        }
        land[bi][0] = l[0];                   /* a cycle already grown, not the birth of it */
        land[bi][1] = l[1];
        land[bi][2] = l[2];
        land[bi][3] = l[3];
        land[bi][4] = l[4] - 0.1;
        land[bi][5] = l[4] + 2.0;
        fprintf(stderr, "beta %4.1f: cycle %.3f  period-2 %.3f  spiral %.3f  double %.3f  outer %.3f\n", b, l[0], l[1], l[2],
                l[3], l[4]);
        for (k = 0; k < NK; k++) {
            double kn = k * 8.0, t;
            meas_t m;
            for (i = 0; i < 4 && kn > KNOB[i + 1]; i++)
                ;
            t = (kn - KNOB[i]) / (KNOB[i + 1] - KNOB[i]);
            alpha[bi][k] = land[bi][i] + t * (land[bi][i + 1] - land[bi][i]);
            m = sim(alpha[bi][k], b);
            f[bi][k] = m.f;
            gx[bi][k] = m.rx; gy[bi][k] = m.ry; gz[bi][k] = m.rz;
            fprintf(stderr, "   knob %3d alpha %6.3f regime %d f %.3f rms %.2f %.3f %.2f\n", k * 8, alpha[bi][k], m.regime,
                    m.f, m.rx, m.ry, m.rz);
        }
    }
    printf("/* generated by tools/gen_chua_tables.c: do not edit */\n");
    printf("#define CHUA_NB %d          /* SHAPE rows: beta 10 .. 34 in steps of 4 */\n", NB);
    printf("#define CHUA_NK %d          /* CHAOS columns: knob 0 .. 128 in steps of 8 */\n", NK);
    printf("#define CHUA_RMS_Q 1024   /* the RMS tables: 1.0 = 1024 */\n");
    printf("/* alpha, Q16 */\nstatic const int32_t CHUA_ALPHA[CHUA_NB][CHUA_NK] = {\n");
    for (bi = 0; bi < NB; bi++) {
        printf("    {");
        for (k = 0; k < NK; k++) printf("%ld%s", lround(alpha[bi][k] * 65536), k < NK - 1 ? ", " : "");
        printf("},\n");
    }
    printf("};\n/* 1 / (rotations per unit time), Q16: the time scale for a pitch */\nstatic const int32_t CHUA_PERIOD[CHUA_NB][CHUA_NK] = {\n");
    for (bi = 0; bi < NB; bi++) {
        printf("    {");
        for (k = 0; k < NK; k++) printf("%ld%s", lround(65536 / f[bi][k]), k < NK - 1 ? ", " : "");
        printf("},\n");
    }
    printf("};\n/* RMS of x, y, z (x and z without their mean), CHUA_RMS_Q */\nstatic const uint16_t CHUA_RMS[3][CHUA_NB][CHUA_NK] = {\n");
    for (i = 0; i < 3; i++) {
        printf("    {\n");
        for (bi = 0; bi < NB; bi++) {
            printf("        {");
            for (k = 0; k < NK; k++) {
                double v = i == 0 ? gx[bi][k] : i == 1 ? gy[bi][k] : gz[bi][k];
                printf("%ld%s", lround(fmax(v, 0.02) * 1024), k < NK - 1 ? ", " : "");
            }
            printf("},\n");
        }
        printf("    },\n");
    }
    printf("};\n");
    return 0;
}
