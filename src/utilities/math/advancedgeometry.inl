// advancedgeometry.inl — reconstruction.
//
// util::CalcSafePerpendicular<Type> (VA 0x00501520):
//   * assert `!IsNan(A_Norm) && !IsNan(B_Norm)` at math/advancedgeometry.inl:98
//     — so the original parameter names are A_Norm / B_Norm and both are
//     expected to be normalized;
//   * normally returns A % B (the cross product);
//   * when A is zero it substitutes a unit axis chosen from B's smallest
//     component, then crosses; when A and B are parallel (or B is zero) it
//     substitutes an axis chosen from A's smallest component;
//   * the parallel test is util::Equals<Vec3> with 1e-8.
namespace util {

namespace advancedgeometry_detail {

template <class Type>
inline Vec3<Type> UnitAxis(int Dimension)
{
	if (Dimension <= 0)
		return Vec3<Type>((Type)1, (Type)0, (Type)0);
	if (Dimension == 1)
		return Vec3<Type>((Type)0, (Type)1, (Type)0);
	return Vec3<Type>((Type)0, (Type)0, (Type)1);
}

}  // namespace advancedgeometry_detail

template <class Type>
inline Vec3<Type> CalcSafePerpendicular(const Vec3<Type>& A_Norm, const Vec3<Type>& B_Norm)
{
	ASSERT(!IsNan(A_Norm) && !IsNan(B_Norm));

	Vec3<Type> A = A_Norm;
	Vec3<Type> B = B_Norm;

	if (A.IsZero())
	{
		if (B.IsZero())
			return Vec3<Type>((Type)1, (Type)0, (Type)0);
		A = advancedgeometry_detail::UnitAxis<Type>(B.SmallestDim());
	}
	else if (B.IsZero() || Equals(A, B, (Type)Epsilon)
	         || Equals(A, Vec3<Type>(-B.X, -B.Y, -B.Z), (Type)Epsilon))
	{
		B = advancedgeometry_detail::UnitAxis<Type>(A.SmallestDim());
	}

	return A % B;
}

}  // namespace util
