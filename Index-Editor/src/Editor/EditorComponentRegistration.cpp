#include <pch.hpp>
#include "Editor/EditorComponentRegistration.hpp"

#include "Gui/ComponentInspectors.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Scene/ComponentInfo.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptComponentInspector.hpp"

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Components/Graphics/SpriteRendererComponent.hpp"
#include "Components/Graphics/ImageComponent.hpp"
#include "Components/Graphics/Camera2DComponent.hpp"
#include "Components/Graphics/ParticleSystem2DComponent.hpp"
#include "Components/Graphics/TextRendererComponent.hpp"
#include "Components/Physics/BoxCollider2DComponent.hpp"
#include "Components/Physics/CircleCollider2DComponent.hpp"
#include "Components/Physics/PolygonCollider2DComponent.hpp"
#include "Components/Physics/Rigidbody2DComponent.hpp"
#include "Components/Physics/FastBody2DComponent.hpp"
#include "Components/Physics/FastBoxCollider2DComponent.hpp"
#include "Components/Physics/FastCircleCollider2DComponent.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"

#include <span>
#include <string>
#include <typeindex>
#include <unordered_map>

namespace Index {
	namespace {
		using InspectorFn = void (*)(std::span<const Entity>);

		struct InspectorBinding {
			std::type_index type;
			InspectorFn inspector;
		};

		template<typename T>
		InspectorBinding Bind(InspectorFn inspector) {
			return InspectorBinding{ std::type_index(typeid(T)), inspector };
		}

		void AttachInspector(SceneManager& sceneManager, std::type_index type, InspectorFn inspector) {
			bool attached = false;
			sceneManager.GetComponentRegistry().ForEachComponentInfo([&](const std::type_index& id, ComponentInfo& info) {
				if (id == type) {
					info.drawInspector = inspector;
					attached = true;
				}
			});
			IDX_CORE_ASSERT(attached, IndexErrorCode::InvalidArgument,
				"AttachInspector: component type not registered. Register it in BuiltInComponentRegistration.cpp before attaching an inspector.");
		}
	}

	void DispatchComponentInspector(const ComponentInfo& info, std::span<const Entity> entities) {
		if (info.drawInspector) {
			info.drawInspector(entities);
			return;
		}
		if (!info.properties.empty()) {
			PropertyDrawer::DrawAll(entities, info.properties, info.displayName);
		}
	}

	void RegisterEditorComponentInspectors(SceneManager& sceneManager) {
		const InspectorBinding bindings[] = {
			// Hybrid: properties + extras (texture preview, runtime read-outs).
			Bind<SpriteRendererComponent>(DrawSpriteRendererInspector),
			Bind<Transform2DComponent>(DrawTransform2DInspector),
			Bind<Camera2DComponent>(DrawCamera2DInspector),
			Bind<FastBody2DComponent>(DrawFastBody2DInspector),

			Bind<ButtonComponent>(DrawButtonInspector),
			Bind<SliderComponent>(DrawSliderInspector),
			Bind<ToggleComponent>(DrawToggleInspector),
			Bind<InputFieldComponent>(DrawInputFieldInspector),
			Bind<DropdownComponent>(DrawDropdownInspector),

			// Custom-only: variant types + per-shape branches don't map cleanly.
			Bind<ParticleSystem2DComponent>(DrawParticleSystem2DInspector),

			Bind<RectTransform2DComponent>(DrawRectTransform2DInspector),

			// Custom-only: per-script field rendering goes through its own
			// PropertyDrawer-driven path (see ScriptComponentInspector.cpp).
			Bind<ScriptComponent>(DrawScriptComponentInspector),
		};

		for (const auto& b : bindings)
			AttachInspector(sceneManager, b.type, b.inspector);
	}
}
