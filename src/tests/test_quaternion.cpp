// Contract tests for the reconstructed util::Quaternion core + AngleClamped.
#include "utilities/coremath/quaternion.h"

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

static bool approxEq(const util::Quaternion<double>& A, const util::Quaternion<double>& B)
{
	return std::fabs(A.X - B.X) < 1e-15 && std::fabs(A.Y - B.Y) < 1e-15
	       && std::fabs(A.Z - B.Z) < 1e-15 && std::fabs(A.W - B.W) < 1e-15;
}

int main()
{
	using util::Quaternion;
	using util::Vec3;

	const double HalfSqrt2 = std::sqrt(2.0) / 2.0;

	// Default is the identity.
	expect(Quaternion<double>() == Quaternion<double>(0, 0, 0, 1), "identity default");

	// A 90-degree rotation about Z: W = cos(45deg), Z = sin(45deg).
	const Quaternion<double> RotZ90(0, 0, HalfSqrt2, HalfSqrt2);
	expect(std::fabs(RotZ90.GetAngle_radians() - (0.5 * 3.14159265358979323846)) < 1e-12,
	       "GetAngle_radians 90deg");
	expect(RotZ90.IsNormalized(), "IsNormalized");
	expect(std::fabs(RotZ90.Length() - 1.0) < 1e-15, "Length");

	// Axis is +Z.
	expect(RotZ90.GetAxis() == Vec3<double>(0, 0, HalfSqrt2), "GetAxis");

	// AngleClamped with a budget above the current angle is a no-op.
	expect(approxEq(RotZ90.AngleClamped(3.14159265358979323846), RotZ90), "AngleClamped no-op");

	// AngleClamped to a smaller angle rebuilds the rotation.
	{
		const double Pi = 3.14159265358979323846;
		const Quaternion<double> Clamped = RotZ90.AngleClamped(Pi / 4.0);
		const Quaternion<double> Want(0, 0, std::sin(Pi / 8.0), std::cos(Pi / 8.0));
		expect(std::fabs(Clamped.X - Want.X) < 1e-15
		           && std::fabs(Clamped.Y - Want.Y) < 1e-15
		           && std::fabs(Clamped.Z - Want.Z) < 1e-15
		           && std::fabs(Clamped.W - Want.W) < 1e-15,
		       "AngleClamped reduce");
	}

	// Conjugate / Inverse.
	expect(RotZ90.Conjugate() == Quaternion<double>(0, 0, -HalfSqrt2, HalfSqrt2),
	       "Conjugate");
	expect(approxEq(RotZ90 * RotZ90.Conjugate(), Quaternion<double>(0, 0, 0, 1)),
	       "q * q^-1 = identity");
	expect(approxEq(RotZ90.Inverse(), RotZ90.Conjugate()), "Inverse of unit");

	// Composition: two 90-degree Z rotations make a 180-degree Z rotation.
	{
		const Quaternion<double> Q = RotZ90 * RotZ90;
		expect(std::fabs(Q.Z - 1.0) < 1e-15 && std::fabs(Q.W) < 1e-15, "compose two 90deg");
	}

	// To4x4Matrix: identity quaternion -> identity matrix (exact).
	{
		const util::Matrix4x4<double> M = Quaternion<double>().To4x4Matrix();
		expect(M.Data[0] == 1.0 && M.Data[5] == 1.0 && M.Data[10] == 1.0 && M.Data[15] == 1.0,
		       "To4x4 identity diagonal");
		expect(M.Data[1] == 0.0 && M.Data[4] == 0.0 && M.Data[12] == 0.0, "To4x4 identity off");
	}

	// To4x4Matrix of a 90-degree Z rotation, in the binary's flat layout.
	{
		const util::Matrix4x4<double> M = RotZ90.To4x4Matrix();
		expect(std::fabs(M.Data[0]) < 1e-15, "To4x4 rot [0]");
		expect(std::fabs(M.Data[5]) < 1e-15, "To4x4 rot [5]");
		expect(std::fabs(M.Data[10] - 1.0) < 1e-15, "To4x4 rot [10]");
		expect(std::fabs(M.Data[1] - 1.0) < 1e-15, "To4x4 rot [1]");
		expect(std::fabs(M.Data[4] + 1.0) < 1e-15, "To4x4 rot [4]");

		// GL convention (column-major, column vector): M * (1,0,0,0) = (0,1,0,0).
		const double vx = M.Data[0] * 1.0;
		const double vy = M.Data[1] * 1.0;
		const double vz = M.Data[2] * 1.0;
		expect(std::fabs(vx) < 1e-15 && std::fabs(vy - 1.0) < 1e-15
		           && std::fabs(vz) < 1e-15,
		       "To4x4 rotates +X to +Y");
	}

	// util::Rotate: q * (1,0,0) = (0,1,0) for the 90-degree Z rotation.
	{
		const Vec3<double> R = util::Rotate(RotZ90, Vec3<double>(1, 0, 0));
		expect(std::fabs(R.X) < 1e-15 && std::fabs(R.Y - 1.0) < 1e-15
		           && std::fabs(R.Z) < 1e-15,
		       "Rotate +X to +Y");
		const Vec3<double> R2 = util::Rotate(RotZ90, Vec3<double>(0, 1, 0));
		expect(std::fabs(R2.X + 1.0) < 1e-15 && std::fabs(R2.Y) < 1e-15,
		       "Rotate +Y to -X");
		// Identity leaves vectors unchanged.
		expect(util::Rotate(Quaternion<double>(), Vec3<double>(3, 4, 5))
		       == Vec3<double>(3, 4, 5), "Rotate identity");
	}

	// ToMatrix upper-left 3x3 agrees with To4x4Matrix's flat layout.
	{
		const util::Matrix3x3<double> M = RotZ90.ToMatrix();
		const util::Matrix4x4<double> R = RotZ90.To4x4Matrix();
		expect(M.Data[0] == R.Data[0] && M.Data[4] == R.Data[5] && M.Data[8] == R.Data[10],
		       "ToMatrix slicing");
	}

	if (g_failures == 0)
		std::printf("ALL PASS (quaternion)\n");	else
		std::printf("%d failures (quaternion)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
