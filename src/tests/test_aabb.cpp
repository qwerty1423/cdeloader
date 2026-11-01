// Contract tests for the reconstructed util::AABB.
#include "utilities/math/geometry.h"

#include <cmath>
#include <cstdio>
#include <cstddef>

static int g_failures = 0;

#define EXPECT(cond)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                    \
			g_failures++;                                                 \
			std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
		}                                                                 \
	} while (0)

static bool Close(double A, double B, double Tol = 1e-12)
{
	return std::fabs(A - B) < Tol;
}

int main()
{
	using util::AABB;
	using util::Line3;
	using util::Maybe;
	using util::Range;
	using util::Vec3;

	typedef AABB<Vec3<double> > Box;

	EXPECT(sizeof(Box) == 48);
	EXPECT(offsetof(Box, Lower) == 0);
	EXPECT(offsetof(Box, Upper) == 24);

	// ---- constructors / accessors ---------------------------------------
	{
		const Box Zero;
		EXPECT(Zero.IsZero());
		EXPECT(Zero.Lower == Vec3<double>(0, 0, 0) && Zero.Upper == Vec3<double>(0, 0, 0));

		const Box Box1(Vec3<double>(0, 0, 0), Vec3<double>(2, 4, 6));
		EXPECT(Box1.GetSize() == Vec3<double>(2, 4, 6));
		EXPECT(Box1.GetCenter() == Vec3<double>(1, 2, 3));
		EXPECT(!Box1.IsZero());

		const Box Box2(Vec3<double>(1, 1, 1));
		EXPECT(Box2.Lower == Box2.Upper);
	}

	// ---- Contains / Intersects ------------------------------------------
	{
		const Box B(Vec3<double>(0, 0, 0), Vec3<double>(1, 1, 1));
		EXPECT(B.Contains(Vec3<double>(0.5, 0.5, 0.5)));
		EXPECT(B.Contains(Vec3<double>(0, 0, 0)));
		EXPECT(!B.Contains(Vec3<double>(1.5, 0.5, 0.5)));

		EXPECT(B.Intersects(Box(Vec3<double>(0.5, 0.5, 0.5), Vec3<double>(2, 2, 2))));
		EXPECT(B.Intersects(Box(Vec3<double>(1, 1, 1), Vec3<double>(2, 2, 2))));
		EXPECT(!B.Intersects(Box(Vec3<double>(1.5, 0, 0), Vec3<double>(2, 1, 1))));
	}

	// ---- Intersect(Line3): slab clip ------------------------------------
	{
		const Box B(Vec3<double>(0, 0, 0), Vec3<double>(1, 1, 1));

		Maybe<Range<double> > Hit = B.Intersect<double>(
		    Line3<double>(Vec3<double>(-1, 0.5, 0.5), Vec3<double>(2, 0.5, 0.5)));
		EXPECT(Hit.IsValid());
		EXPECT(Close(Hit.Get().Lower, 1.0 / 3.0, 1e-9));
		EXPECT(Close(Hit.Get().Upper, 2.0 / 3.0, 1e-9));

		// Segment entering the box: exit is the segment end.
		Hit = B.Intersect<double>(
		    Line3<double>(Vec3<double>(-1, 0.5, 0.5), Vec3<double>(0.5, 0.5, 0.5)));
		EXPECT(Hit.IsValid());
		EXPECT(Close(Hit.Get().Lower, 2.0 / 3.0, 1e-9));
		EXPECT(Close(Hit.Get().Upper, 1.0, 1e-9));

		// Parallel to a slab and outside it: rejected.
		EXPECT(!B.Intersect<double>(
		            Line3<double>(Vec3<double>(-1, 5, 0.5), Vec3<double>(2, 5, 0.5)))
		            .IsValid());

		// Entirely in front of the box: rejected.
		EXPECT(!B.Intersect<double>(
		            Line3<double>(Vec3<double>(2, 0.5, 0.5), Vec3<double>(3, 0.5, 0.5)))
		            .IsValid());
	}

	if (g_failures == 0)
		std::printf("ALL PASS (aabb)\n");
	else
		std::printf("%d failures (aabb)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
