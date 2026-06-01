#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Index {

	// ArgumentValue wire format: Bool="0"/"1", Int=decimal, Float/Double="1.5", String=raw, Vec2="x,y", Color="r,g,b,a", EntityRef=decimal uint64.
	enum class InspectorEventArgKind : uint8_t {
		Void = 0,
		Bool = 1,
		Int = 2,
		Float = 3,
		Double = 4,
		String = 5,
		Vec2 = 6,
		Color = 7,
		EntityRef = 8,
	};

	struct InspectorEventBinding {
		uint64_t TargetEntityUUID = 0;
		std::string ScriptClassName;
		std::string MethodName;
		bool Enabled = true;

		// Typed static argument. `ArgumentKind` matches the C# method's
		// first parameter type; `ArgumentValue` is the encoded string the
		// dispatcher hands to the C# side. `Void` ⇒ argument unused.
		InspectorEventArgKind ArgumentKind = InspectorEventArgKind::Void;
		std::string ArgumentValue;
	};

	struct InspectorEventList {
		std::vector<InspectorEventBinding> Bindings;
	};

}
