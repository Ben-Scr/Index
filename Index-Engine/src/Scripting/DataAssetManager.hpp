#pragma once

#include "Core/Export.hpp"

#include <cstdint>
#include <string>

namespace Index {

	// Owns the set of loaded data-asset GUIDs and drives their managed instances
	// (create + field replay) through ScriptEngine. The .dataasset file on disk is
	// the source of truth; the managed side holds the live object — cleared when the
	// script assembly load context unloads, re-hydrated here by ReloadAll().
	// Scripting-side, not core: a scripting-stripped build never references it.
	class INDEX_API DataAssetManager {
	public:
		DataAssetManager() = delete;

		// Ensure the data asset for `guid` has a live managed instance. Idempotent —
		// preserves an already-loaded instance's in-memory edits. Returns true if an
		// instance exists afterwards (false if the file/type is missing).
		static bool Load(uint64_t guid);

		// Drop a single asset's managed instance and native tracking.
		static void Unload(uint64_t guid);

		// Forget every loaded asset (managed instances are dropped separately when the
		// assembly load context unloads).
		static void DropAll();

		// Re-instantiate every tracked asset from disk after a hot-reload swap cleared
		// the managed instances. Creates all instances before replaying any fields so
		// cross-asset references resolve.
		static void ReloadAll();

		// Editor inspector pass-throughs. GetFields ensures the asset is loaded first
		// and returns the inspector field-array JSON (valid until the next field call).
		static const char* GetFields(uint64_t guid);
		static void SetField(uint64_t guid, const std::string& fieldName, const std::string& value);

		// Persist the asset's current field values to its .dataasset file. Returns false on I/O failure.
		static bool Save(uint64_t guid);

		// Create a new .dataasset of `typeName` at `path` (fields default to the class's
		// initializers) and register it. Returns the new asset GUID, or 0 on failure.
		static uint64_t Create(const std::string& path, const std::string& typeName);

		// The C# type name backing a loaded asset; empty if not loaded.
		static std::string GetTypeName(uint64_t guid);
	};

}
