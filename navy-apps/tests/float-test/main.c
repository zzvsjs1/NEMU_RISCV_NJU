#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <fixedptc.h">

// Allowable error when comparing fixedpt result back to float
#define TOL 0.005f

// Helper: check two floats nearly equal
static int nearly(float a, float b) {
    return fabsf(a - b) <= TOL;
}

static void test_conversion() {
    float vals[] = { 1.0f, 1.2f, -1.0f, -1.2f, 0.0f, 123.456f, -123.456f };
    const int N = sizeof(vals) / sizeof(vals[0]);
    int fails = 0;

    printf("=== Conversion & round‐trip tests ===\n");
    for (int i = 0; i < N; i++) {
        float  f = vals[i];
        fixedpt fp1 = fixedpt_rconst(f);
        float  back = fixedpt_tofloat(fp1);

        // also test fromfloat(void*)
        float tmp = f;
        fixedpt fp2 = fixedpt_fromfloat(&tmp);
        float back2 = fixedpt_tofloat(fp2);

        if (!nearly(back, f)) {
            printf("[FAIL] rconst:  orig=%.6f, back=%.6f\n", f, back);
            fails++;
        }
        if (!nearly(back2, f)) {
            printf("[FAIL] fromfloat: orig=%.6f, back=%.6f\n", f, back2);
            fails++;
        }
    }

    printf("Conversion tests %s\n\n", fails ? "had failures" : "all passed");
}

static void test_floor_ceil() {
    float vals[] = {  1.0f, 1.2f, -1.0f, -1.2f,  0.0f,  123.456f, -123.456f };
    const int N = sizeof(vals) / sizeof(vals[0]);
    int fails = 0;

    printf("=== floor/ceil tests ===\n");
    for (int i = 0; i < N; i++) {
        float      f = vals[i];
        fixedpt    fp = fixedpt_rconst(f);
        float   f_floor = floorf(f);
        float  our_floor = fixedpt_tofloat(fixedpt_floor(fp));
        float    f_ceil  = ceilf(f);
        float   our_ceil = fixedpt_tofloat(fixedpt_ceil(fp));

        if (!nearly(f_floor, our_floor)) {
            printf("[FAIL] floor: orig=%.6f, floorf=%.6f, ours=%.6f\n",
                   f, f_floor, our_floor);
            fails++;
        }
        if (!nearly(f_ceil, our_ceil)) {
            printf("[FAIL] ceil : orig=%.6f, ceilf =%.6f, ours =%.6f\n",
                   f, f_ceil, our_ceil);
            fails++;
        }
    }

    printf("floor/ceil tests %s\n\n", fails ? "had failures" : "all passed");
}

static void test_arithmetic() {
    float  A[] = { 1.5f, -3.2f,  0.0f,   5.75f, -2.5f };
    float  B[] = { 2.5f,  1.6f, -4.0f,  -0.5f,  3.2f };
    const int N = sizeof(A) / sizeof(A[0]);
    int fails = 0;

    printf("=== fixedpt_mul/div tests ===\n");
    for (int i = 0; i < N; i++) {
        float a = A[i], b = B[i];
        fixedpt fa = fixedpt_rconst(a), fb = fixedpt_rconst(b);

        // mul
        fixedpt  fmul = fixedpt_mul(fa, fb);
        float   fmul_f = fixedpt_tofloat(fmul);
        float   exp_mul = a * b;

        if (!nearly(fmul_f, exp_mul)) {
            printf("[FAIL] mul: %.4f * %.4f = %.4f (exp=%.4f)\n",
                   a, b, fmul_f, exp_mul);
            fails++;
        }

        // div (skip div by zero)
        if (b != 0.0f) {
            fixedpt fdiv = fixedpt_div(fa, fb);
            float   fdiv_f = fixedpt_tofloat(fdiv);
            float   exp_div = a / b;
            if (!nearly(fdiv_f, exp_div)) {
                printf("[FAIL] div: %.4f / %.4f = %.4f (exp=%.4f)\n",
                       a, b, fdiv_f, exp_div);
                fails++;
            }
        }
    }

    printf("fixedpt_mul/div tests %s\n\n", fails ? "had failures" : "all passed");

    printf("=== muli/divi/abs tests ===\n");
    {
        int ifails = 0;
        float vals[] = {  3.2f, -3.2f, 100.0f };
        const int M = sizeof(vals)/sizeof(vals[0]);
        int ivals[] = {  5,     -5,     10 };

        for (int i = 0; i < M; i++) {
            float f = vals[i];
            int   I = ivals[i];
            fixedpt fp = fixedpt_rconst(f);

            // muli
            fixedpt fm = fixedpt_muli(fp, I);
            float  fm_f = fixedpt_tofloat(fm);
            if (!nearly(fm_f, f * I)) {
                printf("[FAIL] muli: %.4f * %d = %.4f (exp=%.4f)\n",
                       f, I, fm_f, f*I);
                ifails++;
            }

            // divi (avoid div by zero)
            if (I != 0) {
                fixedpt fd = fixedpt_divi(fp, I);
                float  fd_f = fixedpt_tofloat(fd);
                if (!nearly(fd_f, f / I)) {
                    printf("[FAIL] divi: %.4f / %d = %.4f (exp=%.4f)\n",
                           f, I, fd_f, f/I);
                    ifails++;
                }
            }

            // abs
            fixedpt ab = fixedpt_abs(fp);
            float  ab_f = fixedpt_tofloat(ab);
            if (!nearly(ab_f, fabsf(f))) {
                printf("[FAIL] abs: |%.4f| = %.4f (exp=%.4f)\n",
                       f, ab_f, fabsf(f));
                ifails++;
            }
        }
        printf("muli/divi/abs tests %s\n\n", ifails ? "had failures" : "all passed");
        fails += ifails;
    }
}

int main(void) {
    printf("Running fixedptc self‐tests (tolerance ±%.3f)...\n\n", TOL);
    test_conversion();
    test_floor_ceil();
    test_arithmetic();
    printf("All tests complete.\n");
    return 0;
}
