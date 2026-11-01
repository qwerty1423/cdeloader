// Contract tests for the reconstructed util geometry free functions and the
// tuple2/tuple3 container types.
#include "utilities/core/tuple.h"
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
	using util::Circle;
	using util::Line3;
	using util::Plane;
	using util::Triangle3;
	using util::Vec2;
	using util::Vec3;

	typedef util::tuple3<double, double, double> TripleD;
	typedef util::tuple2<Vec2<double>, Vec2<double> > PairVec2;

	// ---- tuple2 / tuple3 --------------------------------------------------
	EXPECT(sizeof(TripleD) == 24);
	EXPECT(offsetof(TripleD, Value1) == 0);
	EXPECT(offsetof(TripleD, Value2) == 8);
	EXPECT(offsetof(TripleD, Value3) == 16);
	EXPECT(sizeof(PairVec2) == 32);
	{
		const TripleD T(1, 2, 3);
		EXPECT(T.Value1 == 1 && T.Value2 == 2 && T.Value3 == 3);
		EXPECT(T == TripleD(1, 2, 3));
		EXPECT(T != TripleD(1, 2, 4));
	}

	// ---- GetDefaultNearFarPlanes_km --------------------------------------
	{
		util::Range<double> R = util::GetDefaultNearFarPlanes_km(100.0);
		EXPECT(Close(R.Lower, 1.0) && Close(R.Upper, 2e11));
		R = util::GetDefaultNearFarPlanes_km(0.0);
		EXPECT(Close(R.Lower, 1e-5) && Close(R.Upper, 2e11));
		// Near plane above the far plane: values swap to stay ordered.
		R = util::GetDefaultNearFarPlanes_km(1e15);
		EXPECT(Close(R.Lower, 2e11) && Close(R.Upper, 1e13));
	}

	// ---- LinePlaneIntersect ----------------------------------------------
	{
		const Plane<double> P(Vec3<double>(0, 0, 1), 4.0);
		util::Maybe<double> T = util::LinePlaneIntersect<0>(
		    P, Line3<double>(Vec3<double>(0, 0, 0), Vec3<double>(0, 0, 8)));
		EXPECT(T.IsValid() && Close(T.Get(), 0.5));

		// Parallel line: no intersection.
		EXPECT(!util::LinePlaneIntersect<0>(
		           P, Line3<double>(Vec3<double>(0, 0, 5), Vec3<double>(0, 0, 7)))
		           .IsValid());
		// Crossing beyond the segment endpoint: rejected.
		EXPECT(!util::LinePlaneIntersect<0>(
		           P, Line3<double>(Vec3<double>(0, 0, 0), Vec3<double>(0, 0, 2)))
		           .IsValid());
	}

	// ---- PointLineDistance ----------------------------------------------
	{
		const Line3<double> L(Vec3<double>(0, 0, 0), Vec3<double>(10, 0, 0));
		// Segment distance, squared (ClampMode 0, ResultMode 1).
		EXPECT(Close(util::PointLineDistance<0, 1>(L, Vec3<double>(5, 3, 0)), 9.0));
		EXPECT(Close(util::PointLineDistance<0, 1>(L, Vec3<double>(20, 3, 0)), 109.0));
		EXPECT(Close(util::PointLineDistance<0, 1>(L, Vec3<double>(-5, 0, 0)), 25.0));
		// Infinite-line distance (ClampMode 2, ResultMode 0).
		EXPECT(Close(util::PointLineDistance<2, 0>(L, Vec3<double>(20, 3, 0)), 3.0));
		EXPECT(Close(util::PointLineDistance<2, 0>(L, Vec3<double>(5, 3, 0)), 3.0));
	}

	// ---- IsPointWithinTriangle / GetPointBarycenterInTriangle ------------
	{
		const Triangle3<double> T(Vec3<double>(0, 0, 0), Vec3<double>(1, 0, 0),
		                          Vec3<double>(0, 1, 0));
		EXPECT(util::IsPointWithinTriangle(T, Vec3<double>(0.25, 0.25, 0)));
		EXPECT(!util::IsPointWithinTriangle(T, Vec3<double>(0.6, 0.6, 0)));
		EXPECT(!util::IsPointWithinTriangle(T, Vec3<double>(1, 1, 0)));
		// A point on an edge (weight exactly zero) still counts as within.
		EXPECT(util::IsPointWithinTriangle(T, Vec3<double>(0.5, 0.5, 0)));

		const TripleD B =
		    util::GetPointBarycenterInTriangle(T, Vec3<double>(0.25, 0.25, 0));
		EXPECT(Close(B.Value1 + B.Value2 + B.Value3, 1.0));
		EXPECT(B.Value1 >= 0 && B.Value2 >= 0 && B.Value3 >= 0);

		// Degenerate triangle: rejected.
		const Triangle3<double> D(Vec3<double>(0, 0, 0), Vec3<double>(1, 0, 0),
		                          Vec3<double>(2, 0, 0));
		EXPECT(!util::IsPointWithinTriangle(D, Vec3<double>(1, 0, 0)));
	}

	// ---- TangentPointsToCircle -------------------------------------------
	{
		const Circle<double> C(Vec2<double>(0, 0), 1.0);
		const PairVec2 Tan = util::TangentPointsToCircle(Vec2<double>(2, 0), C);

		const double Sin60 = std::sqrt(3.0) / 2.0;
		EXPECT(Close(Tan.Value1.X, 0.5));
		EXPECT(Close(Tan.Value1.Y, -Sin60));
		EXPECT(Close(Tan.Value2.X, 0.5));
		EXPECT(Close(Tan.Value2.Y, Sin60));

		// Each tangent point is on the circle and the radius is perpendicular
		// to the line from the external point.
		for (int i = 0; i < 2; i++)
		{
			const Vec2<double> T = (i == 0) ? Tan.Value1 : Tan.Value2;
			const Vec2<double> P(2, 0);
			EXPECT(Close(T.Length(), 1.0));
			const double Dot = T.X * (T.X - P.X) + T.Y * (T.Y - P.Y);
			EXPECT(Close(Dot, 0.0, 1e-12));
		}
	}

	if (g_failures == 0)
		std::printf("ALL PASS (geometry functions)\n");
	else
		std::printf("%d failures (geometry functions)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
