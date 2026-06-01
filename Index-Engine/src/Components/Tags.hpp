#pragma once

namespace Index {
	struct IdTag {};
	struct StaticTag {};
	// WARNING: cascade hooks mutate child pools during construction/destruction; snapshot view<DisabledTag> to a vector before toggling a parent during iteration.
	struct DisabledTag {};
	struct InheritedDisabledTag {};
	struct DeadlyTag{};
}
