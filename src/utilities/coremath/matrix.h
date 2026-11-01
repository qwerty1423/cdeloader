// util::Matrix3x3 / util::Matrix4x4.
//
// Layout from the PDB type stream:
//   Matrix3x3<double> sizeof 72  -> Type Data[9]
//   Matrix4x4<double> sizeof 128 -> Type Data[16]
// Both store a flat row-major array: element (Row, Col) is Data[Row*N + Col].
//
// Decompiled semantics that pin the layout (see notes/FINDINGS.md F22):
//   * Matrix4x4::FromFrustum writes Data[0], Data[5], Data[8], Data[9],
//     Data[10] = -(Far+Near)/(Far-Near), Data[11] = -1, Data[14] =
//     -2*Far*Near/(Far-Near). Under the row-major accessor above this is the
//     transpose of the classic GL projection matrix, exactly as the binary
//     stores it.
//   * Matrix4x4::operator* computes DataOut[r*4+c] = sum_k DataThis[r*4+k] *
//     DataArg[k*4+c] — the standard row-major product.
//   * FromTranslation places the translation in Data[12..14] and FromScale on
//     the diagonal.
//
// Not reconstructed yet: FromOrtho, FromPerspective, FromOrtho2D, To3x3-free
// helpers. GetDimensions/other inline accessors follow the same convention.
#pragma once

#include "utilities/coremath/vector.h"

namespace util {

template <class Type>
class Matrix4x4
{
public:
	static const int Dimensions = 4;

	Matrix4x4() { ClearToIdentity(); }  // inference: default is the identity

	void ClearToIdentity()
	{
		ClearToZero();
		for (int i = 0; i < Dimensions; ++i)
			Data[i * Dimensions + i] = (Type)1;
	}

	void ClearToZero()
	{
		for (int i = 0; i < Dimensions * Dimensions; ++i)
			Data[i] = (Type)0;
	}

	Type& operator[](const Vec2<int>& Coords) { return Data[Coords.X * Dimensions + Coords.Y]; }
	const Type& operator[](const Vec2<int>& Coords) const
	{
		return Data[Coords.X * Dimensions + Coords.Y];
	}

	Type& operator()(int Row, int Col) { return Data[Row * Dimensions + Col]; }
	const Type& operator()(int Row, int Col) const { return Data[Row * Dimensions + Col]; }

	Matrix4x4 operator*(const Matrix4x4& M) const
	{
		Matrix4x4 Result;
		for (int Row = 0; Row < Dimensions; ++Row)
			for (int Col = 0; Col < Dimensions; ++Col)
			{
				Type Sum = (Type)0;
				for (int k = 0; k < Dimensions; ++k)
					Sum += (*this)(Row, k) * M(k, Col);
				Result(Row, Col) = Sum;
			}
		return Result;
	}

	// The class stores rows and puts the translation in the last row, i.e. it
	// transforms row vectors: Result = V * M  (Result[Col] = sum_r V[r]*M(r,Col)).
	Vec4<Type> operator*(const Vec4<Type>& V) const
	{
		Vec4<Type> Result;
		for (int Col = 0; Col < Dimensions; ++Col)
		{
			Type Sum = (Type)0;
			for (int r = 0; r < Dimensions; ++r)
				Sum += V[r] * (*this)(r, Col);
			Result[Col] = Sum;
		}
		return Result;
	}

	Matrix4x4& operator*=(const Matrix4x4& M) { *this = *this * M; return *this; }

	Matrix4x4 Transpose() const
	{
		Matrix4x4 Result;
		for (int Row = 0; Row < Dimensions; ++Row)
			for (int Col = 0; Col < Dimensions; ++Col)
				Result(Row, Col) = (*this)(Col, Row);
		return Result;
	}

	static Matrix4x4 FromTranslation(const Vec3<Type>& Translation)
	{
		Matrix4x4 Result;
		Result(3, 0) = Translation.X;
		Result(3, 1) = Translation.Y;
		Result(3, 2) = Translation.Z;
		return Result;
	}

	static Matrix4x4 FromScale(const Vec3<Type>& Scale)
	{
		Matrix4x4 Result;
		Result(0, 0) = Scale.X;
		Result(1, 1) = Scale.Y;
		Result(2, 2) = Scale.Z;
		return Result;
	}

	// util::Matrix4x4<double>::FromFrustum (VA 0x00844950).
	static Matrix4x4 FromFrustum(const Type& Left, const Type& Right, const Type& Bottom,
	                             const Type& Top, const Type& Near, const Type& Far)
	{
		Matrix4x4 Result;
		Result.ClearToZero();
		Result(0, 0) = (Near + Near) / (Right - Left);
		Result(1, 1) = (Near + Near) / (Top - Bottom);
		Result(2, 0) = (Right + Left) / (Right - Left);
		Result(2, 1) = (Top + Bottom) / (Top - Bottom);
		Result(2, 2) = -((Far + Near) / (Far - Near));
		Result(2, 3) = (Type)-1;
		Result(3, 2) = (Far * (Type)-2 * Near) / (Far - Near);
		return Result;
	}

	Type Data[Dimensions * Dimensions];
};

template <class Type>
class Matrix3x3
{
public:
	static const int Dimensions = 3;

	Matrix3x3() { ClearToIdentity(); }  // inference: default is the identity

	// util::Matrix3x3<double>::Matrix3x3(const Matrix4x4<double>&) (VA 0x00532??)
	// takes the upper-left 3x3 of the source.
	explicit Matrix3x3(const Matrix4x4<Type>& M)
	{
		for (int Row = 0; Row < Dimensions; ++Row)
			for (int Col = 0; Col < Dimensions; ++Col)
				(*this)(Row, Col) = M(Row, Col);
	}

	void ClearToIdentity()
	{
		ClearToZero();
		for (int i = 0; i < Dimensions; ++i)
			Data[i * Dimensions + i] = (Type)1;
	}

	void ClearToZero()
	{
		for (int i = 0; i < Dimensions * Dimensions; ++i)
			Data[i] = (Type)0;
	}

	Type& operator[](const Vec2<int>& Coords) { return Data[Coords.X * Dimensions + Coords.Y]; }
	const Type& operator[](const Vec2<int>& Coords) const
	{
		return Data[Coords.X * Dimensions + Coords.Y];
	}

	Type& operator()(int Row, int Col) { return Data[Row * Dimensions + Col]; }
	const Type& operator()(int Row, int Col) const { return Data[Row * Dimensions + Col]; }

	Matrix3x3 operator*(const Matrix3x3& M) const
	{
		Matrix3x3 Result;
		for (int Row = 0; Row < Dimensions; ++Row)
			for (int Col = 0; Col < Dimensions; ++Col)
			{
				Type Sum = (Type)0;
				for (int k = 0; k < Dimensions; ++k)
					Sum += (*this)(Row, k) * M(k, Col);
				Result(Row, Col) = Sum;
			}
		return Result;
	}

	Vec3<Type> operator*(const Vec3<Type>& V) const
	{
		Vec3<Type> Result;
		for (int Col = 0; Col < Dimensions; ++Col)
		{
			Type Sum = (Type)0;
			for (int r = 0; r < Dimensions; ++r)
				Sum += V[r] * (*this)(r, Col);
			Result[Col] = Sum;
		}
		return Result;
	}

	Matrix3x3& operator*=(const Matrix3x3& M) { *this = *this * M; return *this; }

	Matrix3x3 operator+(const Matrix3x3& M) const
	{
		Matrix3x3 Result = *this;
		for (int i = 0; i < Dimensions * Dimensions; ++i)
			Result.Data[i] += M.Data[i];
		return Result;
	}

	Matrix3x3 Transpose() const
	{
		Matrix3x3 Result;
		for (int Row = 0; Row < Dimensions; ++Row)
			for (int Col = 0; Col < Dimensions; ++Col)
				Result(Row, Col) = (*this)(Col, Row);
		return Result;
	}

	Type Data[Dimensions * Dimensions];
};

}  // namespace util
