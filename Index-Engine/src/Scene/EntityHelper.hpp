#pragma once

#include "Scene/EntityHandle.hpp"
#include "Scene/Entity.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Components/ComponentUtils.hpp"
#include "Core/Export.hpp"
#include "Core/Log.hpp"

namespace Index {
    class INDEX_API EntityHelper {
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
                IDX_ERROR_TAG("EntityHelper", "Cannot create entity because there is no active scene loaded");
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
                IDX_ERROR_TAG("EntityHelper", "Cannot create entity handle because there is no active scene loaded");
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

        static Entity CreateUIPanel(Scene& scene);
        static Entity CreateUIButton(Scene& scene);
        static Entity CreateUISlider(Scene& scene);
        static Entity CreateUIProgressBar(Scene& scene);
        static Entity CreateUICircularSlider(Scene& scene);
        static Entity CreateUIInputField(Scene& scene);
        static Entity CreateUIDropdown(Scene& scene);
        static Entity CreateUIToggle(Scene& scene);
        static Entity CreateUIScrollbar(Scene& scene);
        static Entity CreateUIScrollRect(Scene& scene);
        static Entity CreateUIHorizontalLayout(Scene& scene);
        static Entity CreateUIVerticalLayout(Scene& scene);
        static Entity CreateUIGridLayout(Scene& scene);
    };

}
