// util::Maybe and util::Sqrt.
//
// Reconstructed from the shipped binary:
//   * Maybe<Type> layout is { Type Value; bool Valid; } (Value at offset 0,
//     the validity byte at offset sizeof(Type)); the assert text
//     `IsValid()` and file .../utilities/core/maybe.h:19 appear in the binary.
//   * util::Sqrt<Type> (VA 0x008332e0) returns a valid Sqrt for X >= 0 and an
//     invalid Maybe (Value 0) otherwise; it is what Vec2/Vec3 length helpers
//     call before their `len > 0` assertions.
#pragma once

#include "utilities/coremath/coremath.h"

namespace util {

template <class Type>
class Maybe
{
public:
	Maybe() : Value(), Valid(false) {}
	Maybe(const Type& InValue) : Value(InValue), Valid(true) {}

	bool IsValid() const { return Valid; }

	const Type& Get() const
	{
		ASSERT(Valid);
		return Value;
	}

	Type& Get()
	{
		ASSERT(Valid);
		return Value;
	}

private:
	Type Value;
	bool Valid;
};

template <class Type>
inline Maybe<Type> Sqrt(const Type& X)
{
	Maybe<Type> Result;
	if (X >= (Type)0)
	{
		Result = Maybe<Type>((Type)std::sqrt(X));
	}
	return Result;
}

}  // namespace util
