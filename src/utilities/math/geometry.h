// util geometry value types: Range, Circle, Cylinder, Line3, Plane, Triangle3.
//
// Layouts from the PDB type stream:
//   Range<double>      sizeof 16  Lower@0 Upper@8
//   Circle<double>     sizeof 24  Center(Vec2)@0 Radius@16
//   Cylinder<double>   sizeof 16  Radius@0 Height@8
//   Line3<double>      sizeof 48  A(Vec3)@0 B(Vec3)@24
//   Plane<double>      sizeof 32  Normal(Vec3)@0 OriginDistance@24
//   Triangle3<double>  three Vec3@0/24/48
//
// Decompiled out-of-line bodies: Line3::ValueAlong (0x0051c700), Plane's two
// value constructors (0x00506110 / 0x0070caa0), Triangle3::IsValid
// (0x00519b20). Methods that only exist inline carry an `inference` note where
// the exact form is not evidenced.
#pragma once

#include "utilities/coremath/vector.h"
#include "utilities/core/maybe.h"
#include "utilities/core/tuple.h"

namespace util {

template <class Type>
class Range
{
public:
	Range() : Lower(0), Upper(0) {}
	explicit Range(const Type& Value) : Lower(Value), Upper(Value) {}
	Range(const Type& InLower, const Type& InUpper) : Lower(InLower), Upper(InUpper) {}

	Type GetSize() const { return Upper - Lower; }
	Type GetMidpoint() const { return (Lower + Upper) / (Type)2; }
	bool IsValid() const { return Lower <= Upper; }                        // inference
	bool Between(const Type& Value) const { return Lower <= Value && Value <= Upper; }  // inference
	Type ClampTo(const Type& Value) const { return Clamp(Value, Lower, Upper); }        // inference
	bool Overlaps(const Range& R) const { return Lower <= R.Upper && R.Lower <= Upper; }  // inference
	Range Intersect(const Range& R) const                                  // inference
	{
		return Range(Max(Lower, R.Lower), Min(Upper, R.Upper));
	}
	Range Join(const Range& R) const                                       // inference
	{
		return Range(Min(Lower, R.Lower), Max(Upper, R.Upper));
	}

	Type Lower;
	Type Upper;
};

template <class Type>
class Circle
{
public:
	Circle() : Center(), Radius(0) {}
	Circle(const Vec2<Type>& InCenter, const Type& InRadius) : Center(InCenter), Radius(InRadius) {}
	Circle(const Type& X, const Type& Y, const Type& InRadius)
	    : Center(X, Y), Radius(InRadius) {}

	Vec2<Type> Center;
	Type Radius;
};

template <class Type>
class Cylinder
{
public:
	Cylinder() : Radius(0), Height(0) {}
	explicit Cylinder(const Type& InRadius) : Radius(InRadius), Height(0) {}
	Cylinder(const Type& InRadius, const Type& InHeight) : Radius(InRadius), Height(InHeight) {}

	Type Radius;
	Type Height;
};

template <class Type>
class Line3
{
public:
	Line3() : A(), B() {}
	Line3(const Vec3<Type>& InA, const Vec3<Type>& InB) : A(InA), B(InB) {}

	Vec3<Type> AsVector() const { return B - A; }
	Vec3<Type> Direction() const { return AsVector().Normalized(); }       // inference
	Type Length() const { return AsVector().Length(); }
	Vec3<Type> MidPoint() const { return (A + B) * (Type)0.5; }            // inference
	Vec3<Type> PointAlong(const Type& T) const { return A + AsVector() * T; }  // inference

	// util::Line3<double>::ValueAlong (VA 0x0051c700): solve for the parameter
	// along the line using the component where the direction is largest; if
	// that component is zero the result is an invalid Maybe.
	Maybe<Type> ValueAlong(const Vec3<Type>& Point) const
	{
		const Vec3<Type> Dir = AsVector();
		const int Dim = Dir.LargestDim();
		if (Dir[Dim] == (Type)0)
			return Maybe<Type>();
		return Maybe<Type>((Point[Dim] - A[Dim]) / Dir[Dim]);
	}

	bool operator==(const Line3& L) const { return A == L.A && B == L.B; }
	bool operator!=(const Line3& L) const { return !(*this == L); }
	Line3 operator+(const Vec3<Type>& V) const { return Line3(A + V, B + V); }  // inference
	Line3 operator-(const Vec3<Type>& V) const { return Line3(A - V, B - V); }  // inference

	Vec3<Type> A;
	Vec3<Type> B;
};

template <class Type>
class Plane
{
public:
	Plane() : Normal(0, 0, 1), OriginDistance(0) {}
	Plane(const Vec3<Type>& InNormal, const Type& InOriginDistance)
	    : Normal(InNormal), OriginDistance(InOriginDistance)
	{
		ASSERT(IsValid());
	}

	// util::Plane<double>::Plane(Vec3 Normal, Vec3 Point) (VA 0x00506110):
	// OriginDistance = (Point . Normal) * |Normal| (the constructor asserts the
	// normal is already unit length).
	Plane(const Vec3<Type>& InNormal, const Vec3<Type>& Point)
	    : Normal(InNormal)
	{
		const Type Dot = Point.X * Normal.X + Point.Y * Normal.Y + Point.Z * Normal.Z;
		OriginDistance = Dot * Normal.Length();
		ASSERT(IsValid());
	}

	bool IsValid() const
	{
		return Normal.IsNormalized() && Normal.IsValid() && !IsNan(OriginDistance);
	}

	// The point in the plane closest to the origin.
	Vec3<Type> GetPointInPlane() const { return Normal * OriginDistance; }  // inference

	Vec3<Type> Normal;
	Type OriginDistance;
};

template <class Type>
class Triangle3
{
public:
	Triangle3() : A(), B(), C() {}
	Triangle3(const Vec3<Type>& InA, const Vec3<Type>& InB, const Vec3<Type>& InC)
	    : A(InA), B(InB), C(InC) {}

	// util::Triangle3<double>::IsValid (VA 0x00519b20): no NaN in any component.
	bool IsValid() const { return A.IsValid() && B.IsValid() && C.IsValid(); }

	Vec3<Type> A;
	Vec3<Type> B;
	Vec3<Type> C;
};

// util::AABB<VectorType> — axis-aligned bounding box. sizeof 48 for
// Vec3<double> (Lower@0, Upper@24).
//
// Recovered bodies:
//   0x007bd140  Intersects(AABB const&)
//   0x005258e0  Intersect<Type,Line3Type>(Line3 const&) -> Maybe<Range<Type>>
//   0x00527be0  AABB(Darray<Vec3> const&) — not yet reconstructed (needs Darray)
// The default / (Lower,Upper) / (Value) constructors and GetSize/GetCenter/
// Contains/IsZero are only inline in the original and are written
// conventionally (`inference`); GetScaled is unrecovered.
template <class VectorType>
class AABB
{
public:
	AABB() : Lower(), Upper() {}
	AABB(const VectorType& InLower, const VectorType& InUpper) : Lower(InLower), Upper(InUpper) {}
	explicit AABB(const VectorType& Value) : Lower(Value), Upper(Value) {}

	VectorType GetSize() const { return Upper - Lower; }
	VectorType GetCenter() const { return (Lower + Upper) / 2; }

	bool Intersects(const AABB& Other) const
	{
		for (int i = 0; i < VectorType::GetDimensions(); i++)
			if (Other.Upper[i] < Lower[i] || Upper[i] < Other.Lower[i])
				return false;
		return true;
	}

	bool Contains(const VectorType& Point) const
	{
		for (int i = 0; i < VectorType::GetDimensions(); i++)
			if (Point[i] < Lower[i] || Upper[i] < Point[i])
				return false;
		return true;
	}

	bool IsZero() const { return Lower.IsZero() && Upper.IsZero(); }

	// 0x005258e0 — slab clip of the segment against the box; the Maybe is
	// invalid when the segment misses, otherwise its value is the parameter
	// interval [enter, exit] over which the segment lies inside the box.
	template <class Type, class LineType>
	Maybe<Range<Type>> Intersect(const LineType& LineIn) const
	{
		const Type Start[3] = {LineIn.A.X, LineIn.A.Y, LineIn.A.Z};
		const Type Dir[3] = {LineIn.B.X - LineIn.A.X, LineIn.B.Y - LineIn.A.Y,
		                     LineIn.B.Z - LineIn.A.Z};
		Type Enter = (Type)0;
		Type Exit = (Type)1;

		for (int Axis = 0; Axis < 3; Axis++)
		{
			if (Equals(Dir[Axis], (Type)0, (Type)Epsilon))
			{
				if (Start[Axis] < Lower[Axis] || Upper[Axis] < Start[Axis])
					return Maybe<Range<Type>>();
				continue;
			}

			const Type Inv = (Type)1 / Dir[Axis];
			Type SlabMin = (Lower[Axis] - Start[Axis]) * Inv;
			Type SlabMax = (Upper[Axis] - Start[Axis]) * Inv;
			if (SlabMax < SlabMin)
			{
				const Type Swap = SlabMin;
				SlabMin = SlabMax;
				SlabMax = Swap;
			}
			if (Exit < SlabMin || SlabMax < Enter)
				return Maybe<Range<Type>>();
			Enter = Max(Enter, SlabMin);
			Exit = Min(Exit, SlabMax);
		}
		return Maybe<Range<Type>>(Range<Type>(Enter, Exit));
	}

	VectorType Lower;
	VectorType Upper;
};

}  // namespace util

#include "geometry.inl"
