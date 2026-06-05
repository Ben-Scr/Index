#pragma once
#include "Components/General/Transform2DComponent.hpp"
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"
#include "Scene/EntityHandle.hpp"
#include <cstddef>
#include <variant>
#include <span>
#include <vector>

namespace Index {
	class Scene;

	class INDEX_API ParticleSystem2DComponent {
		friend class Scene;
		friend class Renderer2D;
		friend class ParticleUpdateSystem;

	public:

		struct Particle {
			Transform2DComponent Transform;
			Color Color;
			float LifeTime;
			Vec2 Velocity;
		};

		enum class Space {
			Local,
			World
		};

		struct Burst {
			uint32_t Count{ 10 };
			float Interval{ 1.f };
			float TimeUntilNext{ 0.f };
		};

		struct RenderingSettings {
			uint32_t MaxParticles{ 1000 };
			Color Color{ Color::White() };
			short SortingOrder{ 0 };
			uint8_t SortingLayer{ 0 };
		};

		struct ParticleSettings {
			float LifeTime{ 1.f };
			float Speed{ 5.f };
			Vec2 Gravity{ 0.0f, -9.7f };
			bool UseGravity{ false };
			bool UseRandomColors{ false };
			Vec2 Scale{ 1.f, 1.f };
			Vec2 MoveDirection{ 0.f, 0.f };
		};

		struct EmissionSettings {
			uint16_t EmitOverTime{ 10 };
			uint16_t RateOverDistance{ 0 };
			Space EmissionSpace{ Space::World };
		};



		struct CircleParams { float Radius = 1.f; bool IsOnCircle = false; };
		struct SquareParams { Vec2 HalfExtends{ 1.f,1.f }; };
		struct EdgeParams { float Length = 1.f; };

		struct RandomColorParams { Color From{ 1.f,1.f,1.f,1.f }; Color To{ 1.f,1.f,1.f,0.f }; };
		struct RandomScaleParams { Vec2 From{ 1.f,1.f }; Vec2 To{ 1.f,1.f }; };
		struct RandomRotationParams { float From{ 0.f }; float To{ 0.f }; };

		enum class ShapeType {
			Circle,
			Square,
			Edge
		};
		using ShapeParams = std::variant<CircleParams, SquareParams, EdgeParams>;


		ParticleSystem2DComponent() = default;
		void SetTexture(const TextureHandle& texture, UUID assetId = UUID(0)) { m_TextureHandle = texture; m_TextureAssetId = assetId; }
		const TextureHandle& GetTextureHandle() const { return m_TextureHandle; }
		UUID GetTextureAssetId() const { return m_TextureAssetId; }
		void Emit(size_t count);
		void AddBurst(const Burst& burst) { m_Bursts.push_back(burst); }
		void RemoveBurst(size_t index) { if (index < m_Bursts.size()) m_Bursts.erase(m_Bursts.begin() + static_cast<std::ptrdiff_t>(index)); }
		void ClearBursts() { m_Bursts.clear(); }
		std::span<Burst> GetBursts() noexcept { return m_Bursts; }
		std::span<const Burst> GetBursts() const noexcept { return m_Bursts; }
		std::span<const Particle> GetParticles() const noexcept { return m_Particles; }
		bool IsPlaying() const { return m_IsEmitting; }
		Transform2DComponent& GetTransform2D();
		const Transform2DComponent& GetTransform2D() const;
		void PreviewUpdate(float deltaTime) { Update(deltaTime); }

		// Info: Enables both emitting and simulating
		void Play() { m_IsEmitting = true; m_IsSimulating = true; }

		// Info: Disables both emitting and simulating
		void Pause() { m_IsEmitting = false; m_IsSimulating = false; }

		// Info: Stops the preview/runtime simulation and clears live particles.
		void Stop() { m_IsEmitting = false; m_IsSimulating = false; ResetSimulation(); }

		void Restart() { ResetSimulation(); Play(); }

		// Single source of the "play on awake" rule, shared by scene-start (systems / editor) and runtime prefab spawns.
		void PlayOnAwakeIfEnabled() { if (PlayOnAwake) Play(); }

		void SetIsSimulating(bool enabled) { m_IsSimulating = enabled; }
		void SetIsEmitting(bool enabled) { m_IsEmitting = enabled; }

		bool IsEmitting() const { return m_IsEmitting; }
		bool IsSimulating() const { return m_IsSimulating; }

		void Clear() { m_Particles.clear(); m_Bursts.clear();  m_EmitAccumulator = 0.f; }

		// MUST call after value-copying a component: assign-copy bypasses on_construct, so the destination inherits the source's stale scene/entity pointers.
		void RebindEmitter(Scene* scene, EntityHandle entity) {
			m_EmitterScene = scene;
			m_EmitterEntity = entity;
		}

		bool PlayOnAwake{ true };
		ParticleSettings ParticleSettings;
		EmissionSettings EmissionSettings;
		ShapeParams Shape = CircleParams{ 1.f };
		RenderingSettings RenderingSettings;

	private:
		// E20: explicit-dt overload — preferred path, called by ParticleUpdateSystem
		// once per frame. Original parameterless Update() forwards to it using the
		// global Application time as a fallback.
		void Update();
		void Update(float deltaTime);
		const Transform2DComponent* TryGetEmitterTransform() const;
		void ResetSimulation();
		std::vector<Particle> m_Particles;
		std::vector<Burst> m_Bursts;

		// EnTT requires copyability for snapshot/restore; m_EmitterScene/m_EmitterEntity must be rebound after copy via RebindEmitter.
		Scene* m_EmitterScene{ nullptr };
		EntityHandle m_EmitterEntity{ entt::null };
		float m_EmitAccumulator{ 0.0f };
		bool m_IsEmitting{ false };
		bool m_IsSimulating{ false };
		TextureHandle m_TextureHandle;
		UUID m_TextureAssetId{ 0 };
	};
}
