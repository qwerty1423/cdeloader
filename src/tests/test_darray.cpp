// Contract tests for the reconstructed util::Darray / util::IArray.
#include "utilities/core/darray.h"

#include <cstdio>
#include <cstddef>

static int g_failures = 0;

#define EXPECT(cond)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                    \
			g_failures++;                                                 \
			std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
		}                                                                 \
	} while (0)

int main()
{
	using util::Darray;

	// ---- layout ----------------------------------------------------------
#if defined(__i386__) || defined(_M_IX86)
	// 32-bit layout from the PDB: vptr@0, Data@4, Size@8, Capacity@12.
	EXPECT(sizeof(Darray<int>) == 16);
	EXPECT(sizeof(Darray<double>) == 16);
#endif

	// ---- append / index / accessors -------------------------------------
	{
		Darray<int> A;
		EXPECT(A.Num() == 0);
		EXPECT(A.Add(5));
		EXPECT(A.Add(6));
		EXPECT(A.Add(7));
		EXPECT(A.Num() == 3);
		EXPECT(A[0] == 5 && A[1] == 6 && A[2] == 7);
		EXPECT(A.First() == 5 && A.Last() == 7);
		EXPECT(A.GetCapacity() >= 3);
		EXPECT(A.Contains(6) && !A.Contains(9));
		EXPECT(A.FindIndex(7) == 2 && A.FindIndex(9) == -1);

		// growth past the initial capacity
		for (int i = 0; i < 100; i++)
			A.Add(i);
		EXPECT(A.Num() == 103);
		EXPECT(A[0] == 5 && A[102] == 99);
	}

	// ---- reserve constructor --------------------------------------------
	{
		Darray<int> A(8);
		EXPECT(A.Num() == 0);
		EXPECT(A.GetCapacity() == 8);
	}

	// ---- insert ----------------------------------------------------------
	{
		Darray<int> A;
		A.Add(1);
		A.Add(3);
		EXPECT(A.Insert(2, 1));
		EXPECT(A.Num() == 3 && A[0] == 1 && A[1] == 2 && A[2] == 3);
		EXPECT(A.Insert(0, 0));
		EXPECT(A[0] == 0 && A[3] == 3);
		EXPECT(!A.Insert(9, -1));
		EXPECT(!A.Insert(9, 99));
	}

	// ---- remove ----------------------------------------------------------
	{
		Darray<int> A;
		for (int i = 0; i < 5; i++)
			A.Add(i);
		EXPECT(A.RemoveAt(0));
		EXPECT(A.Num() == 4 && A[0] == 1 && A[3] == 4);
		EXPECT(!A.RemoveAt(9));
		EXPECT(A.Remove(3));
		EXPECT(A.Num() == 3 && A[0] == 1 && A[1] == 2 && A[2] == 4);
		EXPECT(!A.Remove(42));
		EXPECT(A.SwapRemoveAt(0));
		EXPECT(A.Num() == 2 && A[0] == 4 && A[1] == 2);
	}

	// ---- SetAll / Clear / SingleItem ------------------------------------
	{
		Darray<int> A;
		A.Add(1);
		A.Add(2);
		A.Add(3);
		A.SetAll(7);
		EXPECT(A[0] == 7 && A[1] == 7 && A[2] == 7);
		A.Clear();
		EXPECT(A.Num() == 0);

		const Darray<int> One = Darray<int>::SingleItem(42);
		EXPECT(One.Num() == 1 && One[0] == 42);
	}

	// ---- copy / assign ---------------------------------------------------
	{
		Darray<int> A;
		for (int i = 0; i < 4; i++)
			A.Add(i * i);

		Darray<int> B(A);
		EXPECT(B.Num() == 4 && B[2] == 4);

		Darray<int> C;
		C = A;
		EXPECT(C.Num() == 4 && C[3] == 9);

		Darray<int> Empty;
		C = Empty;
		EXPECT(C.Num() == 0);
	}

	// ---- Start / End -----------------------------------------------------
	{
		Darray<int> A;
		A.Add(10);
		A.Add(20);
		int Sum = 0;
		for (int* P = A.Start(); P != A.End(); ++P)
			Sum += *P;
		EXPECT(Sum == 30);
	}

	if (g_failures == 0)
		std::printf("ALL PASS (darray)\n");
	else
		std::printf("%d failures (darray)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
