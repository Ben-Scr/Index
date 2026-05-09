#pragma once
#include "Collections/Color.hpp"
#include "Core/Export.hpp"
#include "Graphics/GLInitSpecifications.hpp"

#include <glad/glad.h>

namespace Axiom {
	class AXIOM_API OpenGL {
	public:
		OpenGL() = delete;

		static bool Initialize(const GLInitSpecifications& glInitSpecs);
		static bool IsInitialized() { return s_IsInitialized; }
		static void BlendFunc(GLenum sFactor, GLenum dFactor);
		static void Enable(GLenum glEnum);
		static void Disable(GLenum glEnum);
		static void CullFace(GLCullingMode cullingMode);
		static void SetClearColor(const Color& clearColor);
		static Color GetClearColor();

		// GPU caps (cached after Initialize). Empty strings before init.
		static const std::string& GetVersionString();
		static const std::string& GetVendorString();
		static const std::string& GetRendererString();
	private:
		static bool s_IsInitialized;
		static std::string s_VersionString;
		static std::string s_VendorString;
		static std::string s_RendererString;
	};
}
