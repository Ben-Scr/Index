#pragma once

#include <cstdint>

// Backend-neutral rendering enums. MUST NOT include backend headers — translation happens in Backend/WebGPUApi.cpp.

namespace Index {

	enum class PolygonMode : uint8_t {
		Filled = 0,
		Wireframe,
	};

	enum class CullMode : uint8_t {
		None = 0,
		Back,
		Front,
		FrontAndBack,
	};

	enum class BlendMode : uint8_t {
		Disabled = 0,
		Alpha,           // src = SRC_ALPHA, dst = ONE_MINUS_SRC_ALPHA
		Premultiplied,   // src = ONE,       dst = ONE_MINUS_SRC_ALPHA
		Additive,        // src = SRC_ALPHA, dst = ONE
		Opaque,          // src = ONE,       dst = ZERO
	};

	enum class ClearFlags : uint32_t {
		None  = 0,
		Color = 1u << 0,
		Depth = 1u << 1,
		Stencil = 1u << 2,
	};

	constexpr ClearFlags operator|(ClearFlags a, ClearFlags b) noexcept {
		return static_cast<ClearFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}
	constexpr ClearFlags operator&(ClearFlags a, ClearFlags b) noexcept {
		return static_cast<ClearFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}
	constexpr bool HasFlag(ClearFlags value, ClearFlags flag) noexcept {
		return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
	}

	enum class TextureFormat : uint8_t {
		RGBA8 = 0,
		R8,
		Depth24Stencil8,
		RGBA16F,
	};

	enum class TextureFilter : uint8_t {
		Nearest = 0,
		Linear,
	};

} // namespace Index
