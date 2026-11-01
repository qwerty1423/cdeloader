// Contract tests for the reconstructed util::Vec2 / util::Vec3.
#include "utilities/coremath/vector.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

static void expect(bool ok, const char* what)
{
	if (!ok) {
		g_failures++;
		std::printf("FAIL: %s\n", what);
	}
}

int main()
{
	using util::Vec2;
	using util::Vec3;

	// Dimensions.
	expect(Vec2<double>::GetDimensions() == 2, "Vec2 dims");
	expect(Vec3<double>::GetDimensions() == 3, "Vec3 dims");

	// Length / LengthSquared.
	expect(Vec3<double>(3, 4, 0).Length() == 5.0, "Vec3 length 3,4,0");
	expect(Vec3<double>(1, 2, 3).LengthSquared() == 14.0, "Vec3 length^2");
	expect(Vec2<double>(3, 4).Length() == 5.0, "Vec2 length 3,4");
	expect(Vec2<double>(3, 4).LengthSquared() == 25.0, "Vec2 length^2");

	// Manhattan / Chebyshev / Largest / Smallest.
	expect(Vec3<double>(1, -2, 3).ManhattanLength() == 6.0, "Vec3 manhattan");
	expect(Vec3<double>(1, -5, 3).ChebyshevLength() == 5.0, "Vec3 chebyshev");
	expect(Vec3<double>(1, 5, 3).Largest() == 5.0, "Vec3 largest");
	expect(Vec3<double>(1, 5, 3).Smallest() == 1.0, "Vec3 smallest");

	// Normalized.
	{
		const Vec3<double> N = Vec3<double>(3, 4, 0).Normalized();
		expect(std::fabs(N.X - 0.6) < 1e-15 && std::fabs(N.Y - 0.8) < 1e-15
		           && N.Z == 0.0,
		       "Vec3 normalized 3,4,0");
		expect(Vec3<double>(3, 4, 0).Normalized().IsNormalized(), "Vec3 isNormalized");
	}
	{
		const Vec2<double> N = Vec2<double>(3, 4).Normalized();
		expect(std::fabs(N.X - 0.6) < 1e-15 && std::fabs(N.Y - 0.8) < 1e-15,
		       "Vec2 normalized 3,4");
	}

	// SafeNormalized of the zero vector is the zero vector.
	expect(Vec3<double>(0, 0, 0).SafeNormalized().IsZero(), "Vec3 safezero");
	expect(Vec2<double>(0, 0).SafeNormalized().IsZero(), "Vec2 safezero");
	expect(!Vec3<double>(1, 2, 3).SafeNormalized().IsZero(), "Vec3 safenonzero");

	// LargestDim / SmallestDim (first index wins ties).
	expect(Vec3<double>(1, 5, 3).LargestDim() == 1, "Vec3 largestdim Y");
	expect(Vec3<double>(5, 1, 3).LargestDim() == 0, "Vec3 largestdim X");
	expect(Vec3<double>(1, 3, 5).LargestDim() == 2, "Vec3 largestdim Z");
	expect(Vec3<double>(2, 2, 1).LargestDim() == 0, "Vec3 largestdim tie -> X");
	expect(Vec3<double>(5, 1, 3).SmallestDim() == 1, "Vec3 smallestdim Y");
	expect(Vec3<double>(1, 5, 3).SmallestDim() == 0, "Vec3 smallestdim X");
	expect(Vec3<double>(5, 3, 1).SmallestDim() == 2, "Vec3 smallestdim Z");
	expect(Vec2<double>(1, 5).LargestDim() == 1, "Vec2 largestdim Y");
	expect(Vec2<double>(5, 1).LargestDim() == 0, "Vec2 largestdim X");
	expect(Vec2<double>(5, 1).SmallestDim() == 1, "Vec2 smallestdim Y");
	expect(Vec2<double>(1, 5).SmallestDim() == 0, "Vec2 smallestdim X");

	// Arithmetic.
	{
		const Vec3<double> A(1, 2, 3), B(4, 5, 6);
		expect((A + B) == Vec3<double>(5, 7, 9), "Vec3 +");
		expect((B - A) == Vec3<double>(3, 3, 3), "Vec3 -");
		expect((A * 2.0) == Vec3<double>(2, 4, 6), "Vec3 * scalar");
		expect((B / 2.0) == Vec3<double>(2, 2.5, 3), "Vec3 / scalar");
		expect((A * B) == Vec3<double>(4, 10, 18), "Vec3 * component");
		expect((A % B) == Vec3<double>(2 * 6 - 3 * 5, 3 * 4 - 1 * 6, 1 * 5 - 2 * 4),
		       "Vec3 % cross");

		Vec3<double> C = A;
		C += B;
		expect(C == Vec3<double>(5, 7, 9), "Vec3 +=");
		C -= B;
		expect(C == A, "Vec3 -=");
	}

	// IsValid.
	expect(Vec3<double>(1, 2, 3).IsValid(), "Vec3 valid");
	expect(!Vec3<double>(std::nan(""), 2, 3).IsValid(), "Vec3 nan invalid");

	// Equals<Vec3> (component-wise, short-circuiting).
	expect(util::Equals(Vec3<double>(1, 2, 3), Vec3<double>(1, 2, 3), 1e-8),
	       "Equals vec3 equal");
	expect(util::Equals(Vec3<double>(1, 2, 3), Vec3<double>(1, 2, 3 + 1e-12), 1e-8),
	       "Equals vec3 near");
	expect(!util::Equals(Vec3<double>(1, 2, 3), Vec3<double>(1, 2, 4), 1e-8),
	       "Equals vec3 differ");

	if (g_failures == 0)
		std::printf("ALL PASS (vector)\n");
	else
		std::printf("%d failures (vector)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
