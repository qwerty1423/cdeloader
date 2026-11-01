// Contract tests for util::Vec4, util::Matrix3x3 and util::Matrix4x4.
#include "utilities/coremath/matrix.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

static void expect(bool ok, const char* what)
{
	if (!ok) {
		g_failures++;
		std::printf("FAIL: %s\n", what);
	}
}

static bool eq(double a, double b) { return std::fabs(a - b) < 1e-12; }

int main()
{
	using util::Matrix3x3;
	using util::Matrix4x4;
	using util::Vec2;
	using util::Vec3;
	using util::Vec4;

	// Vec4 basics.
	expect(Vec4<double>::GetDimensions() == 4, "Vec4 dims");
	expect(Vec4<double>(3, 4, 12, 0).Length() == 13.0, "Vec4 length");
	expect(Vec4<double>(1, 2, 3, 4)[2] == 3.0, "Vec4 operator[]");

	// Default matrices are the identity.
	{
		const Matrix4x4<double> I;
		expect(I(0, 0) == 1.0 && I(1, 1) == 1.0 && I(2, 2) == 1.0 && I(3, 3) == 1.0,
		       "Matrix4x4 identity diagonal");
		expect(I(0, 1) == 0.0 && I(3, 0) == 0.0, "Matrix4x4 identity off-diagonal");
	}
	{
		const Matrix3x3<double> I;
		expect(I(0, 0) == 1.0 && I(1, 1) == 1.0 && I(2, 2) == 1.0 && I(0, 2) == 0.0,
		       "Matrix3x3 identity");
	}

	// ClearToZero / ClearToIdentity.
	{
		Matrix4x4<double> M;
		M.ClearToZero();
		bool allZero = true;
		for (int i = 0; i < 16; ++i)
			if (M.Data[i] != 0.0)
				allZero = false;
		expect(allZero, "ClearToZero");
		M.ClearToIdentity();
		expect(M.Data[0] == 1.0 && M.Data[5] == 1.0 && M.Data[10] == 1.0 && M.Data[15] == 1.0
		           && M.Data[1] == 0.0,
		       "ClearToIdentity");
	}

	// FromTranslation: translation goes into Data[12..14] (last row).
	{
		const Matrix4x4<double> T = Matrix4x4<double>::FromTranslation(Vec3<double>(1, 2, 3));
		expect(T.Data[12] == 1.0 && T.Data[13] == 2.0 && T.Data[14] == 3.0, "FromTranslation row");
		expect(T.Data[0] == 1.0 && T.Data[5] == 1.0 && T.Data[10] == 1.0 && T.Data[15] == 1.0,
		       "FromTranslation diag");
		expect(T.Data[3] == 0.0 && T.Data[7] == 0.0, "FromTranslation off");

		// Row-vector transform: v * T adds the translation.
		const Vec4<double> P = T * Vec4<double>(10, 20, 30, 1);
		expect(P == Vec4<double>(11, 22, 33, 1), "row-vector translate");
	}

	// FromScale: scale on the diagonal.
	{
		const Matrix4x4<double> S = Matrix4x4<double>::FromScale(Vec3<double>(2, 3, 4));
		expect(S.Data[0] == 2.0 && S.Data[5] == 3.0 && S.Data[10] == 4.0, "FromScale diag");
		expect(S.Data[1] == 0.0 && S.Data[12] == 0.0, "FromScale off");
		const Vec4<double> P = S * Vec4<double>(1, 1, 1, 1);
		expect(P == Vec4<double>(2, 3, 4, 1), "row-vector scale");
	}

	// FromFrustum matches the decompiled flat layout exactly.
	{
		const double L = -1, R = 1, B = -1, Tp = 1, N = 1, F = 100;
		const Matrix4x4<double> P = Matrix4x4<double>::FromFrustum(L, R, B, Tp, N, F);
		expect(eq(P.Data[0], (2 * N) / (R - L)), "frustum [0]");
		expect(eq(P.Data[5], (2 * N) / (Tp - B)), "frustum [5]");
		expect(eq(P.Data[8], (R + L) / (R - L)), "frustum [8]");
		expect(eq(P.Data[9], (Tp + B) / (Tp - B)), "frustum [9]");
		expect(eq(P.Data[10], -((F + N) / (F - N))), "frustum [10]");
		expect(P.Data[11] == -1.0, "frustum [11]");
		expect(eq(P.Data[14], (F * -2 * N) / (F - N)), "frustum [14]");
		expect(P.Data[1] == 0.0 && P.Data[4] == 0.0 && P.Data[12] == 0.0, "frustum zeros");
	}

	// Matrix product is the row-major product; two translations compose.
	{
		const Matrix4x4<double> T1 = Matrix4x4<double>::FromTranslation(Vec3<double>(1, 2, 3));
		const Matrix4x4<double> T2 = Matrix4x4<double>::FromTranslation(Vec3<double>(4, 5, 6));
		const Matrix4x4<double> T = T1 * T2;
		expect(T.Data[12] == 5.0 && T.Data[13] == 7.0 && T.Data[14] == 9.0, "translate compose");

		const Matrix4x4<double> I;
		expect((I * I).Data[0] == 1.0 && (I * I).Data[5] == 1.0, "identity * identity");
	}

	// Transpose.
	{
		Matrix4x4<double> M;
		M(0, 1) = 7.0;
		M(2, 3) = 9.0;
		const Matrix4x4<double> T = M.Transpose();
		expect(T(1, 0) == 7.0 && T(3, 2) == 9.0 && T(0, 1) == 0.0, "transpose swap");
		expect(M.Transpose().Transpose()(0, 1) == 7.0, "transpose involution");
	}

	// operator()/operator[] agree (row-major indexing).
	{
		Matrix4x4<double> M;
		M(1, 2) = 5.0;
		expect(M[Vec2<int>(1, 2)] == 5.0, "operator[] matches operator()");
		expect(M.Data[1 * 4 + 2] == 5.0, "raw layout row-major");
	}

	// Matrix3x3: product, vector transform, transpose, 4x4 slicing.
	{
		Matrix3x3<double> S;
		S(0, 0) = 2.0;
		S(1, 1) = 3.0;
		S(2, 2) = 4.0;
		const Vec3<double> P = S * Vec3<double>(1, 1, 1);
		expect(P == Vec3<double>(2, 3, 4), "Matrix3x3 row-vector scale");

		Matrix4x4<double> M4;
		M4(0, 1) = 8.0;
		M4(2, 2) = 5.0;
		M4(3, 3) = 99.0;  // outside the upper-left 3x3, must be dropped
		const Matrix3x3<double> M3(M4);
		expect(M3(0, 1) == 8.0 && M3(2, 2) == 5.0, "Matrix3x3 from Matrix4x4");

		const Matrix3x3<double> I;
		expect((I * I)(0, 0) == 1.0 && (I * I)(1, 2) == 0.0, "Matrix3x3 identity product");
		expect(I.Transpose()(1, 0) == 0.0, "Matrix3x3 transpose");
	}

	// Matrix3x3 addition.
	{
		Matrix3x3<double> A;
		Matrix3x3<double> B;
		A(0, 0) = 1.0;
		B(0, 0) = 2.0;
		expect((A + B)(0, 0) == 3.0, "Matrix3x3 operator+");
	}

	if (g_failures == 0)
		std::printf("ALL PASS (matrix)\n");
	else
		std::printf("%d failures (matrix)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
