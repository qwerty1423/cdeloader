// coremath.inl — reconstruction.
//
// Evidence for this function (util::NormalizeAngle<double>, VA 0x004d0e70):
//   - PDB public: `??$NormalizeAngle@N@util@@YANN@Z`
//   - Assertion text observed in the binary:
//       file  "...\\utilities\\utilities\\coremath\\coremath.inl"
//       line  0x82 (130)
//       expr  "!IsNan(Angle_radians)"   (so the original parameter name is
//             Angle_radians)
//   - Decompiled body reduces to: subtract floor(x / TwoPi) * TwoPi, then fold
//     the result into [0, TwoPi); a two-pi result is normalised to 0.
//
// Parameter name and file/line are observations; the statement layout below is
// our reconstruction (inference) chosen to match the recovered control flow.
namespace util {

inline double NormalizeAngle(double Angle_radians)
{
	ASSERT(!IsNan(Angle_radians));

	double Angle = Angle_radians - Floor(Angle_radians / TwoPi) * TwoPi;

	if (Angle >= TwoPi)
		Angle = 0.0;
	else if (Angle < 0.0)
	{
		Angle += TwoPi;
		if (Angle == TwoPi)
			Angle = 0.0;
	}

	return Angle;
}

// DecimalMod — reconstruction of util::DecimalMod<Type> (VA 0x004d0b90).
// Returns X mod Y but with the sign of Y (unlike fmod, which follows X):
// for Y > 0 the result is in [0, Y), for Y < 0 it is in (Y, 0].
template <class Type>
inline Type DecimalMod(const Type& X, const Type& Y)
{
	if (Y == (Type)0)
		return X;

	Type Result = X - Floor(X / Y) * Y;

	const bool InRange = (Y > (Type)0) ? (Result < Y) : (Y < Result);
	if (InRange)
	{
		const bool SameSign = (Y > (Type)0) ? (Result > (Type)0) : (Result < (Type)0);
		if (SameSign || Result == (Type)0)
			return Result;
		if (Result + Y != Y)
			return Result + Y;
	}

	return (Type)0;
}

// SafeACos / SafeASin — reconstruction.
//
// Evidence: PDB publics `??$SafeACos@N@util@@YANN@Z` (VA 0x00491eb0) and
// `??$SafeASin@N@util@@YANN@Z` (VA 0x004d0f30). The inlined assert text is
// preserved verbatim from the binary:
//   "Between<Type>(X, (Type)-1, (Type)1) || Equals(X, (Type)-1) || Equals(X, (Type)1)"
// (coremath.h:91). The body clamps X to [-1, 1] and calls acos / asin.
// Helper names (Between/Equals/Clamp/Epsilon) are inference; the expressions
// and threshold come from the binary.
template <class Type>
inline Type SafeACos(const Type& X)
{
	ASSERT(Between<Type>(X, (Type)-1, (Type)1) || Equals(X, (Type)-1) || Equals(X, (Type)1));
	return static_cast<Type>(std::acos(Clamp<Type>(X, (Type)-1, (Type)1)));
}

template <class Type>
inline Type SafeASin(const Type& X)
{
	ASSERT(Between<Type>(X, (Type)-1, (Type)1) || Equals(X, (Type)-1) || Equals(X, (Type)1));
	return static_cast<Type>(std::asin(Clamp<Type>(X, (Type)-1, (Type)1)));
}

// Pow — reconstruction of util::Pow<Type> (VA 0x005ab360).
// A negative exponent inverts the base; the exponent is then applied by a simple
// repeated-multiplication loop (the binary shows this unrolled eight ways, which
// is a compiler transform of the loop below).
template <class Type>
inline Type Pow(Type Base, int Exponent)
{
	if (Exponent < 0)
	{
		Base = (Type)1 / Base;
		Exponent = -Exponent;
	}

	Type Result = (Type)1;
	for (int i = 0; i < Exponent; ++i)
		Result *= Base;

	return Result;
}

}  // namespace util
