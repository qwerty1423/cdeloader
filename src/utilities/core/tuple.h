// util::tuple2 / util::tuple3.
//
// Layouts from the PDB type stream:
//   tuple2<Type1,Type2>        Value1@0,  Value2@sizeof(Type1)
//   tuple3<Type1,Type2,Type3>  Value1@0,  Value2@sizeof(Type1), Value3@...
// Both expose `operator==` / `operator!=` and have a default and a
// value-per-member constructor.
#pragma once

namespace util {

template <class Type1, class Type2>
class tuple2
{
public:
	tuple2() : Value1(), Value2() {}
	tuple2(const Type1& InValue1, const Type2& InValue2) : Value1(InValue1), Value2(InValue2) {}

	bool operator==(const tuple2& T) const { return Value1 == T.Value1 && Value2 == T.Value2; }
	bool operator!=(const tuple2& T) const { return !(*this == T); }

	Type1 Value1;
	Type2 Value2;
};

template <class Type1, class Type2, class Type3>
class tuple3
{
public:
	tuple3() : Value1(), Value2(), Value3() {}
	tuple3(const Type1& InValue1, const Type2& InValue2, const Type3& InValue3)
	    : Value1(InValue1), Value2(InValue2), Value3(InValue3) {}

	bool operator==(const tuple3& T) const
	{
		return Value1 == T.Value1 && Value2 == T.Value2 && Value3 == T.Value3;
	}
	bool operator!=(const tuple3& T) const { return !(*this == T); }

	Type1 Value1;
	Type2 Value2;
	Type3 Value3;
};

}  // namespace util
