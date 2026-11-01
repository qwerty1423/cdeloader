// cde_recon — entry point for the reconstructed engine executable.
//
// This is the growing product of the reconstruction: as more of the engine is
// recovered under src/, this driver links it into a real i686 PE executable.
// For now it exercises the reconstructed units and reports their results.
//
// Return value: 0 if every self-check passed, 1 otherwise.
#include "utilities/coremath/coremath.h"
#include "utilities/coremath/vector.h"
#include "utilities/core/darray.h"
#include "utilities/math/geometry.h"
#include "utilities/strings/alphabetic.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool Ok, const char* What)
{
	if (!Ok) {
		++g_failures;
		std::printf("FAIL: %s\n", What);
	}
}

}  // namespace

int main()
{
	std::printf("CDE reconstruction engine (partial)\n");

	Check(std::fabs(util::NormalizeAngle(3.0 * util::TwoPi + 0.5) - 0.5) < 1e-12,
	      "NormalizeAngle");
	Check(util::DecimalMod(7.0, 3.0) == 1.0, "DecimalMod");
	Check(util::Pow(2.0, 10) == 1024.0, "Pow");
	Check(util::AlphabeticComparison("abc", "abd") == util::AlphabeticComparator::Less,
	      "AlphabeticComparison");
	Check(util::Vec3<double>(3, 4, 0).Length() == 5.0, "Vec3::Length");
	Check(util::Vec3<double>(3, 4, 0).Normalized().IsNormalized(), "Vec3::Normalized");

	{
		const util::Line3<double> L(util::Vec3<double>(1, 1, 1), util::Vec3<double>(4, 5, 1));
		Check(L.Length() == 5.0, "Line3::Length");
		Check(L.ValueAlong(L.MidPoint()).Get() == 0.5, "Line3::ValueAlong");
	}
	Check(util::Plane<double>(util::Vec3<double>(0, 0, 1), util::Vec3<double>(0, 0, 4))
	          .OriginDistance == 4.0,
	      "Plane::Plane(Normal, Point)");
	Check(util::Range<double>(2.0, 5.0).GetSize() == 3.0, "Range::GetSize");

	Check(util::IsPointWithinTriangle(
	          util::Triangle3<double>(util::Vec3<double>(0, 0, 0), util::Vec3<double>(1, 0, 0),
	                                  util::Vec3<double>(0, 1, 0)),
	          util::Vec3<double>(0.25, 0.25, 0)),
	      "IsPointWithinTriangle");
	Check(util::GetDefaultNearFarPlanes_km(100.0).Upper == 2e11, "GetDefaultNearFarPlanes_km");
	Check(util::PointLineDistance<0, 1>(
	          util::Line3<double>(util::Vec3<double>(0, 0, 0), util::Vec3<double>(10, 0, 0)),
	          util::Vec3<double>(5, 3, 0))
	          == 9.0,
	      "PointLineDistance<0,1>");
	{
		typedef util::AABB<util::Vec3<double> > Box;
		const Box B(util::Vec3<double>(0, 0, 0), util::Vec3<double>(1, 1, 1));
		const util::Maybe<util::Range<double> > Hit = B.Intersect<double>(
		    util::Line3<double>(util::Vec3<double>(-1, 0.5, 0.5), util::Vec3<double>(2, 0.5, 0.5)));
		Check(Hit.IsValid() && std::fabs(Hit.Get().Lower - 1.0 / 3.0) < 1e-9, "AABB::Intersect");
	}
	{
		util::Darray<int> A;
		A.Add(1);
		A.Add(2);
		A.Insert(3, 2);
		A.RemoveAt(0);
		Check(A.Num() == 2 && A[0] == 2 && A[1] == 3, "Darray add/insert/remove");
	}

	if (g_failures == 0) {
		std::printf("all reconstructed self-checks passed\n");
		return 0;
	}
	std::printf("%d self-check(s) failed\n", g_failures);
	return 1;
}
