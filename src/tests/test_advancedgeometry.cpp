// Contract tests for the reconstructed util::CalcSafePerpendicular.
#include "utilities/math/advancedgeometry.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

static void expectVec(const util::Vec3<double>& Got, const util::Vec3<double>& Want,
                      const char* what)
{
	if (Got != Want) {
		g_failures++;
		std::printf("FAIL: %s (got (%g,%g,%g) want (%g,%g,%g))\n", what, Got.X, Got.Y,
		            Got.Z, Want.X, Want.Y, Want.Z);
	}
}

int main()
{
	using util::CalcSafePerpendicular;
	using util::Vec3;

	// Non-parallel vectors: plain cross product.
	expectVec(CalcSafePerpendicular(Vec3<double>(1, 0, 0), Vec3<double>(0, 1, 0)),
	          Vec3<double>(0, 0, 1), "cross x,y");

	// Parallel vectors: substitute an axis from A's smallest component.
	expectVec(CalcSafePerpendicular(Vec3<double>(1, 0, 0), Vec3<double>(1, 0, 0)),
	          Vec3<double>(0, 0, 1), "parallel x,x");
	expectVec(CalcSafePerpendicular(Vec3<double>(0, 0, 1), Vec3<double>(0, 0, 1)),
	          Vec3<double>(0, 1, 0), "parallel z,z");

	// Antiparallel vectors behave like parallel.
	expectVec(CalcSafePerpendicular(Vec3<double>(1, 0, 0), Vec3<double>(-1, 0, 0)),
	          Vec3<double>(0, 0, 1), "antiparallel x,-x");

	// B zero: substitute an axis from A's smallest component.
	expectVec(CalcSafePerpendicular(Vec3<double>(1, 0, 0), Vec3<double>(0, 0, 0)),
	          Vec3<double>(0, 0, 1), "B zero");

	// A zero: substitute an axis from B's smallest component.
	expectVec(CalcSafePerpendicular(Vec3<double>(0, 0, 0), Vec3<double>(1, 0, 0)),
	          Vec3<double>(0, 0, -1), "A zero");

	// Both zero.
	expectVec(CalcSafePerpendicular(Vec3<double>(0, 0, 0), Vec3<double>(0, 0, 0)),
	          Vec3<double>(1, 0, 0), "both zero");

	// The result is perpendicular to its inputs in the non-degenerate case.
	{
		const Vec3<double> A = Vec3<double>(1, 2, 3).Normalized();
		const Vec3<double> B = Vec3<double>(-4, 5, 6).Normalized();
		const Vec3<double> P = CalcSafePerpendicular(A, B);
		expectVec(P, A % B, "perpendicular equals cross");
		if (std::fabs(P.X * A.X + P.Y * A.Y + P.Z * A.Z) > 1e-12
		    || std::fabs(P.X * B.X + P.Y * B.Y + P.Z * B.Z) > 1e-12) {
			g_failures++;
			std::printf("FAIL: result not perpendicular\n");
		}
	}

	if (g_failures == 0)
		std::printf("ALL PASS (advancedgeometry)\n");
	else
		std::printf("%d failures (advancedgeometry)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
