#pragma once

#include "Core/Layer.hpp"
#ifdef INDEX_PROFILER_ENABLED
#include "Gui/ProfilerPanel.hpp"
#endif

namespace Index {

	// In-game profiler panel (profiler.enableInRuntime=true). Ctrl+F6 toggles visibility.
	// Stays compiled under --no-profiler so RuntimeApplication.cpp needs no #ifdef around the layer push.
	class RuntimeProfilerLayer : public Layer {
	public:
		using Layer::Layer;

		void OnAttach(Application& app) override;
		void OnDetach(Application& app) override;
		void OnPreRender(Application& app) override;
		void OnPostRender(Application& app) override;
		void OnUpdate(Application& app, float dt) override;

	private:
		bool m_ShowPanel = false;
		bool m_ImGuiInitialized = false;
#ifdef INDEX_PROFILER_ENABLED
		ProfilerPanel m_Panel;
#endif
	};

} // namespace Index
