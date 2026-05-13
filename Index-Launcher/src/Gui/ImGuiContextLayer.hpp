#pragma once

#include "Core/Layer.hpp"

namespace Index {

	// Owns the ImGui context for an Index application. Push this Layer first (before
	// any other ImGui-using Layer) so its OnPreRender / OnPostRender wrap the per-frame
	// NewFrame / Render calls around all other Layers' UI work.
	class ImGuiContextLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

	private:
		static void ApplyIndexTheme();
		bool m_IsInitialized = false;
	};

}
