// Alphabetic string comparison.
//
// Reconstruction of util::AlphabeticComparison<Char> (VA 0x004f0b80) and the
// result type util::AlphabeticComparator::Type.
//
// Recovered behavior:
//   * characters are compared case-insensitively (ASCII 'A'..'Z' lowered by
//     the constant at 0x0095f328, which is 0x20),
//   * when the lowered characters are equal and the raw characters are also
//     equal, the comparison recurses on the next character (the binary really
//     does emit a recursive call, not a loop),
//   * when the lowered characters are equal but the raw characters differ in
//     case, the raw characters decide immediately.
//
// The numeric result values (0/1/2) are observed from the binary; the
// enumerator names are inference.
#pragma once

namespace util {

struct AlphabeticComparator
{
	enum Type
	{
		Greater = 0,  // left sorts after right
		Equal = 1,    // same string
		Less = 2      // left sorts before right
	};
};

template <class Char>
AlphabeticComparator::Type AlphabeticComparison(const Char* A, const Char* B)
{
	const Char AChar = *A;
	const Char BChar = *B;

	const Char ALower = (AChar >= (Char)'A' && AChar <= (Char)'Z')
	                        ? (Char)(AChar + ((Char)'a' - (Char)'A'))
	                        : AChar;
	const Char BLower = (BChar >= (Char)'A' && BChar <= (Char)'Z')
	                        ? (Char)(BChar + ((Char)'a' - (Char)'A'))
	                        : BChar;

	if (ALower != BLower)
	{
		if (ALower == (Char)'\0')
			return AlphabeticComparator::Less;
		if (BLower == (Char)'\0')
			return AlphabeticComparator::Greater;
		return (BLower < ALower) ? AlphabeticComparator::Greater
		                         : AlphabeticComparator::Less;
	}

	if (ALower == (Char)'\0')
		return AlphabeticComparator::Equal;

	if (AChar == BChar)
		return AlphabeticComparison(A + 1, B + 1);

	return (BChar < AChar) ? AlphabeticComparator::Greater
	                       : AlphabeticComparator::Less;
}

}  // namespace util
