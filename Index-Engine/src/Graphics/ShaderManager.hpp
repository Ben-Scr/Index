#pragma once
#include "Shader.hpp"
#include <memory>
#include <stdexcept>

// Stub — ShaderManager is currently unused anywhere in the codebase. Kept
// declared here so the (eventual) caller compiles, but every entry point
// throws so the missing implementation surfaces loudly on first call rather
// than silently returning a moved-from `unique_ptr` (UB) like the previous
// empty-body version.
namespace Index {
	class ShaderManager {
	public:
		std::unique_ptr<Shader> LoadShader(const std::string_view& /*vert*/, const std::string_view& /*frag*/) {
			throw std::logic_error("ShaderManager::LoadShader is not implemented");
		}
	};
}