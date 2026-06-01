#pragma once

#include "Collections/Color.hpp"
#include "Core/Export.hpp"
#include "Graphics/RenderApiTypes.hpp"

#include <string>
#include <string_view>


namespace Index {

	struct GLInitSpecifications;
	class Framebuffer;

	class INDEX_API RenderApi {
	public:
		RenderApi() = delete;

		static bool Init(const GLInitSpecifications& spec);

		// Tear down any backend-owned global state. Resource wrappers handle
		// their own cleanup; this handles backend-init mirror image.
		static void Shutdown();

		static void Present();

		static bool IsInitialized();

		// Identifier of the active backend ("WebGPU", etc.) — shown in
		// the editor's About / Stats overlay.
		static std::string_view BackendName();

		// Cached GPU info (filled in by Init). Empty strings if Init never
		// succeeded. The build / about UI surfaces these.
		static const std::string& GetVersionString();
		static const std::string& GetVendorString();
		static const std::string& GetRendererString();


		static void Clear(ClearFlags flags);
		static void SetClearColor(const Color& color);
		static Color GetClearColor();

		static void SetViewport(int x, int y, int width, int height);
		static void SetScissor(int x, int y, int width, int height);

		// Triggers swap-chain reset; distinct from SetViewport which also fires for FBO binds.
		static void OnWindowResize(int width, int height);
		static void SetVsync(bool enabled);
		static void EnableScissorTest();
		static void DisableScissorTest();

		static void EnableDepthTest();
		static void DisableDepthTest();

		static void SetCullMode(CullMode mode);

		static void SetBlendMode(BlendMode mode);
		static void SetBlendingEnabled(bool enabled);

		static void SetPolygonMode(PolygonMode mode);
		static PolygonMode GetPolygonMode();
		static void SetLineWidth(float width);

		static void SetColorMask(bool r, bool g, bool b, bool a);

		// ── Framebuffer binding ──────────────────────────────────────
		// Bind one of the engine's Framebuffer wrappers as the current
		// render target, or restore the window's default framebuffer.
		static void BindFramebuffer(const Framebuffer& fbo);
		static void BindDefaultFramebuffer();
	};

} // namespace Index
