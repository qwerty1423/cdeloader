// util::Quaternion<Type>.
//
// Layout from the PDB type stream: struct size 32, members X@0 Y@8 Z@16 W@24.
// Method set: 8 constructors, operator* (2), operator/, operator+, operator==,
// !=, Conjugate, Inverse, Length, LengthSquared, Normalized, IsNormalized,
// IsValid, GetAxis, GetAngle_radians, ToMatrix, To4x4Matrix, ToAxisAngle,
// ToPseudovector, AngleClamped, GetFromLookAt (static), SetFromMatrix (private).
//
// Reconstructed here: the algebra (Conjugate / Inverse / Length / Normalized /
// operators), the angle accessors, AngleClamped (decompiled from VA
// 0x0052a520, which reuses util::SafeACos and the reconstructed Clamp), and
// To4x4Matrix (VA 0x00846860).
// ToMatrix / SetFromMatrix / ToAxisAngle / GetFromLookAt are not reconstructed
// yet (Matrix3x3 now exists, so ToMatrix is easy follow-up work).
//
// Constructors: the default and (X,Y,Z,W) are given; the vector/triad
// constructors are not reconstructed yet. Default is the identity quaternion.
#pragma once

#include "utilities/coremath/vector.h"
#include "utilities/coremath/matrix.h"

namespace util {

template <class Type>
class Quaternion
{
public:
	Quaternion() : X(0), Y(0), Z(0), W(1) {}
	Quaternion(const Type& InX, const Type& InY, const Type& InZ, const Type& InW)
	    : X(InX), Y(InY), Z(InZ), W(InW) {}

	bool operator==(const Quaternion& Q) const
	{
		return X == Q.X && Y == Q.Y && Z == Q.Z && W == Q.W;
	}
	bool operator!=(const Quaternion& Q) const { return !(*this == Q); }

	Quaternion operator+(const Quaternion& Q) const
	{
		return Quaternion(X + Q.X, Y + Q.Y, Z + Q.Z, W + Q.W);
	}

	// Hamilton product (inference: standard convention for this codebase).
	Quaternion operator*(const Quaternion& Q) const
	{
		return Quaternion(W * Q.X + X * Q.W + Y * Q.Z - Z * Q.Y,
		                  W * Q.Y - X * Q.Z + Y * Q.W + Z * Q.X,
		                  W * Q.Z + X * Q.Y - Y * Q.X + Z * Q.W,
		                  W * Q.W - X * Q.X - Y * Q.Y - Z * Q.Z);
	}

	Quaternion operator/(const Type& Scale) const
	{
		return Quaternion(X / Scale, Y / Scale, Z / Scale, W / Scale);
	}

	Quaternion Conjugate() const { return Quaternion(-X, -Y, -Z, W); }
	Quaternion Inverse() const { return Conjugate() / LengthSquared(); }

	Type LengthSquared() const { return X * X + Y * Y + Z * Z + W * W; }

	Type Length() const
	{
		const Maybe<Type> Result = Sqrt(LengthSquared());
		ASSERT(Result.IsValid());
		return Result.Get();
	}

	Quaternion Normalized() const { return *this / Length(); }

	bool IsNormalized() const { return Equals(LengthSquared(), (Type)1); }
	bool IsValid() const
	{
		return !IsNan(X) && !IsNan(Y) && !IsNan(Z) && !IsNan(W);
	}

	Vec3<Type> GetAxis() const { return Vec3<Type>(X, Y, Z); }
	Type GetAngle_radians() const { return (Type)2 * SafeACos(W); }
	Vec3<Type> ToPseudovector() const { return GetAxis() * GetAngle_radians(); }

	// util::Quaternion<double>::To4x4Matrix (VA 0x00846860). The flat data is
	// written exactly as the binary does (the classic column-major GL rotation
	// matrix for this quaternion); the term order mirrors the decompilation.
	Matrix4x4<Type> To4x4Matrix() const
	{
		const Type TwoX = X + X;
		const Type TwoY = Y + Y;
		const Type TwoZ = Z + Z;
		const Type TwoXX = X * X + X * X;
		const Type TwoYY = Y * Y + Y * Y;
		const Type TwoZZ = Z * Z + Z * Z;
		const Type OneMinusTwoXX = (Type)1 - TwoXX;

		Matrix4x4<Type> Result;
		Result.Data[0] = ((Type)1 - TwoYY) - TwoZZ;
		Result.Data[1] = (TwoZ * W) + (TwoX * Y);
		Result.Data[2] = (TwoX * Z) - (TwoY * W);
		Result.Data[4] = (TwoX * Y) - (TwoZ * W);
		Result.Data[5] = OneMinusTwoXX - TwoZZ;
		Result.Data[6] = (TwoX * W) + (TwoY * Z);
		Result.Data[8] = (TwoX * Z) + (TwoY * W);
		Result.Data[9] = (TwoY * Z) - (TwoX * W);
		Result.Data[10] = OneMinusTwoXX - TwoYY;
		return Result;
	}

	// ToMatrix: the upper-left 3x3 of To4x4Matrix (inference; consistent with
	// util::Matrix3x3's constructor from util::Matrix4x4).
	Matrix3x3<Type> ToMatrix() const
	{
		const Matrix4x4<Type> R = To4x4Matrix();
		Matrix3x3<Type> Result;
		Result.Data[0] = R.Data[0];
		Result.Data[1] = R.Data[1];
		Result.Data[2] = R.Data[2];
		Result.Data[3] = R.Data[4];
		Result.Data[4] = R.Data[5];
		Result.Data[5] = R.Data[6];
		Result.Data[6] = R.Data[8];
		Result.Data[7] = R.Data[9];
		Result.Data[8] = R.Data[10];
		return Result;
	}

	// util::Quaternion<double>::AngleClamped (VA 0x0052a520): re-derive the
	// axis from (X,Y,Z), then rebuild the rotation with the angle reduced to at
	// most MaxAngle_radians.
	Quaternion AngleClamped(const Type& MaxAngle_radians) const
	{
		const Type ClampedW = Clamp(W, (Type)0, (Type)1);
		const Type Angle = (Type)2 * SafeACos(ClampedW);

		Type SinHalf = (Type)std::sqrt(Clamp((Type)1 - W * W, (Type)0, (Type)1));
		Type AxisX = X;
		Type AxisY = Y;
		Type AxisZ = Z;
		if ((Type)1e-8 < SinHalf)
		{
			SinHalf = (Type)1 / SinHalf;
			AxisX *= SinHalf;
			AxisY *= SinHalf;
			AxisZ *= SinHalf;
		}

		const Type NewHalf = Min(Angle, MaxAngle_radians) * (Type)0.5;
		const Type SinNew = (Type)std::sin(NewHalf);
		return Quaternion(SinNew * AxisX, SinNew * AxisY, SinNew * AxisZ,
		                  (Type)std::cos(NewHalf));
	}

	Type X;
	Type Y;
	Type Z;
	Type W;
};

// util::Rotate<double>(Quaternion, Vec3) (VA 0x00504e90): rotate a vector by a
// quaternion. Written to match the decompiled arithmetic exactly.
template <class Type>
inline Vec3<Type> Rotate(const Quaternion<Type>& Q, const Vec3<Type>& V)
{
	const Type A = V.Z * Q.Y - V.Y * Q.Z + V.X * Q.W;
	const Type B = -V.Z * Q.X + V.X * Q.Z + V.Y * Q.W;
	const Type C = V.Y * Q.X - V.X * Q.Y + V.Z * Q.W;
	const Type D = -(V.X * Q.X + V.Y * Q.Y + V.Z * Q.Z);

	return Vec3<Type>(A * Q.W - B * Q.Z + C * Q.Y - D * Q.X,
	                  Q.W * B + Q.Z * A - Q.X * C - Q.Y * D,
	                  -Q.Y * A + Q.X * B + Q.W * C - Q.Z * D);
}

}  // namespace util
