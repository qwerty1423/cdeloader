// Utilities / core math — reconstruction of the CDE "utilities" library.
//
// Recovered from CDE.exe.original. Names, the TwoPi constant, the debug
// assertion, and the enclosing file/line come from the matched PDB and the
// binary (see notes/FINDINGS.md). Anything not directly evidenced is marked
// `inference`.
#pragma once

#include <cmath>
#include <cstdio>

// Debug assertion.
// Observed in the binary: a failing assert calls a reporting routine (0x00831710)
// with (file, line, expression) and then the DebugBreak import (via [0x00913098]).
// The shipped Release build kept these asserts (e.g. coremath.inl:130).
#if defined(_MSC_VER)
#  define CDE_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__)
#  define CDE_DEBUG_BREAK() __builtin_trap()
#else
#  include <csignal>
#  define CDE_DEBUG_BREAK() std::raise(SIGABRT)
#endif

#ifndef ASSERT
#  define ASSERT(Expression)                                              \
	do {                                                                  \
		if (!(Expression)) {                                              \
			std::printf("%s(%d): assertion failed: %s\n", __FILE__,       \
			            __LINE__, #Expression);                           \
			CDE_DEBUG_BREAK();                                            \
		}                                                                 \
	} while (0)
#endif

namespace util {

// 2*pi. Matches the double at image VA 0x009193f8, bytes
// 18 2d 44 54 fb 21 19 40 (little-endian).
constexpr double TwoPi = 6.283185307179586;

inline bool IsNan(double Value)
{
	return Value != Value;
}

// Relative epsilon for two-argument Equals (decompiled threshold: 1e-08).
constexpr double Epsilon = 1e-8;

// Math wrappers. The binary implements these as thin calls to the CRT, e.g.
// util::Abs (0x008331d0), util::Floor (0x00833250), util::Ceil (0x00833210).
inline double Abs(const double& Value) { return std::fabs(Value); }
inline double Floor(const double& Value) { return std::floor(Value); }
inline double Ceil(const double& Value) { return std::ceil(Value); }
inline double Cbrt(const double& Value) { return std::cbrt(Value); }
inline double Sin(const double& Value) { return std::sin(Value); }
inline double Cos(const double& Value) { return std::cos(Value); }
inline double Tan(const double& Value) { return std::tan(Value); }

template <class Type>
inline Type Max(const Type& A, const Type& B)
{
	return A > B ? A : B;
}

template <class Type>
inline Type Min(const Type& A, const Type& B)
{
	return A < B ? A : B;
}

// Min <= X && X <= Max.  (Expression text taken from the binary's assert.)
template <class Type>
inline bool Between(const Type& X, const Type& Min, const Type& Max)
{
	return Min <= X && X <= Max;
}

// Relative approximate equality. Exact-equal shortcut first; otherwise the
// absolute difference is scaled by the greater magnitude when both operands are
// nonzero. Reconstruction of util::Equals<Type> (VA 0x00501800).
template <class Type>
inline bool Equals(const Type& X, const Type& Y, const Type& Tolerance)
{
	if (X == Y)
		return true;
	Type Difference = Abs(X - Y);
	if (X != (Type)0 && Y != (Type)0)
	{
		Type Greatest = Max(Abs(X), Abs(Y));
		ASSERT(Greatest > (Type)0);
		Difference = Difference / Greatest;
	}
	return Difference < Tolerance;
}

template <class Type>
inline bool Equals(const Type& X, const Type& Y)
{
	return Equals(X, Y, (Type)Epsilon);
}

// Restrict X to [Min, Max], matching the recovered branch order.
template <class Type>
inline Type Clamp(const Type& X, const Type& Min, const Type& Max)
{
	Type Result = X;
	if (Result <= Min)
		Result = Min;
	if (Max <= Result)
		Result = Max;
	return Result;
}

}  // namespace util

#include "coremath.inl"
