#pragma once

#include "Core/Export.hpp"
#include "Components/Tags.hpp"

namespace Index {

	// General
	class INDEX_API Transform2DComponent;
	struct RectTransform2DComponent;
	struct NameComponent;
	struct UUIDComponent;
	struct EntityMetaDataComponent;
	struct PrefabInstanceComponent;
	struct HierarchyComponent;

	// Graphics
	struct INDEX_API SpriteRendererComponent;
	struct ImageComponent;
	class INDEX_API Camera2DComponent;
	class INDEX_API ParticleSystem2DComponent;
	struct INDEX_API PostProcessing2DComponent;
	struct INDEX_API TextRendererComponent;

	// Physics
	class INDEX_API BoxCollider2DComponent;
	class INDEX_API CircleCollider2DComponent;
	class INDEX_API PolygonCollider2DComponent;
	class INDEX_API Rigidbody2DComponent;
	struct INDEX_API FastBody2DComponent;
	struct INDEX_API FastBoxCollider2DComponent;
	struct INDEX_API FastCircleCollider2DComponent;

	// Audio
	class INDEX_API AudioSourceComponent;

	// Scripting
	struct ScriptComponent;

	// UI
	struct InteractableComponent;
	struct ButtonComponent;
	struct SliderComponent;
	struct InputFieldComponent;
	struct DropdownComponent;
	struct ToggleComponent;
	struct ScrollbarComponent;
	struct ScrollRectComponent;
	struct MaskComponent;
	struct ContentSizeFitterComponent;
	struct WidthConstraintComponent;
	struct CircularSliderComponent;
	struct HorizontalLayoutGroupComponent;
	struct VerticalLayoutGroupComponent;
	struct GridLayoutGroupComponent;

}
