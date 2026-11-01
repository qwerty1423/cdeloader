// Contract tests for the reconstructed util::NormalizeAngle.
//
// These verify the function's contract (result in [0, TwoPi), congruent to the
// input modulo TwoPi) and its recovered edge-case behavior. They do not execute
// the original binary; they check the reconstruction against the decompiled
// semantics (notes/FINDINGS.md).
#include "utilities/coremath/coremath.h"

#include <cmath>
#include <cstdio>
#include <random>

static constexpr double kPi = 3.14159265358979323846;

static int g_failures = 0;

static void expect(bool ok, const char* what, double in, double got)
{
	if (!ok) {
		g_failures++;
		std::printf("FAIL: %s  (in=%.17g got=%.17g)\n", what, in, got);
	}
}

static bool congruent(double in, double r)
{
	// (in - r) must be an integer multiple of TwoPi.
	double k = (in - r) / util::TwoPi;
	return std::fabs(k - std::llround(k)) < 1e-6 * (1.0 + std::fabs(k));
}

int main()
{
	using util::NormalizeAngle;
	using util::TwoPi;

	// The assert path must not fire for valid input; NaN is detected separately.
	if (!util::IsNan(std::nan(""))) {
		std::printf("FAIL: IsNan(NaN) false\n");
		g_failures++;
	}
	if (util::IsNan(1.0)) {
		std::printf("FAIL: IsNan(1.0) true\n");
		g_failures++;
	}

	// Exact / known values.
	expect(NormalizeAngle(0.0) == 0.0, "0 -> 0", 0.0, NormalizeAngle(0.0));
	expect(NormalizeAngle(1.0) == 1.0, "1 -> 1", 1.0, NormalizeAngle(1.0));
	expect(NormalizeAngle(TwoPi) == 0.0, "2pi -> 0", TwoPi, NormalizeAngle(TwoPi));
	expect(std::fabs(NormalizeAngle(-0.5) - (TwoPi - 0.5)) < 1e-12,
	       "-0.5 -> 2pi-0.5", -0.5, NormalizeAngle(-0.5));
	expect(std::fabs(NormalizeAngle(-TwoPi)) < 1e-12,
	       "-2pi -> 0", -TwoPi, NormalizeAngle(-TwoPi));
	expect(std::fabs(NormalizeAngle(3.0 * kPi) - kPi) < 1e-9,
	       "3pi -> pi", 3.0 * kPi, NormalizeAngle(3.0 * kPi));

	// coremath helpers (values are self-consistent; names are inference).
	expect(util::Clamp(0.5, 0.0, 1.0) == 0.5, "Clamp mid", 0.5, util::Clamp(0.5, 0.0, 1.0));
	expect(util::Clamp(-2.0, -1.0, 1.0) == -1.0, "Clamp lo", -2.0, util::Clamp(-2.0, -1.0, 1.0));
	expect(util::Clamp(2.0, -1.0, 1.0) == 1.0, "Clamp hi", 2.0, util::Clamp(2.0, -1.0, 1.0));
	expect(util::Between(0.5, -1.0, 1.0), "Between in", 0.5, 0.0);
	expect(!util::Between(1.5, -1.0, 1.0), "Between out", 1.5, 0.0);
	expect(util::Equals(1.0, 1.0 + 1e-10), "Equals near", 1.0, 0.0);
	expect(!util::Equals(1.0, 1.1), "Equals far", 1.0, 0.0);

	// DecimalMod: result follows the sign of the divisor, unlike fmod.
	expect(util::DecimalMod<double>(7.0, 3.0) == 1.0, "mod 7,3", 7.0, util::DecimalMod<double>(7.0, 3.0));
	expect(util::DecimalMod<double>(-7.0, 3.0) == 2.0, "mod -7,3", -7.0, util::DecimalMod<double>(-7.0, 3.0));
	expect(util::DecimalMod<double>(7.0, -3.0) == -2.0, "mod 7,-3", 7.0, util::DecimalMod<double>(7.0, -3.0));
	expect(util::DecimalMod<double>(-7.0, -3.0) == -1.0, "mod -7,-3", -7.0, util::DecimalMod<double>(-7.0, -3.0));
	expect(util::DecimalMod<double>(6.0, 3.0) == 0.0, "mod 6,3", 6.0, util::DecimalMod<double>(6.0, 3.0));
	expect(util::DecimalMod<double>(5.0, 0.0) == 5.0, "mod y=0", 5.0, util::DecimalMod<double>(5.0, 0.0));
	expect(util::DecimalMod<double>(1.5, 1.0) == 0.5, "mod 1.5,1", 1.5, util::DecimalMod<double>(1.5, 1.0));

	// Pow.
	expect(util::Pow<double>(2.0, 10) == 1024.0, "Pow 2^10", 2.0, util::Pow<double>(2.0, 10));
	expect(util::Pow<double>(2.0, 0) == 1.0, "Pow 2^0", 2.0, util::Pow<double>(2.0, 0));
	expect(util::Pow<double>(2.0, 1) == 2.0, "Pow 2^1", 2.0, util::Pow<double>(2.0, 1));
	expect(util::Pow<double>(2.0, -1) == 0.5, "Pow 2^-1", 2.0, util::Pow<double>(2.0, -1));
	expect(std::fabs(util::Pow<double>(3.0, -2) - 1.0 / 9.0) < 1e-15, "Pow 3^-2", 3.0,
	       util::Pow<double>(3.0, -2));
	expect(util::Pow<double>(0.5, 3) == 0.125, "Pow 0.5^3", 0.5, util::Pow<double>(0.5, 3));

	// SafeACos / SafeASin must equal acos/asin over the whole valid domain.
	// A small tolerance: on i686 the x87 path and a different libm than the
	// original MSVC build make bit-exact equality unreliable across calls.
	for (int i = 0; i <= 1000; ++i) {
		double x = -1.0 + 2.0 * i / 1000.0;
		expect(std::fabs(util::SafeACos<double>(x) - std::acos(x)) < 1e-12,
		       "SafeACos", x, util::SafeACos<double>(x));
		expect(std::fabs(util::SafeASin<double>(x) - std::asin(x)) < 1e-12,
		       "SafeASin", x, util::SafeASin<double>(x));
	}

	// Contract over a spread of inputs, including a random sample.
	std::mt19937_64 rng(0xC0FFEE);
	std::uniform_real_distribution<double> dist(-1.0e6, 1.0e6);

	int n = 0;
	auto checkOne = [&](double in) {
		double r = NormalizeAngle(in);
		expect(r >= 0.0 && r < TwoPi, "range [0,2pi)", in, r);
		expect(congruent(in, r), "congruent mod 2pi", in, r);
		n++;
	};

	const double seeds[] = {
	    0.0, 1e-12, -1e-12, kPi, -kPi, TwoPi, -TwoPi, 3.0 * kPi,
	    -3.0 * kPi, 100.0 * kPi, -100.0 * kPi, 12345.678, -99999.123,
	    1.0e-300, -1.0e-300, 1.0e7, -1.0e7};
	for (double s : seeds)
		checkOne(s);
	for (int i = 0; i < 200000; ++i)
		checkOne(dist(rng));

	std::printf("checked %d inputs, %d failures\n", n + 6, g_failures);
	if (g_failures == 0)
		std::printf("ALL PASS\n");
	return g_failures == 0 ? 0 : 1;
}
