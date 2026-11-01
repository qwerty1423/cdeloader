// Contract tests for the reconstructed util::AlphabeticComparison.
#include "utilities/strings/alphabetic.h"

#include <cstdio>

static int g_failures = 0;

static void check(util::AlphabeticComparator::Type got,
                  util::AlphabeticComparator::Type want, const char* what)
{
	if (got != want) {
		g_failures++;
		std::printf("FAIL: %s (got %d want %d)\n", what, (int)got, (int)want);
	}
}

int main()
{
	using util::AlphabeticComparator;
	using util::AlphabeticComparison;

	check(AlphabeticComparison("abc", "abc"), AlphabeticComparator::Equal, "equal");
	check(AlphabeticComparison("", ""), AlphabeticComparator::Equal, "empty");
	check(AlphabeticComparison("abc", "abd"), AlphabeticComparator::Less, "abc<abd");
	check(AlphabeticComparison("abd", "abc"), AlphabeticComparator::Greater, "abd>abc");
	check(AlphabeticComparison("ab", "abc"), AlphabeticComparator::Less, "prefix shorter");
	check(AlphabeticComparison("abc", "ab"), AlphabeticComparator::Greater, "prefix longer");
	check(AlphabeticComparison("", "a"), AlphabeticComparator::Less, "empty<a");
	check(AlphabeticComparison("a", ""), AlphabeticComparator::Greater, "a>empty");
	check(AlphabeticComparison("aBc", "aBc"), AlphabeticComparator::Equal, "mixed equal");

	// Lowercasing decides when the first characters differ without a case-only tie.
	check(AlphabeticComparison("a", "B"), AlphabeticComparator::Less, "lowercase compare");
	check(AlphabeticComparison("x", "Y"), AlphabeticComparator::Less, "x<Y");

	// Recovered quirk: a case-only difference is decided by raw char order and
	// stops the recursion.
	check(AlphabeticComparison("ABC", "abc"), AlphabeticComparator::Less, "ABC<abc raw");
	check(AlphabeticComparison("abc", "ABC"), AlphabeticComparator::Greater, "abc>ABC raw");
	check(AlphabeticComparison("a", "A"), AlphabeticComparator::Greater, "a>A raw");
	check(AlphabeticComparison("A", "a"), AlphabeticComparator::Less, "A<a raw");

	if (g_failures == 0)
		std::printf("ALL PASS (alphabetic)\n");
	else
		std::printf("%d failures (alphabetic)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
