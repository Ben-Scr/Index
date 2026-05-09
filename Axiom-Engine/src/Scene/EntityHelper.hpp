#pragma once

#include "Scene/EntityHandle.hpp"
#include "Scene/Entity.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Components/ComponentUtils.hpp"
#include "Core/Export.hpp"
#include "Core/Log.hpp"

namespace Axiom {
    class AXIOM_API EntityHelper {
    public:
        template<typename... Components>
        static Entity CreateWith(Scene& scene) {
            Entity entity = scene.GetEntity(scene.CreateEntityHandle());

            (entity.AddComponent<Components>(), ...);

            return entity;
        }

        // Info: Creates an entity with the entered Components
        template<typename... Components>
        static Entity CreateWith() {
            Scene* activeScene = SceneManager::Get().GetActiveScene();
            if (!activeScene || !activeScene->IsLoaded()) {
                AIM_ERROR_TAG("EntityHelper", "Cannot create entity because there is no active scene loaded");
                return Entity::Null;
            }

            Entity entity = activeScene->GetEntity(activeScene->CreateEntityHandle());

            (entity.AddComponent<Components>(), ...);

            return entity;
        }

        template<typename... Components>
        static EntityHandle CreateHandleWith(Scene& scene) {
            EntityHandle entity = scene.CreateEntityHandle();

            (scene.AddComponent<Components>(entity), ...);
            return entity;
        }

        template<typename... Components>
        static EntityHandle CreateHandleWith() {
            Scene* activeScene = SceneManager::Get().GetActiveScene();
            if (!activeScene || !activeScene->IsLoaded()) {
                AIM_ERROR_TAG("EntityHelper", "Cannot create entity handle because there is no active scene loaded");
                return entt::null;
            }

            EntityHandle entity = activeScene->CreateEntityHandle();

            (activeScene->AddComponent<Components>(entity), ...);
            return entity;
        }

        static void SetEnabled(Entity entity, bool enabled);
        static bool IsEnabled(Entity entity);

        // Info: Gives you back the global entities count
        static std::size_t EntitiesCount();

        static Entity CreateCamera2DEntity(Scene& scene);
        // Info: Basically calls CreateWith<Transform2D, Canera2D>();
        static Entity CreateCamera2DEntity();
        static Entity CreateSpriteEntity(Scene& scene);
        // Info: Basically calls CreateWith<Transform2D, SpriteRenderer>();
        static Entity CreateSpriteEntity();
        static Entity CreateImageEntity(Scene& scene);
        // Info: Basically calls CreateWith<RectTransform2D, Image>();
        static Entity CreateImageEntity();

        // ── UI presets ─────────────────────────────────────────────
        // Each returns the parent entity for the widget. Children
        // (label entity, slider handle, toggle checkmark) are created
        // alongside and parented via Entity::SetParent so they show up
        // nested in the editor's hierarchy panel.

        // Empty UI rect (RectTransform2D + Image as a transparent panel).
        static Entity CreateUIPanel(Scene& scene);

        // Clickable button: RectTransform2D + Image (background, tinted by
        // UIEventSystem) + Interactable + Button + a child TextRenderer
        // for the label.
        static Entity CreateUIButton(Scene& scene);

        // Horizontal slider: RectTransform2D + Image (track) + Interactable
        // + Slider, with a child Image for the handle (referenced by
        // SliderComponent::HandleEntity).
        static Entity CreateUISlider(Scene& scene);

        // Progress Bar: read-only horizontal slider. Same components as
        // CreateUISlider but with IsReadOnly = true and an additional
        // child TextRenderer the slider system fills with "{N}%" of the
        // current normalised value. No Handle child — the fill itself
        // is the only moving piece.
        static Entity CreateUIProgressBar(Scene& scene);

        // Circular Slider: ring-shaped value control. The arc is drawn
        // procedurally by GuiRenderer (no Image / texture required), the
        // hit-test is the ring annulus, and dragging follows the cursor's
        // angle around the centre. Created with a default 200×200 rect
        // and a 20 px thickness; tweak StartAngle / Sweep / Clockwise on
        // the component to make a half-ring or reverse-direction dial.
        static Entity CreateUICircularSlider(Scene& scene);

        // Single-line text field: RectTransform2D + Image (background) +
        // Interactable + InputField, with a child TextRenderer for
        // the entered text / placeholder.
        static Entity CreateUIInputField(Scene& scene);

        // Dropdown: RectTransform2D + Image + Interactable + Dropdown
        // + child TextRenderer showing the current selection.
        static Entity CreateUIDropdown(Scene& scene);

        // Toggle / checkbox: RectTransform2D + Image (box) + Interactable
        // + Toggle, with a child Image for the checkmark.
        static Entity CreateUIToggle(Scene& scene);

        // Scrollbar: RectTransform2D + Image (track) + Interactable +
        // Scrollbar, with a child Image (Handle) that owns its own
        // InteractableComponent so dragging the thumb works.
        static Entity CreateUIScrollbar(Scene& scene);

        // Scroll Rect: RectTransform2D + Image (background) +
        // Interactable + ScrollRect, with a Viewport child clipping a
        // Content child. Two scrollbars are created and parented.
        static Entity CreateUIScrollRect(Scene& scene);

        // Empty UI rect with a HorizontalLayoutGroup attached. Children
        // added under it get arranged left-to-right.
        static Entity CreateUIHorizontalLayout(Scene& scene);
        static Entity CreateUIVerticalLayout(Scene& scene);
        static Entity CreateUIGridLayout(Scene& scene);
    };

}
