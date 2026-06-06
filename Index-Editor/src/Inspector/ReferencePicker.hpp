#pragma once

#include "Assets/AssetKind.hpp"
#include "Inspector/PropertyType.hpp"

#include <optional>
#include <string>
#include <vector>


namespace Index {

	struct PropertyValue;

	namespace ReferencePicker {

		enum class EntryGroup {
			Any,
			Scene,
			Assets,
		};

		// IsSlice/SliceX/Y/W/H: for sprite-sheet sub-entries; crop the thumbnail preview to the slice rect instead of the full sheet.
		struct Entry {
			std::string Label;
			std::string Secondary;
			std::string SearchKey;
			std::string Value;
			std::string UniqueId;
			bool IsBuiltIn = false;
			bool IsSlice = false;
			int SliceX = 0;
			int SliceY = 0;
			int SliceW = 0;
			int SliceH = 0;
			EntryGroup Group = EntryGroup::Any;
		};

		std::vector<Entry> CollectAssetsByKind(AssetKind kind);
		std::vector<Entry> CollectEntities(bool includePrefabAssets = true); // loaded scene entities, optionally Prefab assets
		std::vector<Entry> CollectComponentTargets(const std::string& componentDisplayName);

		enum class Style { Plain, Thumbnails };

		// Request that the picker window open this frame for `fieldKey`.
		// `entries` are owned by the picker until the window closes.
		void OpenForFieldKey(const std::string& fieldKey, const std::string& title,
			std::vector<Entry> entries, Style style = Style::Plain, bool useEntityTabs = false);

		// Safe to call exactly once per frame; idempotent for multiple requestors.
		void RenderPopup();

		std::optional<std::string> ConsumeSelection(const std::string& fieldKey);

		bool DrawReferenceField(const char* label, const std::string& displayValue,
			const std::string& secondary, bool missing, bool mixed, bool& outHovered,
			float reservedTrailingWidth = 0.0f);

		std::string ResolveAssetDisplay(uint64_t assetId, AssetKind expectedKind,
			bool& outMissing, std::string* outSecondary);
		std::string ResolvePrefabDisplay(uint64_t prefabId, bool& outMissing,
			std::string* outSecondary);
		std::string ResolveEntityDisplay(uint64_t entityId, bool& outMissing,
			std::string* outSecondary);
		std::string ResolveComponentRefDisplay(uint64_t entityId,
			const std::string& componentTypeName, bool& outMissing,
			std::string* outSecondary);

		// MUST call on reload/detach: clears picker state (cache, search, eye-toggle) that otherwise survives project switches with stale AssetRegistry pointers.
		void Shutdown();

	} // namespace ReferencePicker

} // namespace Index
