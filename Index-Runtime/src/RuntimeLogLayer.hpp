#pragma once

#include "Core/Layer.hpp"
#include "Diagnostics/LogOverlay.hpp"

namespace Index {

	class RuntimeStatsLayer; // for stacking-offset query

	class RuntimeLogLayer : public Layer {
	public:
		using Layer::Layer; // inherit explicit Layer(name)

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnUpdate(Application& app, float dt) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;

		bool IsVisible() const { return m_Show; }
		void SetVisible(bool v) { m_Show = v; }

	private:
		bool m_Show = false;
		bool m_ImGuiAcquired = false;
		// unique_ptr delays LogOverlay construction (and its Log::OnLog subscription) until OnAttach, not at layer-push time.
		std::unique_ptr<Diagnostics::LogOverlay> m_Overlay;
	};

} // namespace Index
