#pragma once
#include "Scene/Entity.hpp"

#include <span>

namespace Index {

	// Hybrid inspectors: call PropertyDrawer::DrawAll first, then append extras (texture preview, runtime read-outs) that don't fit the declarative property model.
	void DrawSpriteRendererInspector(std::span<const Entity> entities);
	void DrawTransform2DInspector(std::span<const Entity> entities);
	void DrawCamera2DInspector(std::span<const Entity> entities);
	void DrawFastBody2DInspector(std::span<const Entity> entities);

	// Hybrid inspector: declarative particle fields plus a texture preview.
	void DrawParticleSystem2DInspector(std::span<const Entity> entities);

	void DrawRectTransform2DInspector(std::span<const Entity> entities);

	void DrawButtonInspector(std::span<const Entity> entities);
	void DrawSliderInspector(std::span<const Entity> entities);
	void DrawToggleInspector(std::span<const Entity> entities);
	void DrawInputFieldInspector(std::span<const Entity> entities);
	void DrawDropdownInspector(std::span<const Entity> entities);

} // namespace Index
