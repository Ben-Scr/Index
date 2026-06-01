#pragma once

#include "Collections/Color.hpp"
#include "Core/Export.hpp"
#include "Graphics/GLInitSpecifications.hpp"

// DEPRECATED: thin shim over `RenderApi`. New code should use `RenderApi` directly.

namespace Index {

	// DEPRECATED: bridges historical OpenGL:: surface to RenderApi; scheduled for deletion after one release cycle.
	class INDEX_API OpenGL {
	public:
		OpenGL() = delete;

		static bool Initialize(const GLInitSpecifications& glInitSpecs);
		static bool IsInitialized();

		static void SetClearColor(const Color& clearColor);
		static Color GetClearColor();

		static const std::string& GetVersionString();
		static const std::string& GetVendorString();
		static const std::string& GetRendererString();
	};

} // namespace Index
