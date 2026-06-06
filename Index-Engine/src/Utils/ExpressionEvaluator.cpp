#include "pch.hpp"
#include "Utils/ExpressionEvaluator.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Index::ExpressionEvaluator {

	namespace {

		// Recursive-descent parser. Precedence, low to high:
		//   expr   := term (('+'|'-') term)*
		//   term   := unary (('*'|'/'|'%') unary)*
		//   unary  := ('+'|'-') unary | power
		//   power  := primary ('^' unary)?      right-associative
		//   primary:= number | '(' expr ')'
		struct Parser {
			const char* p;
			const char* end;
			bool ok = true;

			void SkipSpaces() {
				while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
			}

			char Peek() {
				SkipSpaces();
				return p < end ? *p : '\0';
			}

			bool AtEnd() {
				SkipSpaces();
				return p >= end;
			}

			double ParseExpr() {
				double value = ParseTerm();
				for (;;) {
					const char c = Peek();
					if (c == '+') { ++p; value += ParseTerm(); }
					else if (c == '-') { ++p; value -= ParseTerm(); }
					else break;
				}
				return value;
			}

			double ParseTerm() {
				double value = ParseUnary();
				for (;;) {
					const char c = Peek();
					if (c == '*') { ++p; value *= ParseUnary(); }
					else if (c == '/') { ++p; value /= ParseUnary(); }
					else if (c == '%') { ++p; value = std::fmod(value, ParseUnary()); }
					else break;
				}
				return value;
			}

			double ParseUnary() {
				const char c = Peek();
				if (c == '+') { ++p; return ParseUnary(); }
				if (c == '-') { ++p; return -ParseUnary(); }
				return ParsePower();
			}

			double ParsePower() {
				const double base = ParsePrimary();
				if (Peek() == '^') {
					++p;
					return std::pow(base, ParseUnary());
				}
				return base;
			}

			double ParsePrimary() {
				if (Peek() == '(') {
					++p;
					const double value = ParseExpr();
					if (Peek() == ')') ++p;
					else ok = false;
					return value;
				}
				// Peek() already skipped whitespace, so p points at the first digit/dot.
				char* numEnd = nullptr;
				const double value = std::strtod(p, &numEnd);
				if (numEnd == p) { ok = false; return 0.0; }
				p = numEnd;
				return value;
			}
		};

		bool OperatesOnCurrentValue(char c) {
			return c == '*' || c == '/' || c == '%' || c == '^';
		}

	} // namespace

	bool Evaluate(std::string_view input, double currentValue, double& outResult) {
		std::size_t start = 0;
		while (start < input.size() &&
			(input[start] == ' ' || input[start] == '\t' ||
				input[start] == '\r' || input[start] == '\n')) {
			++start;
		}
		if (start >= input.size()) return false;

		std::string expr;
		if (OperatesOnCurrentValue(input[start])) {
			// "/2" becomes "(<current>)/2". %.17g round-trips a double exactly.
			// Parenthesize the substituted value: without it, "^2" on a negative
			// current value composes "-8^2", which parses as -(8^2) because '^'
			// binds tighter than unary minus — giving -64 instead of (-8)^2 = 64.
			char currentBuf[32];
			std::snprintf(currentBuf, sizeof(currentBuf), "%.17g", currentValue);
			expr.assign("(");
			expr.append(currentBuf);
			expr.append(")");
			expr.append(input.substr(start));
		}
		else {
			expr.assign(input.substr(start));
		}

		Parser parser{ expr.c_str(), expr.c_str() + expr.size() };
		const double value = parser.ParseExpr();
		if (!parser.ok || !parser.AtEnd()) return false; // syntax error or trailing garbage
		if (!std::isfinite(value)) return false;          // div-by-zero, overflow, NaN

		outResult = value;
		return true;
	}

}
