#pragma once
#include "Scripting/ScriptGlue.hpp"

namespace Index {
	class ScriptBindings {
	public:
		static void PopulateNativeBindings(NativeBindings& bindings);
		static bool IsScriptInputEnabled();
	};

}
