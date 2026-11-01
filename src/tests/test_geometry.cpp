// Contract tests for the reconstructed util geometry value types.
//
// Layout expectations come straight from the PDB type stream (see
// src/utilities/math/geometry.h). Behavioural expectations come from the
// decompiled out-of-line bodies recorded in notes/FINDINGS.md.
#include "utilities/math/geometry.h"

#include <cmath>
#include <cstdio>
#include <cstddef>

static int g_failures = 0;

#define EXPECT(cond)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                     \
			g_failures++;                                                  \
			std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
		}                                                                  \
	} while (0)

int main()
{
	using util::Circle;
	using util::Cylinder;
	using util::Line3;
	using util::Plane;
	using util::Range;
	using util::Triangle3;
	using util::Vec2;
	using util::Vec3;

	// ---- layouts --------------------------------------------------------
	EXPECT(sizeof(Range<double>) == 16);
	EXPECT(offsetof(Range<double>, Lower) == 0);
	EXPECT(offsetof(Range<double>, Upper) == 8);

	EXPECT(sizeof(Circle<double>) == 24);
	EXPECT(offsetof(Circle<double>, Center) == 0);
	EXPECT(offsetof(Circle<double>, Radius) == 16);

	EXPECT(sizeof(Cylinder<double>) == 16);
	EXPECT(offsetof(Cylinder<double>, Radius) == 0);
	EXPECT(offsetof(Cylinder<double>, Height) == 8);

	EXPECT(sizeof(Line3<double>) == 48);
	EXPECT(offsetof(Line3<double>, A) == 0);
	EXPECT(offsetof(Line3<double>, B) == 24);

	EXPECT(sizeof(Plane<double>) == 32);
	EXPECT(offsetof(Plane<double>, Normal) == 0);
	EXPECT(offsetof(Plane<double>, OriginDistance) == 24);

	EXPECT(sizeof(Triangle3<double>) == 72);

	// ---- Range ----------------------------------------------------------
	{
		const Range<double> R(2.0, 5.0);
		EXPECT(R.GetSize() == 3.0);
		EXPECT(R.GetMidpoint() == 3.5);
		EXPECT(R.IsValid());
		EXPECT(R.Between(3.0));
		EXPECT(!R.Between(6.0));
		EXPECT(R.ClampTo(9.0) == 5.0);
		EXPECT(R.ClampTo(-1.0) == 2.0);
		EXPECT((Range<double>(1.0, 3.0).Overlaps(Range<double>(2.5, 4.0))));
		EXPECT(!(Range<double>(1.0, 3.0).Overlaps(Range<double>(3.5, 4.0))));
		const Range<double> I = Range<double>(1.0, 3.0).Intersect(Range<double>(2.0, 8.0));
		EXPECT(I.Lower == 2.0 && I.Upper == 3.0);
	}

	// ---- Circle / Cylinder ---------------------------------------------
	{
		const Circle<double> C(Vec2<double>(1, 2), 3.5);
		EXPECT(C.Center.X == 1.0 && C.Center.Y == 2.0 && C.Radius == 3.5);
		const Cylinder<double> Y(2.0, 7.0);
		EXPECT(Y.Radius == 2.0 && Y.Height == 7.0);
	}

	// ---- Line3 ----------------------------------------------------------
	{
		const Line3<double> L(Vec3<double>(1, 1, 1), Vec3<double>(4, 5, 1));
		EXPECT(L.AsVector() == Vec3<double>(3, 4, 0));
		EXPECT(L.Length() == 5.0);
		EXPECT(L.MidPoint() == Vec3<double>(2.5, 3, 1));
		EXPECT(L.PointAlong(0.0) == L.A);
		EXPECT(L.PointAlong(1.0) == L.B);

		// ValueAlong uses the largest component of the direction (here X: 3).
		// Point.P = A + t*(B - A) => t = (P.X - A.X) / 3.
		util::Maybe<double> t = L.ValueAlong(Vec3<double>(2.5, 3, 1));
		EXPECT(t.IsValid() && std::fabs(t.Get() - 0.5) < 1e-12);

		// Degenerate line: the dominant component is zero -> invalid Maybe.
		const Line3<double> D(Vec3<double>(0, 0, 0), Vec3<double>(0, 5, 0));
		t = D.ValueAlong(Vec3<double>(0, 1, 0));
		EXPECT(t.IsValid() && std::fabs(t.Get() - 0.2) < 1e-12);
		const Line3<double> Z(Vec3<double>(0, 0, 0), Vec3<double>(0, 0, 0));
		EXPECT(!Z.ValueAlong(Vec3<double>(1, 1, 1)).IsValid());
	}

	// ---- Plane ----------------------------------------------------------
	{
		// Plane(Normal, Point): OriginDistance = (Point . Normal) * |Normal|.
		// The normal must be unit length (asserted by the ctor).
		const Plane<double> P(Vec3<double>(0, 0, 1), Vec3<double>(0, 0, 4));
		EXPECT(P.Normal == Vec3<double>(0, 0, 1));
		EXPECT(P.OriginDistance == 4.0);
		EXPECT(P.IsValid());

		// Plane(Normal, Distance) stores the distance directly.
		const Plane<double> Q(Vec3<double>(0, 1, 0), 2.5);
		EXPECT(Q.OriginDistance == 2.5);
	}

	// ---- Triangle3 ------------------------------------------------------
	{
		const Triangle3<double> T(Vec3<double>(0, 0, 0), Vec3<double>(1, 0, 0),
		                          Vec3<double>(0, 1, 0));
		EXPECT(T.IsValid());
		Triangle3<double> Bad = T;
		Bad.B.X = std::nan("");
		EXPECT(!Bad.IsValid());
	}

	if (g_failures == 0)
		std::printf("ALL PASS (geometry)\n");
	else
		std::printf("%d failures (geometry)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
