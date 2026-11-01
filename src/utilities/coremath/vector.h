// util::Vec2 / util::Vec3.
//
// Reconstructed from the PDB type stream and the decompiled bodies. The class
// layouts and method sets are exact:
//   Vec2<double> sizeof 16, members X@0 Y@8
//   Vec3<double> sizeof 24, members X@0 Y@8 Z@16
// Method set: GetDimensions (static), 3 constructors, operator[], +,-,*,/,%,
// +=,-=,*=,/=,%=, ==,!=, Length, LengthSquared, ManhattanLength,
// ChebyshevLength, LengthClamped, Normalized, SafeNormalized, LargestDim,
// SmallestDim, Largest, Smallest, IsNormalized, IsZero, IsValid; Vec2 also has
// RightPerp / LeftPerp.
//
// Verified from decompiled out-of-line bodies (see notes/FINDINGS.md F18):
//   Normalized (Vec3 0x0051a100, Vec2 0x0067adf0), SafeNormalized
//   (0x0051af10, 0x0080d0c0), Length (0x00713cf0), LargestDim (0x00519ff0),
//   SmallestDim (0x0051c230).
//
// Methods that exist only inline (operators, trivial accessors) are written in
// the conventional way; where the exact form is not evidenced it is marked
// `inference`.
#pragma once

#include "utilities/coremath/coremath.h"
#include "utilities/core/maybe.h"

namespace util {

template <class Type>
class Vec2
{
public:
	Vec2() : X(0), Y(0) {}
	explicit Vec2(const Type& Value) : X(Value), Y(Value) {}
	Vec2(const Type& InX, const Type& InY) : X(InX), Y(InY) {}

	static int GetDimensions() { return 2; }

	Type& operator[](int Index) { return (&X)[Index]; }
	const Type& operator[](int Index) const { return (&X)[Index]; }

	Vec2 operator+(const Vec2& V) const { return Vec2(X + V.X, Y + V.Y); }
	Vec2 operator-(const Vec2& V) const { return Vec2(X - V.X, Y - V.Y); }
	Vec2 operator*(const Vec2& V) const { return Vec2(X * V.X, Y * V.Y); }
	Vec2 operator*(const Type& Scale) const { return Vec2(X * Scale, Y * Scale); }
	Vec2 operator/(const Vec2& V) const { return Vec2(X / V.X, Y / V.Y); }
	Vec2 operator/(const Type& Scale) const { return Vec2(X / Scale, Y / Scale); }
	Type operator%(const Vec2& V) const { return X * V.Y - Y * V.X; }  // inference: 2D cross

	Vec2& operator+=(const Vec2& V) { X += V.X; Y += V.Y; return *this; }
	Vec2& operator-=(const Vec2& V) { X -= V.X; Y -= V.Y; return *this; }
	Vec2& operator*=(const Vec2& V) { X *= V.X; Y *= V.Y; return *this; }
	Vec2& operator*=(const Type& Scale) { X *= Scale; Y *= Scale; return *this; }
	Vec2& operator/=(const Vec2& V) { X /= V.X; Y /= V.Y; return *this; }
	Vec2& operator/=(const Type& Scale) { X /= Scale; Y /= Scale; return *this; }

	bool operator==(const Vec2& V) const { return X == V.X && Y == V.Y; }
	bool operator!=(const Vec2& V) const { return !(*this == V); }

	Type LengthSquared() const { return X * X + Y * Y; }

	Type Length() const
	{
		const Maybe<Type> Result = Sqrt(LengthSquared());
		ASSERT(Result.IsValid());
		return Result.Get();
	}

	Type ManhattanLength() const { return Abs(X) + Abs(Y); }
	Type ChebyshevLength() const { return Max(Abs(X), Abs(Y)); }

	Vec2 Normalized() const
	{
		const Type Len = Length();
		ASSERT(Len > (Type)0);
		const Type Scale = (Type)1 / Len;
		return Vec2(X * Scale, Y * Scale);
	}

	Vec2 SafeNormalized() const
	{
		const Maybe<Type> Len = Sqrt(LengthSquared());
		ASSERT(Len.IsValid());
		const Type Scale = (Type)1 / Len.Get();
		if ((X != (Type)0 || Y != (Type)0) && Len.Get() != (Type)0)
			return Vec2(X * Scale, Y * Scale);
		return Vec2((Type)0, (Type)0);
	}

	Vec2 RightPerp() const { return Vec2(Y, -X); }   // inference
	Vec2 LeftPerp() const { return Vec2(-Y, X); }    // inference

	int LargestDim() const { return Abs(X) < Abs(Y) ? 1 : 0; }
	int SmallestDim() const { return Abs(Y) < Abs(X) ? 1 : 0; }

	Type Largest() const { return Max(X, Y); }
	Type Smallest() const { return Min(X, Y); }
	bool IsNormalized() const { return Equals(LengthSquared(), (Type)1); }
	bool IsZero() const { return X == (Type)0 && Y == (Type)0; }
	bool IsValid() const { return !IsNan(X) && !IsNan(Y); }

	Type X;
	Type Y;
};

template <class Type>
class Vec3
{
public:
	Vec3() : X(0), Y(0), Z(0) {}
	explicit Vec3(const Type& Value) : X(Value), Y(Value), Z(Value) {}
	Vec3(const Type& InX, const Type& InY, const Type& InZ) : X(InX), Y(InY), Z(InZ) {}

	static int GetDimensions() { return 3; }

	Type& operator[](int Index) { return (&X)[Index]; }
	const Type& operator[](int Index) const { return (&X)[Index]; }

	Vec3 operator+(const Vec3& V) const { return Vec3(X + V.X, Y + V.Y, Z + V.Z); }
	Vec3 operator-(const Vec3& V) const { return Vec3(X - V.X, Y - V.Y, Z - V.Z); }
	Vec3 operator*(const Vec3& V) const { return Vec3(X * V.X, Y * V.Y, Z * V.Z); }
	Vec3 operator*(const Type& Scale) const { return Vec3(X * Scale, Y * Scale, Z * Scale); }
	Vec3 operator/(const Vec3& V) const { return Vec3(X / V.X, Y / V.Y, Z / V.Z); }
	Vec3 operator/(const Type& Scale) const { return Vec3(X / Scale, Y / Scale, Z / Scale); }
	// inference: `%` as the cross product.
	Vec3 operator%(const Vec3& V) const
	{
		return Vec3(Y * V.Z - Z * V.Y, Z * V.X - X * V.Z, X * V.Y - Y * V.X);
	}

	Vec3& operator+=(const Vec3& V) { X += V.X; Y += V.Y; Z += V.Z; return *this; }
	Vec3& operator-=(const Vec3& V) { X -= V.X; Y -= V.Y; Z -= V.Z; return *this; }
	Vec3& operator*=(const Vec3& V) { X *= V.X; Y *= V.Y; Z *= V.Z; return *this; }
	Vec3& operator*=(const Type& Scale) { X *= Scale; Y *= Scale; Z *= Scale; return *this; }
	Vec3& operator/=(const Vec3& V) { X /= V.X; Y /= V.Y; Z /= V.Z; return *this; }
	Vec3& operator/=(const Type& Scale) { X /= Scale; Y /= Scale; Z /= Scale; return *this; }

	bool operator==(const Vec3& V) const { return X == V.X && Y == V.Y && Z == V.Z; }
	bool operator!=(const Vec3& V) const { return !(*this == V); }

	Type LengthSquared() const { return X * X + Y * Y + Z * Z; }

	Type Length() const
	{
		const Maybe<Type> Result = Sqrt(LengthSquared());
		ASSERT(Result.IsValid());
		return Result.Get();
	}

	Type ManhattanLength() const { return Abs(X) + Abs(Y) + Abs(Z); }
	Type ChebyshevLength() const { return Max(Max(Abs(X), Abs(Y)), Abs(Z)); }

	Vec3 Normalized() const
	{
		const Type Len = Length();
		ASSERT(Len > (Type)0);
		const Type Scale = (Type)1 / Len;
		return Vec3(X * Scale, Y * Scale, Z * Scale);
	}

	Vec3 SafeNormalized() const
	{
		const Maybe<Type> Len = Sqrt(LengthSquared());
		ASSERT(Len.IsValid());
		const Type Scale = (Type)1 / Len.Get();
		if ((X == (Type)0 && Y == (Type)0 && Z == (Type)0) || Len.Get() == (Type)0)
			return Vec3((Type)0, (Type)0, (Type)0);
		return Vec3(X * Scale, Y * Scale, Z * Scale);
	}

	int LargestDim() const
	{
		const Type AbsX = Abs(X), AbsY = Abs(Y), AbsZ = Abs(Z);
		if (AbsX < AbsY)
			return (AbsY < AbsZ) ? 2 : 1;
		return (AbsX < AbsZ) ? 2 : 0;
	}

	int SmallestDim() const
	{
		const Type AbsX = Abs(X), AbsY = Abs(Y), AbsZ = Abs(Z);
		if (AbsY < AbsX)
			return (AbsZ < AbsY) ? 2 : 1;
		return (AbsZ < AbsX) ? 2 : 0;
	}

	Type Largest() const { return Max(Max(X, Y), Z); }
	Type Smallest() const { return Min(Min(X, Y), Z); }
	bool IsNormalized() const { return Equals(LengthSquared(), (Type)1); }
	bool IsZero() const { return X == (Type)0 && Y == (Type)0 && Z == (Type)0; }
	bool IsValid() const { return !IsNan(X) && !IsNan(Y) && !IsNan(Z); }

	Type X;
	Type Y;
	Type Z;
};

template <class Type>
class Vec4
{
public:
	Vec4() : X(0), Y(0), Z(0), W(0) {}
	explicit Vec4(const Type& Value) : X(Value), Y(Value), Z(Value), W(Value) {}
	Vec4(const Type& InX, const Type& InY, const Type& InZ, const Type& InW)
	    : X(InX), Y(InY), Z(InZ), W(InW) {}

	static int GetDimensions() { return 4; }

	Type& operator[](int Index) { return (&X)[Index]; }
	const Type& operator[](int Index) const { return (&X)[Index]; }

	Vec4 operator+(const Vec4& V) const { return Vec4(X + V.X, Y + V.Y, Z + V.Z, W + V.W); }
	Vec4 operator-(const Vec4& V) const { return Vec4(X - V.X, Y - V.Y, Z - V.Z, W - V.W); }
	Vec4 operator*(const Type& Scale) const { return Vec4(X * Scale, Y * Scale, Z * Scale, W * Scale); }
	Vec4 operator/(const Type& Scale) const { return Vec4(X / Scale, Y / Scale, Z / Scale, W / Scale); }

	Vec4& operator+=(const Vec4& V) { X += V.X; Y += V.Y; Z += V.Z; W += V.W; return *this; }
	Vec4& operator-=(const Vec4& V) { X -= V.X; Y -= V.Y; Z -= V.Z; W -= V.W; return *this; }
	Vec4& operator*=(const Type& Scale) { X *= Scale; Y *= Scale; Z *= Scale; W *= Scale; return *this; }
	Vec4& operator/=(const Type& Scale) { X /= Scale; Y /= Scale; Z /= Scale; W /= Scale; return *this; }

	bool operator==(const Vec4& V) const
	{
		return X == V.X && Y == V.Y && Z == V.Z && W == V.W;
	}
	bool operator!=(const Vec4& V) const { return !(*this == V); }

	Type LengthSquared() const { return X * X + Y * Y + Z * Z + W * W; }

	Type Length() const
	{
		const Maybe<Type> Result = Sqrt(LengthSquared());
		ASSERT(Result.IsValid());
		return Result.Get();
	}

	Type ManhattanLength() const { return Abs(X) + Abs(Y) + Abs(Z) + Abs(W); }
	Type ChebyshevLength() const { return Max(Max(Abs(X), Abs(Y)), Max(Abs(Z), Abs(W))); }

	Vec4 Normalized() const
	{
		const Type Len = Length();
		ASSERT(Len > (Type)0);
		const Type Scale = (Type)1 / Len;
		return Vec4(X * Scale, Y * Scale, Z * Scale, W * Scale);
	}

	Vec4 SafeNormalized() const
	{
		const Maybe<Type> Len = Sqrt(LengthSquared());
		ASSERT(Len.IsValid());
		const Type Scale = (Type)1 / Len.Get();
		if (Len.Get() == (Type)0)
			return Vec4((Type)0, (Type)0, (Type)0, (Type)0);
		return Vec4(X * Scale, Y * Scale, Z * Scale, W * Scale);
	}

	int LargestDim() const
	{
		const Type A0 = Abs(X), A1 = Abs(Y), A2 = Abs(Z), A3 = Abs(W);
		int Best = 0;
		Type BestAbs = A0;
		if (BestAbs < A1) { Best = 1; BestAbs = A1; }
		if (BestAbs < A2) { Best = 2; BestAbs = A2; }
		if (BestAbs < A3) { Best = 3; BestAbs = A3; }
		return Best;
	}

	int SmallestDim() const
	{
		const Type A0 = Abs(X), A1 = Abs(Y), A2 = Abs(Z), A3 = Abs(W);
		int Best = 0;
		Type BestAbs = A0;
		if (A1 < BestAbs) { Best = 1; BestAbs = A1; }
		if (A2 < BestAbs) { Best = 2; BestAbs = A2; }
		if (A3 < BestAbs) { Best = 3; BestAbs = A3; }
		return Best;
	}

	Type Largest() const { return Max(Max(X, Y), Max(Z, W)); }
	Type Smallest() const { return Min(Min(X, Y), Min(Z, W)); }
	bool IsNormalized() const { return Equals(LengthSquared(), (Type)1); }
	bool IsZero() const { return X == (Type)0 && Y == (Type)0 && Z == (Type)0 && W == (Type)0; }
	bool IsValid() const
	{
		return !IsNan(X) && !IsNan(Y) && !IsNan(Z) && !IsNan(W);
	}

	Type X;
	Type Y;
	Type Z;
	Type W;
};

// util::Equals for Vec3 (VA 0x005018e0): component-wise, short-circuiting.
template <class Type>
inline bool Equals(const Vec3<Type>& A, const Vec3<Type>& B, const Type& Tolerance)
{
	return Equals(A.X, B.X, Tolerance) && Equals(A.Y, B.Y, Tolerance)
	       && Equals(A.Z, B.Z, Tolerance);
}

// util::IsNan for Vec3 (used by the `!IsNan(A_Norm) && !IsNan(B_Norm)` assert
// in advancedgeometry.inl).
template <class Type>
inline bool IsNan(const Vec3<Type>& V)
{
	return IsNan(V.X) || IsNan(V.Y) || IsNan(V.Z);
}

template <class Type>
inline bool Equals(const Vec4<Type>& A, const Vec4<Type>& B, const Type& Tolerance)
{
	return Equals(A.X, B.X, Tolerance) && Equals(A.Y, B.Y, Tolerance)
	       && Equals(A.Z, B.Z, Tolerance) && Equals(A.W, B.W, Tolerance);
}

template <class Type>
inline bool IsNan(const Vec4<Type>& V)
{
	return IsNan(V.X) || IsNan(V.Y) || IsNan(V.Z) || IsNan(V.W);
}

}  // namespace util
