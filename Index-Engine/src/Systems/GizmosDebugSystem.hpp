#pragma once
#include "Core/Layer.hpp"
#include "Core/Export.hpp"

namespace Index {
	// Layer (not ISystem): registered as editor overlay so serialized scenes don't break on class lookup; do NOT convert to ISystem — Layers get overlay ordering that scene-bound systems cannot.
	class INDEX_API GizmosDebugSystem : public Layer {
	public:
		using Layer::Layer;

		void OnUpdate(Application& app, float dt) override;
	};
}
