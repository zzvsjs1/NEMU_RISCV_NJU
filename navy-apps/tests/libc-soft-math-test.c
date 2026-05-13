#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * This test is intentionally independent from the host libm.  It links against
 * Navy's math.c directly, then checks fixed reference values and identities
 * that ONScripter depends on.  Compile it with -fno-builtin so the compiler
 * cannot silently replace calls such as sin() with host-library assumptions.
 */

double fabs(double);
double sin(double);
double cos(double);
double tan(double);
double sqrt(double);
double pow(double, double);
double log(double);
double round(double);
double floor(double);
double ceil(double);

static int failures;

static uint64_t bits_of_double(double x)
{
  union {
    double f;
    uint64_t u;
  } v;
  v.f = x;
  return v.u;
}

static double local_abs(double x) {
  return x < 0.0 ? -x : x;
}

static int local_is_nan(double x) {
  uint64_t bits = bits_of_double(x);
  return (bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000) &&
         (bits & UINT64_C(0x000fffffffffffff)) != 0;
}

static void expect_near(const char *name, double actual, double expected, double tolerance)
{
  double diff = local_abs(actual - expected);
  if (!(diff <= tolerance)) {
    printf("[FAIL] %s: expected %.17g, got %.17g, diff %.17g, tolerance %.17g\n",
           name, expected, actual, diff, tolerance);
    failures++;
  }
}

static void expect_exact_bits(const char *name, double actual, uint64_t expected_bits)
{
  uint64_t actual_bits = bits_of_double(actual);
  if (actual_bits != expected_bits) {
    printf("[FAIL] %s: expected bits 0x%016llx, got 0x%016llx\n",
           name, (unsigned long long)expected_bits, (unsigned long long)actual_bits);
    failures++;
  }
}

static void expect_nan(const char *name, double actual)
{
  if (!local_is_nan(actual)) {
    printf("[FAIL] %s: expected NaN, got %.17g with bits 0x%016llx\n",
           name, actual, (unsigned long long)bits_of_double(actual));
    failures++;
  }
}

static void expect_scaled_int(const char *name, double actual, int expected, int tolerance)
{
  int rounded_towards_zero = (int)actual;
  int diff = rounded_towards_zero - expected;
  if (diff < 0)
    diff = -diff;
  if (diff > tolerance) {
    printf("[FAIL] %s: expected scaled integer %d +/- %d, got %d from %.17g\n",
           name, expected, tolerance, rounded_towards_zero, actual);
    failures++;
  }
}

static void test_trig_reference_values(void)
{
  const double pi = 3.14159265358979323846264338327950288;

  expect_near("sin(0)", sin(0.0), 0.0, 1e-15);
  expect_near("sin(pi / 6)", sin(pi / 6.0), 0.5, 1e-13);
  expect_near("sin(pi / 4)", sin(pi / 4.0), 0.70710678118654757, 1e-13);
  expect_near("sin(pi / 2)", sin(pi / 2.0), 1.0, 1e-13);
  expect_near("sin(pi)", sin(pi), 0.0, 1e-13);

  expect_near("cos(0)", cos(0.0), 1.0, 1e-15);
  expect_near("cos(pi / 3)", cos(pi / 3.0), 0.5, 1e-13);
  expect_near("cos(pi / 2)", cos(pi / 2.0), 0.0, 1e-13);
  expect_near("cos(pi)", cos(pi), -1.0, 1e-13);

  expect_near("tan(0)", tan(0.0), 0.0, 1e-15);
  expect_near("tan(pi / 6)", tan(pi / 6.0), 0.57735026918962573, 1e-12);
  expect_near("tan(pi / 4)", tan(pi / 4.0), 1.0, 1e-12);

  expect_near("sin(1000000)", sin(1000000.0), -0.34999350217129294, 1e-12);
  expect_near("cos(1000000)", cos(1000000.0), 0.93675212753314474, 1e-12);
  expect_near("tan(1000000)", tan(1000000.0), -0.37362445398759900, 1e-12);
}

static void test_onscripter_scaled_trig(void)
{
  const double pi = 3.14159265358979323846264338327950288;

  /*
   * ONScripter's script commands store sin/cos/tan as integers scaled by
   * 1000.  The real libm result can land just below an exact decimal value
   * such as 0.5, so these checks allow one count of truncation difference.
   */
  expect_scaled_int("script sin 30", sin(pi * 30.0 / 180.0) * 1000.0, 500, 1);
  expect_scaled_int("script sin 45", sin(pi * 45.0 / 180.0) * 1000.0, 707, 1);
  expect_scaled_int("script sin 90", sin(pi * 90.0 / 180.0) * 1000.0, 1000, 1);
  expect_scaled_int("script sin -30", sin(pi * -30.0 / 180.0) * 1000.0, -500, 1);

  expect_scaled_int("script cos 0", cos(0.0) * 1000.0, 1000, 0);
  expect_scaled_int("script cos 60", cos(pi * 60.0 / 180.0) * 1000.0, 500, 1);
  expect_scaled_int("script cos 90", cos(pi * 90.0 / 180.0) * 1000.0, 0, 1);
  expect_scaled_int("script cos 180", cos(pi) * 1000.0, -1000, 1);

  expect_scaled_int("script tan 0", tan(0.0) * 1000.0, 0, 0);
  expect_scaled_int("script tan 30", tan(pi * 30.0 / 180.0) * 1000.0, 577, 1);
  expect_scaled_int("script tan 45", tan(pi * 45.0 / 180.0) * 1000.0, 1000, 1);
  expect_scaled_int("script tan -45", tan(pi * -45.0 / 180.0) * 1000.0, -1000, 1);
}

static void test_sqrt_floor_ceil_round_fabs(void)
{
  expect_near("sqrt(0)", sqrt(0.0), 0.0, 0.0);
  expect_exact_bits("sqrt(-0)", sqrt(-0.0), UINT64_C(0x8000000000000000));
  expect_near("sqrt(0.25)", sqrt(0.25), 0.5, 0.0);
  expect_near("sqrt(2)", sqrt(2.0), 1.4142135623730951, 1e-15);
  expect_near("sqrt(12345.6789)", sqrt(12345.6789), 111.11111060555555, 1e-12);
  expect_nan("sqrt(-1)", sqrt(-1.0));

  expect_exact_bits("floor(-2.7)", floor(-2.7), UINT64_C(0xc008000000000000));
  expect_exact_bits("floor(-0.5)", floor(-0.5), UINT64_C(0xbff0000000000000));
  expect_exact_bits("floor(0.5)", floor(0.5), UINT64_C(0x0000000000000000));
  expect_exact_bits("floor(2.7)", floor(2.7), UINT64_C(0x4000000000000000));

  expect_exact_bits("ceil(-2.7)", ceil(-2.7), UINT64_C(0xc000000000000000));
  expect_exact_bits("ceil(-0.5)", ceil(-0.5), UINT64_C(0x8000000000000000));
  expect_exact_bits("ceil(0.5)", ceil(0.5), UINT64_C(0x3ff0000000000000));
  expect_exact_bits("ceil(2.7)", ceil(2.7), UINT64_C(0x4008000000000000));

  expect_exact_bits("round(-0.5)", round(-0.5), UINT64_C(0xbff0000000000000));
  expect_exact_bits("round(0.5)", round(0.5), UINT64_C(0x3ff0000000000000));
  expect_exact_bits("round(2.5)", round(2.5), UINT64_C(0x4008000000000000));
  expect_exact_bits("fabs(-3.25)", fabs(-3.25), UINT64_C(0x400a000000000000));
}

static void test_log_pow_smoke(void)
{
  expect_near("log(1)", log(1.0), 0.0, 1e-15);
  expect_near("log(e)", log(2.71828182845904523536), 1.0, 1e-13);
  expect_nan("log(-1)", log(-1.0));

  expect_near("pow(2, 10)", pow(2.0, 10.0), 1024.0, 0.0);
  expect_near("pow(9, 0.5)", pow(9.0, 0.5), 3.0, 1e-12);
  expect_near("pow(-2, 3)", pow(-2.0, 3.0), -8.0, 0.0);
  expect_nan("pow(-2, 0.5)", pow(-2.0, 0.5));
}

int main(void)
{
  test_trig_reference_values();
  test_onscripter_scaled_trig();
  test_sqrt_floor_ceil_round_fabs();
  test_log_pow_smoke();

  if (failures != 0) {
    printf("libc soft math test failed: %d failure(s)\n", failures);
    return 1;
  }

  printf("libc soft math test passed\n");
  return 0;
}
