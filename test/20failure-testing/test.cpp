// Failure-focused testing — the template's opinionated answer to "what should
// a test actually prove?"
//
// A happy-path assertion (`REQUIRE(fun(10 / 5) == 2)`) only proves the code
// returns what you already know it returns, on input you chose because it
// works. The valuable tests are the adversarial ones: *how could this fail?*
// For every function, enumerate the failure modes — bad input, boundaries,
// overflow, malformed external data — and make those the tests. The
// happy-path check is the LAST test, a smoke check that the harness runs.
//
// This example uses a safe integer-division helper that returns
// std::expected (C++23) instead of invoking UB, so its failure modes are
// explicit and testable.

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <expected>

namespace example {

enum class DivError { DivideByZero, Overflow };

// divide(a, b) = a / b, with the two ways integer division can fail made
// explicit rather than undefined:
//   * b == 0            -> DivideByZero   (would be UB / SIGFPE)
//   * INT_MIN / -1      -> Overflow       (would be UB: quotient not
//                                          representable)
auto divide(int a, int b) -> std::expected<int, DivError> {
  if (b == 0) {
    return std::unexpected{DivError::DivideByZero};
  }
  if (a == INT_MIN && b == -1) {
    return std::unexpected{DivError::Overflow};
  }
  return a / b;
}

}  // namespace example

using example::DivError;
using example::divide;

TEST_CASE("divide: failure modes are the contract", "[divide][failure]") {
  SECTION("division by zero is rejected, not UB") {
    const auto r = divide(10, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == DivError::DivideByZero);
  }

  SECTION("INT_MIN / -1 overflows instead of invoking UB") {
    const auto r = divide(INT_MIN, -1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == DivError::Overflow);
  }

  SECTION("boundary: INT_MIN / 1 is exact and representable") {
    const auto r = divide(INT_MIN, 1);
    REQUIRE(r.has_value());
    REQUIRE(r.value() == INT_MIN);
  }

  SECTION("boundary: truncation toward zero (7 / 2 == 3, not 4)") {
    REQUIRE(divide(7, 2).value() == 3);
    REQUIRE(divide(-7, 2).value() == -3);
  }

  // The happy path — present, but deliberately last and least interesting.
  SECTION("happy path: ordinary division") {
    REQUIRE(divide(10, 5).value() == 2);
  }
}
