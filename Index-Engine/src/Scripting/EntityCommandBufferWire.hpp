#pragma once

#include <cstdint>

// ECB wire format: shared by managed recorder and native playback (ScriptBindingsEcb.cpp). Little-endian byte-packed.

namespace Index {

	struct EcbHeader {
		uint32_t entityCount;
		uint32_t commandCount;
	};
	static_assert(sizeof(EcbHeader) == 8, "ECB header layout drift would corrupt the managed/native binding");

	enum EcbOpcode : uint8_t {
		Ecb_AddComponent = 1,
		// Set on an existing component (replaces stored value). Same payload
		// as AddComponent but skips dependency-add logic on the assumption
		// the component is already present. Reserved for future use.
		Ecb_SetComponent = 2,

		// entityIndex slot becomes the root; 11-byte prefix matches AddComponent so the remap walker needs no per-opcode branching.
		Ecb_InstantiatePrefab = 3,

		// No payload; fires C++ member-initializers via defaultEmplace (use instead of AddComponent to avoid zero-init overwriting engine defaults).
		Ecb_DefaultConstructComponent = 4,

		// Local ECB root: payloadSize 0 and entityIndex names the root slot.
		// Live scene entity: entityIndex is kEcbNoName and payload is uint64 persistent id.
		Ecb_DestroyEntity = 5,

		// Enable/disable an entity. Local ECB root: entityIndex names the root slot, payload is u8 enabled.
		// Live scene entity: entityIndex is kEcbNoName, payload is u8 enabled + uint64 persistent id (9 bytes).
		// Routes to Entity::SetEnabled at playback, so the DisabledTag cascade and OnEnable/OnDisable fire.
		Ecb_SetEntityEnabled = 6,
	};

	// Sentinel "no name" in the entity table.
	constexpr uint32_t kEcbNoName = 0xFFFFFFFFu;

	// Negative return codes from Ecb_Playback. Positive returns are the
	// number of entities created.
	constexpr int kEcbErrorTruncated = -1;
	constexpr int kEcbErrorNoScene   = -2;
	constexpr int kEcbErrorOutputTooSmall = -3;
	constexpr int kEcbErrorUnknownComponent = -4;

	constexpr int kEcbErrorEntityCapExceeded = -5;

	constexpr int kEcbErrorBadPrefab = -6;

	// An exception escaped playback (e.g. std::bad_alloc during prefab bake). Caught at the
	// reverse-pinvoke boundary because unwinding into CoreCLR is UB; surfaced as an error code.
	constexpr int kEcbErrorInternal = -7;

	// Ecb_InstantiatePrefab referenced a GUID that doesn't resolve to a loadable Prefab asset
	// (vs kEcbErrorBadPrefab, where the prefab WAS found but its components can't be baked).
	constexpr int kEcbErrorUnknownPrefab = -8;

} // namespace Index
