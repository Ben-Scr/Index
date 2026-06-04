// Tests for Index::ExpressionEvaluator::Evaluate — the arithmetic parser behind
// expression entry in numeric inspector fields (e.g. typing "5/2" or "/2").

#include <doctest/doctest.h>

#include "Utils/ExpressionEvaluator.hpp"

using Index::ExpressionEvaluator::Evaluate;

namespace {
	// Evaluate with a current value of 0 (irrelevant unless the input starts with
	// an operator) and return the result; fails the check if parsing failed.
	double Eval(const char* input, double current = 0.0) {
		double out = 0.0;
		REQUIRE(Evaluate(input, current, out));
		return out;
	}

	bool Fails(const char* input, double current = 0.0) {
		double out = 12345.0;
		const bool ok = Evaluate(input, current, out);
		if (!ok) CHECK(out == 12345.0); // outResult must be untouched on failure
		return !ok;
	}
} // namespace

TEST_CASE("Evaluate — plain literals") {
	CHECK(Eval("5") == doctest::Approx(5.0));
	CHECK(Eval("5.5") == doctest::Approx(5.5));
	CHECK(Eval("-5") == doctest::Approx(-5.0));
	CHECK(Eval(".5") == doctest::Approx(0.5));
	CHECK(Eval("1e3") == doctest::Approx(1000.0));
	CHECK(Eval("1.5e-2") == doctest::Approx(0.015));
	CHECK(Eval("  42  ") == doctest::Approx(42.0)); // surrounding whitespace
}

TEST_CASE("Evaluate — operators and precedence") {
	CHECK(Eval("5/2") == doctest::Approx(2.5));
	CHECK(Eval("2+3*4") == doctest::Approx(14.0));      // * before +
	CHECK(Eval("(2+3)*4") == doctest::Approx(20.0));    // parens override
	CHECK(Eval("10-2-3") == doctest::Approx(5.0));      // left-assoc subtraction
	CHECK(Eval("5 % 3") == doctest::Approx(2.0));
	CHECK(Eval("7.5 % 2") == doctest::Approx(1.5));     // float modulo
}

TEST_CASE("Evaluate — power is right-associative, below unary minus") {
	CHECK(Eval("2^3") == doctest::Approx(8.0));
	CHECK(Eval("2^3^2") == doctest::Approx(512.0));     // 2^(3^2), not (2^3)^2
	CHECK(Eval("-3^2") == doctest::Approx(-9.0));        // -(3^2)
	CHECK(Eval("2^-3") == doctest::Approx(0.125));
}

TEST_CASE("Evaluate — unary chains") {
	CHECK(Eval("-(3)") == doctest::Approx(-3.0));
	CHECK(Eval("--5") == doctest::Approx(5.0));
	CHECK(Eval("-(2+3)") == doctest::Approx(-5.0));
}

TEST_CASE("Evaluate — leading * / % ^ operate on the current value") {
	CHECK(Eval("/2", 10.0) == doctest::Approx(5.0));
	CHECK(Eval("*3", 4.0) == doctest::Approx(12.0));
	CHECK(Eval("%3", 10.0) == doctest::Approx(1.0));
	CHECK(Eval("^2", 5.0) == doctest::Approx(25.0));
	CHECK(Eval("*-2", 3.0) == doctest::Approx(-6.0));     // current * (-2)
	CHECK(Eval("/2+1", 10.0) == doctest::Approx(6.0));    // (current/2)+1, precedence preserved
	CHECK(Eval("/2", -8.0) == doctest::Approx(-4.0));     // negative current
}

TEST_CASE("Evaluate — leading +/- is a literal sign, not an op on current") {
	CHECK(Eval("-5", 10.0) == doctest::Approx(-5.0));     // not 10-5
	CHECK(Eval("+5", 10.0) == doctest::Approx(5.0));      // not 10+5
}

TEST_CASE("Evaluate — invalid input leaves the value untouched") {
	CHECK(Fails(""));
	CHECK(Fails("   "));
	CHECK(Fails("5/"));        // dangling operator
	CHECK(Fails("5abc"));      // trailing garbage
	CHECK(Fails("abc"));
	CHECK(Fails("(1+2"));      // unbalanced parens
	CHECK(Fails("*"));         // lone operator, even with current
}

TEST_CASE("Evaluate — non-finite results are rejected") {
	CHECK(Fails("5/0"));       // +inf
	CHECK(Fails("0/0"));       // NaN
	CHECK(Fails("/0", 5.0));   // current / 0
}
