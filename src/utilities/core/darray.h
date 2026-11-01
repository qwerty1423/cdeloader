// util::IArray<Type> / util::Darray<Type> — the dynamic-array container.
//
// Recovered from the shipped binary (original path
// utilities/utilities/core/darray.inl):
//   Darray<Type> layout: vtable@0 (from IArray), Data@4, Size@8, Capacity@12,
//   sizeof 16. The array maximum is DARRAY_MAX_SIZE = 0x3fffffff.
//
// Out-of-line bodies used as evidence:
//   0x005051f0  Darray(int Capacity)      — reserve, elements value-initialised
//   0x00505140  Darray(Darray const&)     — copy
//   0x004f3530  operator=(Darray const&)
//   0x00492620  Clear                     — SetSize(0)
//   0x00492640  SetAll                    — assign every live element
//   0x00494fb0  CheckSize(int)            — grow to fit Size, copy existing
//   0x00499400  SetSize(int const&)       — clamp/assert, zero the tail
//   0x004e7380  Add(IndexData const&)     — SetSize(Size+1); CheckSize; store
//   0x004e4910  Insert(Level* const&, int const&)
//   0x006f8be0  RemoveAt(int const&)      — shift down, shrink
//   0x0089f610  Remove(Reflectable* const&)
//   0x0089dbd0  Contains(Reflectable* const&)
// The 0x00505140/0x004f3530 copies use the raw allocator; here we use C++
// storage, which is behaviourally equivalent and idiomatic.
#pragma once

#include "utilities/coremath/coremath.h"

namespace util {

// DARRAY_MAX_SIZE — asserted in SetSize (`Between(value, 0, DARRAY_MAX_SIZE)`
// at core/darray.inl:0x2a5).
constexpr int DarrayMaxSize = 0x3fffffff;

template <class Type>
class IArray
{
public:
	virtual ~IArray() {}

	virtual Type& operator[](const int& Index) const = 0;
	virtual Type& First() = 0;
	virtual const Type& First() const = 0;
	virtual Type& Last() = 0;
	virtual const Type& Last() const = 0;
	virtual const int Num() const = 0;
	virtual void Clear() = 0;
	virtual void SetAll(const Type& Value) = 0;
};

template <class Type>
class Darray : public IArray<Type>
{
public:
	Darray() : Data(0), Size(0), Capacity(0) {}

	explicit Darray(int InCapacity) : Data(0), Size(0), Capacity(0)
	{
		if (InCapacity < 0 || InCapacity > DarrayMaxSize)
			return;
		if (InCapacity > 0)
		{
			Capacity = InCapacity;
			Data = new Type[Capacity]();
		}
	}

	Darray(const Darray& Other) : Data(0), Size(0), Capacity(0)
	{
		Capacity = Other.Capacity;
		SetSize(Other.Num());
		if (Capacity > 0)
		{
			Data = new Type[Capacity]();
			for (int i = 0; i < Size; i++)
				Data[i] = Other.Data[i];
		}
	}

	virtual ~Darray() { delete[] Data; }

	Darray& operator=(const Darray& Other)
	{
		if (Other.Num() == 0)
		{
			Clear();
			return *this;
		}
		SetSize(Other.Num());
		if (Capacity < Size)
		{
			int NewCapacity = (Capacity == 0) ? Size : Capacity;
			while (NewCapacity < Size)
				NewCapacity *= 2;
			if (NewCapacity > DarrayMaxSize)
				NewCapacity = DarrayMaxSize;
			delete[] Data;
			Data = new Type[NewCapacity]();
			Capacity = NewCapacity;
		}
		for (int i = 0; i < Other.Size; i++)
			Data[i] = Other.Data[i];
		return *this;
	}

	virtual Type& operator[](const int& Index) const { return Data[Index]; }
	virtual Type& First() { return Data[0]; }
	virtual const Type& First() const { return Data[0]; }
	virtual Type& Last() { return Data[Size - 1]; }
	virtual const Type& Last() const { return Data[Size - 1]; }
	virtual const int Num() const { return Size; }
	virtual void Clear() { SetSize(0); }
	virtual void SetAll(const Type& Value)
	{
		for (int i = 0; i < Size; i++)
			Data[i] = Value;
	}

	int GetCapacity() const { return Capacity; }
	Type* Start() { return Data; }
	Type* End() { return Data + Size; }
	const Type* Start() const { return Data; }
	const Type* End() const { return Data + Size; }

	static Darray SingleItem(const Type& Value)
	{
		Darray Result(1);
		Result.Add(Value);
		return Result;
	}

	bool Add(const Type& Value)
	{
		const int OldSize = Size;
		SetSize(OldSize + 1);
		if (!CheckSize(OldSize))
			return false;
		Data[OldSize] = Value;
		return true;
	}

	bool AddUnique(const Type& Value)
	{
		if (Contains(Value))
			return false;
		return Add(Value);
	}

	bool Insert(const Type& Value, const int& Index)
	{
		if (Index < 0 || Index > Num())
			return false;
		const int OldSize = Size;
		SetSize(OldSize + 1);
		if (!CheckSize(OldSize))
			return false;
		for (int i = OldSize - 1; i >= Index; i--)
			Data[i + 1] = Data[i];
		Data[Index] = Value;
		return true;
	}

	bool RemoveAt(const int& Index)
	{
		const int N = Num();
		if (N == 0 || !Between(Index, 0, N - 1))
			return false;
		for (int i = Index; i < N - 1; i++)
			Data[i] = Data[i + 1];
		SetSize(N - 1);
		return true;
	}

	bool SwapRemoveAt(const int& Index)
	{
		const int N = Num();
		if (N == 0 || !Between(Index, 0, N - 1))
			return false;
		Data[Index] = Data[N - 1];
		SetSize(N - 1);
		return true;
	}

	bool Remove(const Type& Value)
	{
		const int Index = FindIndex(Value);
		if (Index < 0)
			return false;
		return RemoveAt(Index);
	}

	int FindIndex(const Type& Value) const
	{
		for (int i = 0; i < Size; i++)
			if (Data[i] == Value)
				return i;
		return -1;
	}

	bool Contains(const Type& Value) const { return FindIndex(Value) >= 0; }

	Darray& operator+=(const Darray& Other)
	{
		for (int i = 0; i < Other.Num(); i++)
			Add(Other[i]);
		return *this;
	}

	Darray operator+(const Darray& Other) const
	{
		Darray Result(*this);
		Result += Other;
		return Result;
	}

	bool operator==(const Darray& Other) const
	{
		if (Num() != Other.Num())
			return false;
		for (int i = 0; i < Size; i++)
			if (!(Data[i] == Other.Data[i]))
				return false;
		return true;
	}

	bool operator!=(const Darray& Other) const { return !(*this == Other); }

private:
	// The field order matches the recovered layout (Data, Size, Capacity); the
	// vtable pointer precedes them and keeps sizeof(Darray) == 16.
	Type* Data;
	int Size;
	int Capacity;

	bool CheckSize(int ElementsToCopy)
	{
		if (Size < 0)
			SetSize(0);
		if (Size > DarrayMaxSize)
			SetSize(DarrayMaxSize);
		if (Size <= Capacity)
			return true;

		int NewCapacity = (Capacity == 0) ? Size : Capacity;
		while (NewCapacity < Size)
			NewCapacity *= 2;
		if (NewCapacity > DarrayMaxSize)
			NewCapacity = DarrayMaxSize;

		Type* NewData = new Type[NewCapacity]();
		if (Data != 0)
		{
			for (int i = 0; i < ElementsToCopy && i < NewCapacity; i++)
				NewData[i] = Data[i];
			delete[] Data;
		}
		Data = NewData;
		Capacity = NewCapacity;
		return true;
	}

	void SetSize(const int& Value)
	{
		ASSERT(Between(Value, 0, DarrayMaxSize));
		const int NewSize = Clamp(Value, 0, DarrayMaxSize);
		if (NewSize < Size && Data != 0)
			for (int i = NewSize; i < Size; i++)
				Data[i] = Type();
		Size = NewSize;
	}
};

}  // namespace util
