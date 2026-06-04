#pragma once
#include "Core/Export.hpp"
#include <string_view>

namespace Index::ExpressionEvaluator {

	// Evaluates an arithmetic expression to a double: + - * / % ^ (power,
	// right-assoc), parentheses, unary +/-, and decimal/scientific literals.
	// Whitespace is ignored.
	//
	// A leading *, /, %, or ^ operates on currentValue ("/2" -> currentValue / 2),
	// matching the inspector's "edit the current value" idiom. A leading + or - is
	// a literal sign ("-5" stays -5, not currentValue - 5) so negative-number entry
	// stays intuitive.
	//
	// Returns false and leaves outResult untouched on parse error, empty input, or a
	// non-finite result (division by zero, overflow). Math is done in double, so
	// callers targeting integer fields should round the result themselves.
	INDEX_API bool Evaluate(std::string_view input, double currentValue, double& outResult);

}
