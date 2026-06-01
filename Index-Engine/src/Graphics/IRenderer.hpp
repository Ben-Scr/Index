#pragma once

#include "Core/Export.hpp"

namespace Index {

	// Swappable rendering boundary; install an alternate impl before Application::Initialize. Currently only lifecycle+frame methods; concrete systems still use Renderer2D* directly.
	class INDEX_API IRenderer {
	public:
		virtual ~IRenderer() = default;

		virtual void Initialize() = 0;
		virtual void Shutdown() = 0;

		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;

		virtual bool IsInitialized() const = 0;
	};

}
