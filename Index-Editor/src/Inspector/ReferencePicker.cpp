#include "pch.hpp"
#include "Inspector/ReferencePicker.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Assets/TextureMeta.hpp"
#include "Components/General/NameComponent.hpp"
#include "Graphics/Text/FontHandle.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/AssetType.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/Icons.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Gui/ThumbnailCache.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Serialization/Path.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace Index::ReferencePicker {

	namespace {

		struct PickerState {
			bool IsOpen = false;
			bool RequestOpen = false;
			char Search[128] = {};
			std::string Title;
			std::string TargetFieldKey;
			std::vector<Entry> Entries;
			std::string PendingFieldKey;
			std::string PendingValue;
			Style Style = Style::Plain;
			bool UseEntityTabs = false;
			EntryGroup ActiveEntityTab = EntryGroup::Scene;
			std::vector<std::size_t> VisibleEntryIndices;
			std::string LastFilter;
			bool LastIncludeBuiltIns = true;
			EntryGroup LastEntityTab = EntryGroup::Any;
			bool VisibleEntriesDirty = true;

			bool IncludeBuiltIns = true;

			ThumbnailCache Thumbnails;
			bool ThumbnailsInitialized = false;
			std::unordered_set<std::string> LoadedThumbnailPaths;

			// Tracks the last frame RenderPopup rendered; subsequent calls in the same frame return early to avoid duplicate ImGui window IDs.
			int LastRenderedFrame = -1;

			// Deferred discard: inline release is unsafe because the current frame's draw list still references those texture pointers.
			bool DiscardPending = false;
		};

		PickerState s_State;

		// Module-level (not local static) so Shutdown() can reset it on reload — a local static would stick true and skip the re-walk.
		bool s_BuiltInsDone = false;

		struct AssetEntryCache {
			uint64_t RegistryVersion = 0;
			std::vector<Entry> Entries;
		};
		std::unordered_map<int, AssetEntryCache> s_AssetEntryCache;

		int MakeAssetEntryCacheKey(AssetKind kind, bool includeSlices) {
			return (static_cast<int>(kind) << 1) | (includeSlices ? 1 : 0);
		}

		void EnsureThumbnailCacheInitialized() {
			if (!s_State.ThumbnailsInitialized) {
				s_State.Thumbnails.Initialize();
				s_State.ThumbnailsInitialized = true;
			}
		}

		void DiscardThumbnails() {
			if (s_State.ThumbnailsInitialized) {
				s_State.Thumbnails.Clear();
			}
			s_State.LoadedThumbnailPaths.clear();
		}

		std::string ToLowerCopy(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return value;
		}

		void RebuildVisibleEntriesIfNeeded() {
			const std::string filter = ToLowerCopy(std::string(s_State.Search));
			if (!s_State.VisibleEntriesDirty
				&& filter == s_State.LastFilter
				&& s_State.IncludeBuiltIns == s_State.LastIncludeBuiltIns
				&& s_State.ActiveEntityTab == s_State.LastEntityTab) {
				return;
			}

			s_State.VisibleEntryIndices.clear();
			s_State.VisibleEntryIndices.reserve(s_State.Entries.size());
			for (std::size_t i = 0; i < s_State.Entries.size(); ++i) {
				const Entry& entry = s_State.Entries[i];
				if (s_State.UseEntityTabs
					&& entry.Group != EntryGroup::Any
					&& entry.Group != s_State.ActiveEntityTab) {
					continue;
				}
				if (!s_State.IncludeBuiltIns && entry.IsBuiltIn) continue;
				if (!filter.empty() && entry.SearchKey.find(filter) == std::string::npos) continue;
				s_State.VisibleEntryIndices.push_back(i);
			}

			s_State.LastFilter = filter;
			s_State.LastIncludeBuiltIns = s_State.IncludeBuiltIns;
			s_State.LastEntityTab = s_State.ActiveEntityTab;
			s_State.VisibleEntriesDirty = false;
		}

		std::string GetEntityName(const Scene& scene, EntityHandle handle, uint64_t entityId) {
			if (scene.HasComponent<NameComponent>(handle)) {
				const std::string& name = scene.GetComponent<NameComponent>(handle).Name;
				if (!name.empty()) return name;
			}
			return "Entity " + std::to_string(entityId);
		}

		const char* GetAssetKindDisplayName(AssetKind kind) {
			switch (kind) {
				case AssetKind::Texture: return "Texture";
				case AssetKind::Audio: return "Audio";
				case AssetKind::Scene: return "Scene";
				case AssetKind::Prefab: return "Prefab";
				case AssetKind::Script: return "Script";
				case AssetKind::Font: return "Font";
				case AssetKind::Shader: return "Shader";
				case AssetKind::DataAsset: return "Data Asset";
				case AssetKind::Unknown:
				case AssetKind::Other:
				default: return "Asset";
			}
		}

		std::string GetNoneAssetLabel(AssetKind kind) {
			return "None (" + std::string(GetAssetKindDisplayName(kind)) + ")";
		}

		// AssetRegistry is header-only with inline statics, so the editor EXE has its own copy that the engine DLL never populates; this re-registers built-ins into the EXE's copy.
		// TODO: export AssetRegistry properly (INDEX_API) to remove this duplication.
		void EnsureBuiltInsRegisteredInEditor() {
			if (s_BuiltInsDone) return;
			s_BuiltInsDone = true;

			// Default font with hand-picked GUID (matches FontManager's own
			// registration so scenes referencing k_DefaultFontAssetId resolve).
			const std::string fontDir = Path::ResolveIndexAssets("Fonts");
			if (!fontDir.empty()) {
				const std::string fontPath = Path::Combine(fontDir, "GoogleSans-Regular.ttf");
				if (std::filesystem::exists(fontPath)) {
					std::error_code ec;
					std::string canonical = std::filesystem::weakly_canonical(
						std::filesystem::path(fontPath), ec).make_preferred().string();
					if (ec) canonical = fontPath;
					AssetRegistry::RegisterBuiltInAsset(canonical, k_DefaultFontAssetId, AssetKind::Font);
				}
			}

			// Recursive scan of IndexAssets/ for icons, audio, shaders, etc.
			const std::string indexAssetsRoot = Path::ResolveIndexAssets("");
			if (!indexAssetsRoot.empty()) {
				AssetRegistry::RegisterBuiltInDirectory(indexAssetsRoot);
			}
		}

		const ComponentInfo* FindComponentByDisplayName(const std::string& displayName) {
			const ComponentInfo* found = nullptr;
			SceneManager::Get().GetComponentRegistry().ForEachComponentInfo(
				[&](const std::type_index&, const ComponentInfo& info) {
					if (!found && info.category == ComponentCategory::Component && info.displayName == displayName) {
						found = &info;
					}
				});
			return found;
		}

	} // namespace

	std::vector<Entry> CollectAssetsByKind(AssetKind kind, bool includeSlices) {
		EnsureBuiltInsRegisteredInEditor();

		const int cacheKey = MakeAssetEntryCacheKey(kind, includeSlices);
		const uint64_t registryVersion = AssetRegistry::GetChangeVersion();
		if (auto cacheIt = s_AssetEntryCache.find(cacheKey); cacheIt != s_AssetEntryCache.end()) {
			if (cacheIt->second.RegistryVersion == registryVersion
				|| (AssetRegistry::IsDirty() && !cacheIt->second.Entries.empty())) {
				return cacheIt->second.Entries;
			}
		}

		AssetRegistry::Sync();

		// Filter out IndexAssets/Textures/Editor entries (editor chrome icons) from all asset-kind listings.
		auto isEditorIconPath = [](const std::string& path) {
			auto contains = [&path](std::string_view needle) {
				return path.find(needle) != std::string::npos;
			};
			return contains("IndexAssets/Textures/Editor")
				|| contains("IndexAssets\\Textures\\Editor");
		};

		std::vector<Entry> entries;
		entries.push_back({ GetNoneAssetLabel(kind), "", "(none)", "", "__none__", false });
		const auto records = AssetRegistry::GetAssetsByKind(kind);
#ifndef NDEBUG
		if (records.empty()) {
			static bool s_Warned = false;
			if (!s_Warned) {
				IDX_CORE_WARN_TAG("ReferencePicker",
					"GetAssetsByKind returned empty - did EnsureBuiltInsRegisteredInEditor run?");
				s_Warned = true;
			}
		}
#endif
		for (const AssetRegistry::Record& record : records) {
			if (isEditorIconPath(record.Path)) continue;
			Entry entry;
			entry.Label = std::filesystem::path(record.Path).filename().string();
			entry.Secondary = record.Path;
			entry.SearchKey = ToLowerCopy(entry.Label + " " + entry.Secondary);
			entry.Value = std::to_string(record.Id);
			entry.UniqueId = entry.Value;
			entry.IsBuiltIn = AssetRegistry::IsBuiltIn(record.Id);
			entries.push_back(std::move(entry));

			if (kind != AssetKind::Texture || !includeSlices) continue;
			TextureMeta meta = AssetRegistry::ReadTextureMeta(record.Path);
			if (meta.Sprites.empty()) continue;
			const std::string parentName = std::filesystem::path(record.Path).filename().string();
			for (const SpriteSlice& slice : meta.Sprites) {
				Entry sliceEntry;
				sliceEntry.Label = parentName + " > " + slice.Name;
				sliceEntry.Secondary = record.Path;
				sliceEntry.SearchKey = ToLowerCopy(sliceEntry.Label + " " + record.Path);
				sliceEntry.Value = std::to_string(record.Id) + "|slice|" + slice.Name;
				sliceEntry.UniqueId = "slice:" + std::to_string(record.Id) + ":" + slice.Name;
				sliceEntry.IsBuiltIn = AssetRegistry::IsBuiltIn(record.Id);
				sliceEntry.IsSlice = true;
				sliceEntry.SliceX = slice.X;
				sliceEntry.SliceY = slice.Y;
				sliceEntry.SliceW = slice.W;
				sliceEntry.SliceH = slice.H;
				entries.push_back(std::move(sliceEntry));
			}
		}
		std::sort(entries.begin() + 1, entries.end(), [](const Entry& a, const Entry& b) {
			if (a.Label == b.Label) return a.Secondary < b.Secondary;
			return a.Label < b.Label;
		});
		s_AssetEntryCache[cacheKey] = AssetEntryCache{ AssetRegistry::GetChangeVersion(), entries };
		return entries;
	}

	std::vector<Entry> CollectEntities(bool includePrefabAssets) {
		std::vector<Entry> entries;
		entries.push_back({ k_NoneLabel, "", "(none)", "0", "__none__" });
		std::vector<Entry> sceneEntries;
		std::vector<Entry> assetEntries;

		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			const std::string scenePrefix =
				std::to_string(static_cast<uint64_t>(scene.GetSceneId())) + ":";
			auto view = scene.GetRegistry().view<entt::entity>();
			for (EntityHandle handle : view) {
				if (!scene.IsValid(handle)) continue;
				const uint64_t entityId = scene.GetEntityPersistentID(handle);
				if (entityId == 0) continue;

				Entry entry;
				entry.Label = GetEntityName(scene, handle, entityId);
				entry.Secondary = scene.GetName();
				entry.SearchKey = ToLowerCopy(entry.Label + " " + entry.Secondary);
				entry.Value = std::to_string(entityId);
				entry.UniqueId = scenePrefix + entry.Value;
				entry.Group = EntryGroup::Scene;
				sceneEntries.push_back(std::move(entry));
			}
		});

		if (includePrefabAssets) {
			AssetRegistry::Sync();
			for (const AssetRegistry::Record& record : AssetRegistry::GetAssetsByKind(AssetKind::Prefab)) {
				Entry entry;
				entry.Label = std::filesystem::path(record.Path).filename().string();
				entry.Secondary = record.Path;
				entry.SearchKey = ToLowerCopy(entry.Label + " prefab " + entry.Secondary);
				entry.Value = "prefab:" + std::to_string(record.Id);
				entry.UniqueId = entry.Value;
				entry.Group = EntryGroup::Assets;
				assetEntries.push_back(std::move(entry));
			}
		}

		auto sortByDisplay = [](const Entry& a, const Entry& b) {
			if (a.Label == b.Label) return a.Secondary < b.Secondary;
			return a.Label < b.Label;
		};
		std::sort(sceneEntries.begin(), sceneEntries.end(), sortByDisplay);
		std::sort(assetEntries.begin(), assetEntries.end(), sortByDisplay);
		entries.insert(entries.end(), sceneEntries.begin(), sceneEntries.end());
		entries.insert(entries.end(), assetEntries.begin(), assetEntries.end());
		return entries;
	}

	std::vector<Entry> CollectComponentTargets(const std::string& componentDisplayName) {
		std::vector<Entry> entries;
		entries.push_back({ k_NoneLabel, "", "(none)", "", "__none__" });
		const ComponentInfo* info = FindComponentByDisplayName(componentDisplayName);
		if (!info || !info->has) return entries;

		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			const std::string scenePrefix =
				std::to_string(static_cast<uint64_t>(scene.GetSceneId())) + ":";
			auto view = scene.GetRegistry().view<entt::entity>();
			for (EntityHandle handle : view) {
				if (!scene.IsValid(handle)) continue;
				const uint64_t entityId = scene.GetEntityPersistentID(handle);
				if (entityId == 0) continue;

				Entity entity = scene.GetEntity(handle);
				if (!info->has(entity)) continue;

				const std::string entityName = GetEntityName(scene, handle, entityId);
				Entry entry;
				entry.Label = entityName + " (" + componentDisplayName + ")";
				entry.SearchKey = ToLowerCopy(entityName + " " + componentDisplayName);
				entry.Value = std::to_string(entityId) + ":" + componentDisplayName;
				entry.UniqueId = scenePrefix + entry.Value;
				entries.push_back(std::move(entry));
			}
		});

		std::sort(entries.begin() + 1, entries.end(), [](const Entry& a, const Entry& b) {
			if (a.Label == b.Label) return a.Secondary < b.Secondary;
			return a.Label < b.Label;
		});
		return entries;
	}

	void OpenForFieldKey(const std::string& fieldKey, const std::string& title,
		std::vector<Entry> entries, Style style, bool useEntityTabs)
	{
		s_State.RequestOpen = true;
		s_State.IsOpen = true;
		s_State.Title = title;
		s_State.TargetFieldKey = fieldKey;
		s_State.Entries = std::move(entries);
		s_State.Search[0] = '\0';
		s_State.Style = style;
		s_State.UseEntityTabs = useEntityTabs;
		s_State.ActiveEntityTab = EntryGroup::Scene;
		s_State.VisibleEntriesDirty = true;
		s_State.VisibleEntryIndices.clear();
		s_State.LastFilter.clear();
		s_State.LastIncludeBuiltIns = s_State.IncludeBuiltIns;
		s_State.LastEntityTab = EntryGroup::Any;
		if (style == Style::Thumbnails) {
			EnsureThumbnailCacheInitialized();
			s_State.DiscardPending = true;
		}
	}

	std::optional<std::string> ConsumeSelection(const std::string& fieldKey) {
		if (s_State.PendingFieldKey != fieldKey) return std::nullopt;
		std::string value = s_State.PendingValue;
		s_State.PendingFieldKey.clear();
		s_State.PendingValue.clear();
		return value;
	}

	void CancelForPrefix(const std::string& prefix) {
		const auto startsWith = [&prefix](const std::string& s) {
			return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
		};
		if (startsWith(s_State.PendingFieldKey)) {
			s_State.PendingFieldKey.clear();
			s_State.PendingValue.clear();
		}
		if (s_State.IsOpen && startsWith(s_State.TargetFieldKey)) {
			s_State.IsOpen = false;
		}
	}

	void RenderPopup() {
		const int frame = ImGui::GetFrameCount();
		if (s_State.LastRenderedFrame == frame) return;
		s_State.LastRenderedFrame = frame;

		if (s_State.DiscardPending) {
			DiscardThumbnails();
			s_State.DiscardPending = false;
		}

		if (!s_State.IsOpen) {
			return;
		}

		const ImVec2 size = (s_State.Style == Style::Thumbnails)
			? ImVec2(360.0f, 460.0f)
			: ImVec2(440.0f, 430.0f);
		if (s_State.RequestOpen) {
			ImGui::SetNextWindowSize(size, ImGuiCond_Always);
			ImGui::SetNextWindowFocus();
			s_State.RequestOpen = false;
		}
		else {
			ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
		}

		const std::string windowTitle = s_State.Title + "##ReferencePickerWindow";
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (!ImGui::Begin(windowTitle.c_str(), &s_State.IsOpen,
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
		{
			ImGui::End();
			// Queue the thumbnail cache discard for the next frame —
			// see DiscardPending comment for the crash this avoids.
			if (!s_State.IsOpen) s_State.DiscardPending = true;
			return;
		}

		const ImGuiStyle& style = ImGui::GetStyle();
		const bool showingBuiltIns = s_State.IncludeBuiltIns;
		const uint64_t eyeIcon = EditorIcons::Get(
			showingBuiltIns ? "eye_open" : "eye_closed", 16);
		const ImVec2 eyeIconSize(16.0f, 16.0f);
		const float toggleWidth = (eyeIcon != 0)
			? eyeIconSize.x + style.FramePadding.x * 2.0f
			: ImGui::CalcTextSize("(o)").x + style.FramePadding.x * 2.0f;
		Icons::TextIcon(Icons::Type::Search);
		const float searchWidth = std::max(60.0f,
			ImGui::GetContentRegionAvail().x - toggleWidth - style.ItemSpacing.x);
		ImGui::SetNextItemWidth(searchWidth);
		if (ImGui::InputTextWithHint("##ReferenceSearch", "Search...", s_State.Search, sizeof(s_State.Search))) {
			s_State.VisibleEntriesDirty = true;
		}
		ImGui::SameLine();
		bool toggleClicked = false;
		if (eyeIcon != 0) {
			// Full-white tint in both states — the open/closed glyph swap
			// is what communicates the state, not the brightness.
			constexpr ImVec4 k_Tint(1.0f, 1.0f, 1.0f, 1.0f);
			toggleClicked = ImGui::ImageButton("##BuiltInToggle",
				static_cast<ImTextureID>(static_cast<intptr_t>(eyeIcon)),
				eyeIconSize, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
				ImVec4(0.0f, 0.0f, 0.0f, 0.0f), k_Tint);
		}
		else {
			if (showingBuiltIns) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			}
			toggleClicked = ImGui::Button(showingBuiltIns ? "(o)##BuiltInToggle" : "(-)##BuiltInToggle");
			if (showingBuiltIns) {
				ImGui::PopStyleColor();
			}
		}
		if (toggleClicked) {
			s_State.IncludeBuiltIns = !s_State.IncludeBuiltIns;
			s_State.VisibleEntriesDirty = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(showingBuiltIns
				? "Built-in (engine-shipped) assets are visible.\nClick to hide them."
				: "Built-in (engine-shipped) assets are hidden.\nClick to show them.");
		}
		ImGui::Separator();
		if (s_State.UseEntityTabs) {
			if (ImGui::BeginTabBar("##EntityReferenceTabs")) {
				if (ImGui::BeginTabItem("Scene")) {
					if (s_State.ActiveEntityTab != EntryGroup::Scene) {
						s_State.ActiveEntityTab = EntryGroup::Scene;
						s_State.VisibleEntriesDirty = true;
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Assets")) {
					if (s_State.ActiveEntityTab != EntryGroup::Assets) {
						s_State.ActiveEntityTab = EntryGroup::Assets;
						s_State.VisibleEntriesDirty = true;
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}

		RebuildVisibleEntriesIfNeeded();

		auto applySelection = [&](const Entry* entry) {
			s_State.PendingFieldKey = s_State.TargetFieldKey;
			s_State.PendingValue = entry ? entry->Value : std::string();
			s_State.IsOpen = false;
		};

		ImGui::BeginChild("##ReferencePickerList");

		if (s_State.Style == Style::Thumbnails) {
			const float thumbnailSize = 48.0f;
			const float rowPadding = 6.0f;
			const float lineHeight = ImGui::GetTextLineHeight();
			const float rowHeight = std::max(thumbnailSize + rowPadding * 2.0f,
				lineHeight * 2.0f + rowPadding * 2.0f + 2.0f);
			std::unordered_set<std::string> visiblePaths;
			ImDrawList* drawList = ImGui::GetWindowDrawList();

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(s_State.VisibleEntryIndices.size()), rowHeight);
			while (clipper.Step()) {
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
					const Entry& entry = s_State.Entries[
						s_State.VisibleEntryIndices[static_cast<std::size_t>(index)]];
					const bool hasThumbnail = !entry.Secondary.empty();
					if (hasThumbnail) {
						visiblePaths.insert(entry.Secondary);
						s_State.LoadedThumbnailPaths.insert(entry.Secondary);
					}

					ImGui::PushID(entry.UniqueId.c_str());
					const float rowWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
					const ImVec2 rowMin = ImGui::GetCursorScreenPos();
					const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
					// Use the button's click result (press AND release on this row), not
					// IsItemClicked (which fires on mouse-down) — so a drag/scroll that
					// starts on a row, or releases off it, doesn't select it.
					const bool clicked = ImGui::InvisibleButton("##Row", ImVec2(rowWidth, rowHeight));
					const bool hovered = ImGui::IsItemHovered();
					if (hovered) {
						drawList->AddRectFilled(rowMin, rowMax, IM_COL32(70, 78, 92, 120), 4.0f);
					}
					if (clicked) {
						applySelection(&entry);
					}

					const ImVec2 thumbMin(rowMin.x + rowPadding, rowMin.y + rowPadding);
					if (hasThumbnail) {
						const ImVec2 thumbMax(thumbMin.x + thumbnailSize, thumbMin.y + thumbnailSize);
						drawList->AddRectFilled(thumbMin, thumbMax, IM_COL32(35, 35, 35, 255), 4.0f);

						const uint64_t thumbnail = s_State.Thumbnails.GetThumbnail(entry.Secondary);
						Texture2D* texture = s_State.Thumbnails.GetCacheEntry(entry.Secondary);
						if (thumbnail != 0 && texture && texture->IsValid()) {
							const float texW = static_cast<float>(texture->GetWidth());
							const float texH = static_cast<float>(texture->GetHeight());

							float aspectW = texW;
							float aspectH = texH;
							float u0 = 0.0f, u1 = 1.0f;
							float v0 = 1.0f, v1 = 0.0f;
							if (entry.IsSlice
								&& texW > 0.0f && texH > 0.0f
								&& entry.SliceW > 0 && entry.SliceH > 0)
							{
								aspectW = static_cast<float>(entry.SliceW);
								aspectH = static_cast<float>(entry.SliceH);
								u0 = static_cast<float>(entry.SliceX) / texW;
								u1 = static_cast<float>(entry.SliceX + entry.SliceW) / texW;
								v0 = 1.0f - static_cast<float>(entry.SliceY) / texH;
								v1 = 1.0f - static_cast<float>(entry.SliceY + entry.SliceH) / texH;
							}

							float drawWidth = thumbnailSize;
							float drawHeight = thumbnailSize;
							if (aspectW > 0.0f && aspectH > 0.0f) {
								const float aspect = aspectW / aspectH;
								if (aspect > 1.0f) drawHeight = thumbnailSize / aspect;
								else               drawWidth  = thumbnailSize * aspect;
							}
							const ImVec2 imageMin(
								thumbMin.x + (thumbnailSize - drawWidth) * 0.5f,
								thumbMin.y + (thumbnailSize - drawHeight) * 0.5f);
							const ImVec2 imageMax(imageMin.x + drawWidth, imageMin.y + drawHeight);
							ImGuiUtils::AddImageFiltered(drawList,
								static_cast<ImTextureID>(static_cast<intptr_t>(thumbnail)),
								imageMin, imageMax, texture->GetFilter(),
								ImVec2(u0, v0), ImVec2(u1, v1));
						}
						else {
							ThumbnailCache::DrawAssetIcon(AssetType::Image, thumbMin, thumbnailSize);
						}
					}

					const float textX = hasThumbnail
						? thumbMin.x + thumbnailSize + rowPadding * 1.5f
						: rowMin.x + rowPadding;
					const float textWidth = std::max(rowMax.x - textX - rowPadding, 1.0f);
					bool nameTruncated = false;
					bool pathTruncated = false;
					const std::string displayName = ImGuiUtils::Ellipsize(entry.Label, textWidth, &nameTruncated);
					drawList->AddText(ImVec2(textX, rowMin.y + rowPadding),
						ImGui::GetColorU32(ImGuiCol_Text), displayName.c_str());
					// Surface the project-relative form (Assets/..., IndexAssets/...) instead
					// of the absolute path stored in Secondary — Secondary itself stays the
					// absolute path because the thumbnail cache + asset lookups below key on it.
					std::string secondaryDisplay;
					if (!entry.Secondary.empty()) {
						secondaryDisplay = Path::ToProjectRelativeDisplay(entry.Secondary);
						const std::string displayPath = ImGuiUtils::Ellipsize(secondaryDisplay, textWidth, &pathTruncated);
						drawList->AddText(ImVec2(textX, rowMin.y + rowPadding + lineHeight),
							ImGui::GetColorU32(ImGuiCol_TextDisabled), displayPath.c_str());
					}
					if (hovered && (nameTruncated || pathTruncated)) {
						ImGui::SetTooltip("%s", secondaryDisplay.empty()
							? entry.Label.c_str()
							: secondaryDisplay.c_str());
					}
					ImGui::PopID();
				}
			}

			for (auto it = s_State.LoadedThumbnailPaths.begin(); it != s_State.LoadedThumbnailPaths.end();) {
				if (visiblePaths.find(*it) == visiblePaths.end()) {
					s_State.Thumbnails.Invalidate(*it);
					it = s_State.LoadedThumbnailPaths.erase(it);
				}
				else {
					++it;
				}
			}
		}
		else {
			for (std::size_t entryIndex : s_State.VisibleEntryIndices) {
				const Entry& entry = s_State.Entries[entryIndex];
				ImGui::PushID(entry.UniqueId.c_str());
				bool truncated = false;
				const std::string label = ImGuiUtils::Ellipsize(entry.Label, ImGui::GetContentRegionAvail().x, &truncated);
				if (ImGui::Selectable(label.c_str(), false)) {
					applySelection(&entry);
				}
				// Entities/component-refs feed plain scene names through Secondary,
				// so the helper is a no-op for them; for prefab rows (Secondary is
				// an absolute path) it collapses to "Assets/.../Foo.prefab".
				std::string secondaryDisplay;
				if (!entry.Secondary.empty()) {
					secondaryDisplay = Path::ToProjectRelativeDisplay(entry.Secondary);
					if (secondaryDisplay.empty()) secondaryDisplay = entry.Secondary;
				}
				if (ImGui::IsItemHovered() && (truncated || !secondaryDisplay.empty())) {
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(entry.Label.c_str());
					if (!secondaryDisplay.empty()) {
						ImGui::Separator();
						ImGui::TextDisabled("%s", secondaryDisplay.c_str());
					}
					ImGui::EndTooltip();
				}
				if (!secondaryDisplay.empty()) {
					ImGui::Indent(14.0f);
					ImGuiUtils::TextDisabledEllipsis(secondaryDisplay);
					ImGui::Unindent(14.0f);
				}
				ImGui::PopID();
			}
		}

		if (s_State.VisibleEntryIndices.empty()) ImGui::TextDisabled("No matching items");
		ImGui::EndChild();
		ImGui::End();

		if (!s_State.IsOpen) s_State.DiscardPending = true;
	}

	bool DrawReferenceField(const char* label, const std::string& displayValue,
		const std::string& secondary, bool missing, bool mixed, bool& outHovered,
		float reservedTrailingWidth)
	{
		ImGui::PushID(label);
		ImGuiUtils::BeginInspectorFieldRow(label);
		const ImGuiStyle& style = ImGui::GetStyle();
		const float availableWidth = ImGui::GetContentRegionAvail().x;
		float buttonWidth = availableWidth;
		if (reservedTrailingWidth > 0.0f) {
			buttonWidth = std::max(1.0f, availableWidth - reservedTrailingWidth - style.ItemSpacing.x);
		}
		else {
			buttonWidth = std::max(buttonWidth, 120.0f);
		}

		if (missing) {
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.12f, 0.12f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.16f, 0.16f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.10f, 0.10f, 1.0f));
		}
		else if (mixed) {
			ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
		}
		else {
			ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
		}

		const std::string buttonLabel = mixed ? std::string("\xe2\x80\x94") : displayValue;  // em-dash
		bool truncated = false;
		const float textMaxWidth = std::max(1.0f, buttonWidth - style.FramePadding.x * 2.0f);
		const std::string buttonText = ImGuiUtils::Ellipsize(buttonLabel, textMaxWidth, &truncated);
		const bool clicked = ImGui::Button((buttonText + "##ReferenceValue").c_str(), ImVec2(buttonWidth, 0.0f));
		outHovered = ImGui::IsItemHovered();
		if (outHovered && !mixed && (truncated || !secondary.empty())) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(displayValue.c_str());
			if (!secondary.empty()) {
				ImGui::Separator();
				ImGui::TextDisabled("%s", secondary.c_str());
			}
			ImGui::EndTooltip();
		}
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		return clicked;
	}

	std::string ResolveAssetDisplay(uint64_t assetId, AssetKind expectedKind,
		bool& outMissing, std::string* outSecondary)
	{
		outMissing = false;
		if (outSecondary) outSecondary->clear();
		if (assetId == 0) return GetNoneAssetLabel(expectedKind);

		EnsureBuiltInsRegisteredInEditor();

		// Script-created (procedural) textures live in the GPU TextureManager under a
		// synthetic runtime UUID, never in the AssetRegistry — so the kind check below
		// would flag them "(Missing Asset)" in red. They're valid, not missing: the
		// preview thumbnail already resolves. Label them as runtime instead.
		if (expectedKind == AssetKind::Texture && TextureManager::IsRuntimeTextureUUID(assetId)) {
			if (outSecondary) *outSecondary = "Created at runtime via Texture.Create()";
			return "(Runtime Texture)";
		}

		const AssetKind kind = AssetRegistry::GetKind(assetId);
		if (kind != expectedKind) {
			outMissing = true;
			return "(Missing Asset)";
		}
		if (outSecondary) *outSecondary = Path::ToProjectRelativeDisplay(AssetRegistry::ResolvePath(assetId));
		const std::string name = AssetRegistry::GetDisplayName(assetId);
		if (name.empty()) {
			outMissing = true;
			return "(Missing Asset)";
		}
		return name;
	}

	std::string ResolvePrefabDisplay(uint64_t prefabId, bool& outMissing,
		std::string* outSecondary)
	{
		outMissing = false;
		if (outSecondary) outSecondary->clear();
		if (prefabId == 0) return GetNoneAssetLabel(AssetKind::Prefab);
		if (AssetRegistry::GetKind(prefabId) != AssetKind::Prefab) {
			outMissing = true;
			return "(Missing Prefab)";
		}
		if (outSecondary) *outSecondary = Path::ToProjectRelativeDisplay(AssetRegistry::ResolvePath(prefabId));
		return AssetRegistry::GetDisplayName(prefabId);
	}

	std::string ResolveEntityDisplay(uint64_t entityId, bool& outMissing,
		std::string* outSecondary)
	{
		outMissing = false;
		if (outSecondary) outSecondary->clear();
		if (entityId == 0) return k_NoneLabel;

		std::string display;
		std::string secondary;
		bool resolved = false;
		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			if (resolved) return;
			EntityHandle handle = entt::null;
			if (scene.TryResolveEntityRef(entityId, handle)) {
				display = GetEntityName(scene, handle, entityId);
				secondary = scene.GetName();
				resolved = true;
			}
		});
		if (!resolved) {
			outMissing = true;
			return "(Missing Entity)";
		}
		if (outSecondary) *outSecondary = secondary;
		return display;
	}

	std::string ResolveComponentRefDisplay(uint64_t entityId,
		const std::string& componentTypeName, bool& outMissing,
		std::string* outSecondary)
	{
		outMissing = false;
		if (outSecondary) outSecondary->clear();
		if (entityId == 0) return k_NoneLabel;

		std::string entityName;
		bool resolved = false;
		SceneManager::Get().ForeachLoadedScene([&](const Scene& scene) {
			if (resolved) return;
			EntityHandle handle = entt::null;
			// UUID-aware: see ResolveEntityDisplay — same reload story.
			if (scene.TryResolveEntityRef(entityId, handle)) {
				entityName = GetEntityName(scene, handle, entityId);
				resolved = true;
			}
		});
		if (!resolved) {
			outMissing = true;
			return "(Missing)." + componentTypeName;
		}
		return entityName + " (" + componentTypeName + ")";
	}

	void Shutdown() {
		if (s_State.ThumbnailsInitialized) {
			s_State.Thumbnails.Clear();
		}
		s_State = {};
		s_BuiltInsDone = false; // H22: rerun built-in registration on next open.
		s_AssetEntryCache.clear();
	}

} // namespace Index::ReferencePicker
