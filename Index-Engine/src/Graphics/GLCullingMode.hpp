#pragma once

#include <cstdint>

namespace Index {

	// Restricted to the three values glCullFace accepts; previous enum included draw/read-buffer constants that silently produced GL_INVALID_ENUM.
	enum class GLCullingMode : uint32_t {
		None             = 0,
		Front            = 0x0404,    // GL_FRONT
		Back             = 0x0405,    // GL_BACK
		FrontAndBack     = 0x0408,    // GL_FRONT_AND_BACK
	};

} // namespace Index
