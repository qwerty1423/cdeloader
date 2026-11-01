// geometry.inl — reconstruction of the CDE "utilities" geometry free functions
// (original path utilities/utilities/math/geometry.inl).
//
// Recovered from the shipped binary (VA → function):
//   0x004923c0  GetDefaultNearFarPlanes_km(double)
//   0x00503060  IsPointWithinTriangle<double>(Triangle3, Vec3)
//   0x00502bc0  GetPointBarycenterInTriangle<double>(Triangle3, Vec3)
//   0x00503890  LinePlaneIntersect<0, double>(Plane, Line3)
//   0x00504d80  PointLineDistance<0, 1, double>(Line3, Vec3)
//   0x0059e7b0  PointLineDistance<2, 0, double>(Line3, Vec3)
//   0x004e6fb0  TangentPointsToCircle<double>(Vec2, Circle)
//
// The `Maybe`/`tuple2`/`tuple3` return conventions and the assert locations
// (core/maybe.h:19) come straight from the decompiled bodies. Where a template
// integer parameter's meaning is only pinned by the one or two shipped
// instantiations, the mapping is marked `inference`.
namespace util {

// 0x004923c0. Scales the supplied distance by 0.01, floors it at 1e-5 km, and
// pairs it with the 2e11 km far plane; if the near plane would exceed the far
// plane the two are swapped so the result stays ordered.
// (Original parameter name not evidenced — `Distance_km` is ours.)
inline Range<double> GetDefaultNearFarPlanes_km(double Distance_km)
{
	const double Near = Distance_km * 0.01;
	double Lower = Near;
	if (Lower <= 1e-5)
		Lower = 1e-5;

	Range<double> Result(Lower, 2e11);
	if (2e11 < Lower)
	{
		Result.Lower = 2e11;
		Result.Upper = Lower;
	}
	return Result;
}

// 0x00503890. Parameter t at which the line segment crosses the plane, or an
// invalid Maybe when the line is parallel to the plane or the crossing falls
// outside [0,1]. `Mode` is the shipped template tag (only <0, double> exists);
// it is not used by the body. `N` is assumed unit length (the Plane invariant).
template <int Mode, class Type>
inline Maybe<Type> LinePlaneIntersect(const Plane<Type>& PlaneIn, const Line3<Type>& LineIn)
{
	const Vec3<Type>& N = PlaneIn.Normal;
	const Type Denom = (LineIn.B.X - LineIn.A.X) * N.X + (LineIn.B.Y - LineIn.A.Y) * N.Y
	                   + (LineIn.B.Z - LineIn.A.Z) * N.Z;
	if (Denom == (Type)0)
		return Maybe<Type>();

	const Type Numer = PlaneIn.OriginDistance * (N.X * N.X + N.Y * N.Y + N.Z * N.Z)
	                   - (LineIn.A.X * N.X + LineIn.A.Y * N.Y + LineIn.A.Z * N.Z);
	const Type T = Numer / Denom;
	if (T < (Type)0 || (Type)1 < T)
		return Maybe<Type>();
	return Maybe<Type>(T);
}

// 0x00504d80 / 0x0059e7b0. Squared/linear distance from `Point` to `LineIn`.
// The template tags select the shipped behaviour (inference from the two
// instantiations):
//   ClampMode 0 -> clamp the foot of the perpendicular to the segment [A,B];
//             2 -> use the infinite line through A and B (no clamp).
//   ResultMode 0 -> return the (rooted) distance; 1 -> return the squared
//              distance.
template <int ClampMode, int ResultMode, class Type>
inline Type PointLineDistance(const Line3<Type>& LineIn, const Vec3<Type>& Point)
{
	const Vec3<Type> Dir = LineIn.B - LineIn.A;
	const Type DirLenSq = Dir.X * Dir.X + Dir.Y * Dir.Y + Dir.Z * Dir.Z;

	Vec3<Type> Closest;
	if (DirLenSq == (Type)0)
	{
		Closest = LineIn.A;
	}
	else
	{
		Type T = ((Point.X - LineIn.A.X) * Dir.X + (Point.Y - LineIn.A.Y) * Dir.Y
		          + (Point.Z - LineIn.A.Z) * Dir.Z)
		         / DirLenSq;
		if (ClampMode == 0)
		{
			if (T <= (Type)0)
				T = (Type)0;
			if ((Type)1 <= T)
				T = (Type)1;
		}
		Closest = Vec3<Type>(LineIn.A.X + T * Dir.X, LineIn.A.Y + T * Dir.Y,
		                     LineIn.A.Z + T * Dir.Z);
	}

	const Type Dx = Point.X - Closest.X;
	const Type Dy = Point.Y - Closest.Y;
	const Type Dz = Point.Z - Closest.Z;
	const Type DistSq = Dx * Dx + Dy * Dy + Dz * Dz;

	if (ResultMode == 0)
	{
		const Maybe<Type> Root = Sqrt(DistSq);
		ASSERT(Root.IsValid());
		return Root.Get();
	}
	return DistSq;
}

// 0x00502bc0. Barycentric coordinates of `Point` in `Triangle`, computed from
// the 2D projection that drops the triangle normal's largest component (this
// keeps the projected triangle well conditioned and preserves the weights
// exactly). Value1/Value2 are the weights of the first two vertices, Value3 is
// the remaining one; the permutation is not observable — the only caller
// (IsPointWithinTriangle) tests all three.
template <class Type>
inline tuple3<Type, Type, Type> GetPointBarycenterInTriangle(const Triangle3<Type>& Triangle,
                                                             const Vec3<Type>& Point)
{
	const Vec3<Type> Normal = ((Triangle.B - Triangle.A) % (Triangle.C - Triangle.A)).SafeNormalized();
	const int Drop = Normal.LargestDim();
	const int I0 = (Drop == 0) ? 1 : 0;
	const int I1 = (Drop == 2) ? 1 : 2;

	const Type AX = Triangle.A[I0], AY = Triangle.A[I1];
	const Type BX = Triangle.B[I0], BY = Triangle.B[I1];
	const Type CX = Triangle.C[I0], CY = Triangle.C[I1];
	const Type PX = Point[I0], PY = Point[I1];

	const Type Area = (BX - AX) * (CY - AY) - (CX - AX) * (BY - AY);
	const Type Weight0 = ((BX - PX) * (CY - PY) - (CX - PX) * (BY - PY)) / Area;
	const Type Weight1 = ((PX - AX) * (CY - AY) - (CX - AX) * (PY - AY)) / Area;
	return tuple3<Type, Type, Type>(Weight0, Weight1, (Type)1 - Weight0 - Weight1);
}

// 0x00503060. True when all three barycentric coordinates lie in [0,1]. A
// triangle with a zero-area normal (degenerate) is rejected.
template <class Type>
inline bool IsPointWithinTriangle(const Triangle3<Type>& Triangle, const Vec3<Type>& Point)
{
	const Vec3<Type> Normal = ((Triangle.B - Triangle.A) % (Triangle.C - Triangle.A)).SafeNormalized();
	if (Normal.IsZero())
		return false;

	const tuple3<Type, Type, Type> Barycenter = GetPointBarycenterInTriangle(Triangle, Point);
	return Between(Barycenter.Value1, (Type)0, (Type)1)
	       && Between(Barycenter.Value2, (Type)0, (Type)1)
	       && Between(Barycenter.Value3, (Type)0, (Type)1);
}

// 0x004e6fb0. The two tangent points from an external `Point` to a circle:
// with u = (Point - Center)/|Point - Center| and alpha = SafeACos(Radius / d),
// the points are Center + Radius * Rot(-/+alpha) * u. Value1 is the -alpha
// rotation, Value2 the +alpha rotation (matching the shipped store order).
template <class Type>
inline tuple2<Vec2<Type>, Vec2<Type>> TangentPointsToCircle(const Vec2<Type>& Point,
                                                            const Circle<Type>& CircleIn)
{
	const Vec2<Type> Delta = Point - CircleIn.Center;
	const Type Distance = Delta.Length();
	const Type Alpha = SafeACos(CircleIn.Radius / Distance);
	const Vec2<Type> Unit = Delta.SafeNormalized();

	const Type CosA = Cos(Alpha);
	const Type SinA = Sin(Alpha);

	const Vec2<Type> Point1 = CircleIn.Center
	                          + Vec2<Type>(CosA * Unit.X + SinA * Unit.Y,
	                                       CosA * Unit.Y - SinA * Unit.X)
	                                * CircleIn.Radius;
	const Vec2<Type> Point2 = CircleIn.Center
	                          + Vec2<Type>(CosA * Unit.X - SinA * Unit.Y,
	                                       SinA * Unit.X + CosA * Unit.Y)
	                                * CircleIn.Radius;
	return tuple2<Vec2<Type>, Vec2<Type>>(Point1, Point2);
}

}  // namespace util
