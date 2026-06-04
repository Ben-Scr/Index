#include <pch.hpp>
#include "ImGuiEditorLayer.hpp"

#include <imgui.h>
#include <imgui_internal.h>
// glad include retired with the legacy GL backend (2026-05); the editor
// no longer makes raw GL calls, all drawing routes through engine renderers.

#include <cstdio>

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
#include "Components/Audio/AudioSourceComponent.hpp"
#include "Scripting/ScriptComponent.hpp"
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
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Events/EventDispatcher.hpp"
#include "Events/SceneEvents.hpp"
#include "Events/WindowFocusEvent.hpp"
#include "Graphics/Renderer2D.hpp"
#include "Graphics/GizmoRenderer.hpp"
#include "Profiling/Profiler.hpp"
#include "Graphics/Gizmo.hpp"
#include "Gui/CustomTitlebar.hpp"
#include "Gui/GuiRenderer.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/ComponentRegistry.hpp"
#include <Scene/EntityHelper.hpp>

#include "Graphics/TextureManager.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Serialization/Path.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Serialization/SceneSerializerShared.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/File.hpp"
#include "Utils/Process.hpp"
#include "Packages/NuGetSource.hpp"
#include "Packages/GitHubSource.hpp"
#include "Editor/ApplicationEditorAccess.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Gui/AddComponentPopup.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/Icons.hpp"
#include "Gui/EditorTheme.hpp"
#include "Gui/ImGuiContextLayer.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Assets/AssetRegistry.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptComponentInspector.hpp"
#include "Systems/TransformHierarchySystem.hpp"
#include "Systems/UILayoutSystem.hpp"
#include "Math/VectorMath.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_set>

namespace Index {
	namespace {
		// Awakens entities that already exist when play begins; runtime-spawned ones are
		// handled at instantiation (SceneSerializer::InstantiatePrefab). Both share the
		// per-component PlayOnAwakeIfEnabled() rule.
		void StartPlayOnAwakeComponents(Scene& scene)
		{
			for (auto [ent, audio] : scene.GetRegistry().view<AudioSourceComponent>(entt::exclude<DisabledTag>).each())
				audio.PlayOnAwakeIfEnabled();

			for (auto [ent, particleSystem] : scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>).each())
				particleSystem.PlayOnAwakeIfEnabled();
		}

		bool HasSelectedAncestor(const Scene& scene, EntityHandle entity, const std::unordered_set<uint32_t>& selectedEntities)
		{
			const auto& registry = scene.GetRegistry();
			EntityHandle current = entity;
			while (current != entt::null && registry.valid(current) && registry.all_of<HierarchyComponent>(current)) {
				const EntityHandle parent = registry.get<HierarchyComponent>(current).Parent;
				if (parent == entt::null || !registry.valid(parent)) {
					return false;
				}

				if (selectedEntities.contains(static_cast<uint32_t>(parent))) {
					return true;
				}

				current = parent;
			}

			return false;
		}

		std::vector<EntityHandle> FilterSelectedHierarchyRoots(const Scene& scene, const std::vector<EntityHandle>& selectedEntities)
		{
			std::unordered_set<uint32_t> selectedSet;
			selectedSet.reserve(selectedEntities.size());
			for (EntityHandle entity : selectedEntities) {
				if (entity != entt::null && scene.IsValid(entity)) {
					selectedSet.insert(static_cast<uint32_t>(entity));
				}
			}

			std::vector<EntityHandle> roots;
			roots.reserve(selectedEntities.size());
			for (EntityHandle entity : selectedEntities) {
				if (entity == entt::null || !scene.IsValid(entity)) {
					continue;
				}

				if (!HasSelectedAncestor(scene, entity, selectedSet)) {
					roots.push_back(entity);
				}
			}

			return roots;
		}

		bool IsAsciiDigits(std::string_view value)
		{
			if (value.empty()) return false;
			for (char c : value) {
				if (c < '0' || c > '9') return false;
			}
			return true;
		}

		std::string StripEditorNumericSuffix(std::string_view name)
		{
			std::string base(name);
			if (base.empty()) return "Entity";

			if (base.size() > 3 && base.back() == ')') {
				const std::size_t open = base.rfind(" (");
				if (open != std::string::npos && open + 2 < base.size() - 1
					&& IsAsciiDigits(std::string_view(base).substr(open + 2, base.size() - open - 3))) {
					base.erase(open);
					return base.empty() ? "Entity" : base;
				}
			}

			auto stripDelimitedDigits = [&](char delimiter) -> bool {
				const std::size_t pos = base.rfind(delimiter);
				if (pos == std::string::npos || pos + 1 >= base.size()) return false;
				if (!IsAsciiDigits(std::string_view(base).substr(pos + 1))) return false;
				base.erase(pos);
				return true;
			};

			if (stripDelimitedDigits('-') || stripDelimitedDigits('_') || stripDelimitedDigits(' ')) {
				return base.empty() ? "Entity" : base;
			}

			return base;
		}

		std::string FormatEditorEntitySuffix(
			const std::string& base,
			int index,
			IndexProject::EditorEntityNameSuffixStyle style)
		{
			switch (style) {
			case IndexProject::EditorEntityNameSuffixStyle::SpaceNumber:
				return base + " " + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::HyphenNumber:
				return base + "-" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::UnderscoreNumber:
				return base + "_" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber:
			default:
				return base + " (" + std::to_string(index) + ")";
			}
		}

		std::string BuildScriptMenuLabel(const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			return scriptEntry.ClassName + "  " + scriptEntry.Extension;
		}

		bool AttachScriptToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || scriptEntry.Type == ScriptType::Unknown) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			const bool alreadyHadScript = scriptComponent.HasScript(scriptEntry.ClassName, scriptEntry.Type);

			if (!alreadyHadScript) {
				scriptComponent.AddScript(scriptEntry.ClassName, scriptEntry.Type);
			}

			// Also populate DynamicComponentStorage so HasNativeComponent<T>()/GetRef<T>() in scripts can find it; done unconditionally so re-attach after failed registration also seeds the storage.
			bool storageAdded = false;
			if (scriptEntry.IsNativeComponent) {
				auto& componentRegistry = SceneManager::Get().GetComponentRegistry();
				if (const ComponentInfo* info = componentRegistry.FindBySerializedName(scriptEntry.ClassName)) {
					if (info->isDynamic && info->add && (!info->has || !info->has(entity))) {
						info->add(entity);
						storageAdded = true;
					}
				}
			}

			if (alreadyHadScript && !storageAdded) {
				return false;
			}

			scene.MarkDirty();
			return true;
		}

		bool AttachManagedComponentToEntity(Entity entity, Scene& scene, const EditorScriptDiscovery::ScriptEntry& scriptEntry)
		{
			if (scriptEntry.ClassName.empty() || !scriptEntry.IsManagedComponent) {
				return false;
			}

			if (!entity.HasComponent<ScriptComponent>()) {
				entity.AddComponent<ScriptComponent>();
			}

			auto& scriptComponent = entity.GetComponent<ScriptComponent>();
			if (scriptComponent.HasManagedComponent(scriptEntry.ClassName)) {
				return false;
			}

			scriptComponent.AddManagedComponent(scriptEntry.ClassName);
			scene.MarkDirty();
			return true;
		}

		bool IsLeftMouseDragPastClickThreshold()
		{
			const ImGuiIO& io = ImGui::GetIO();
			const ImVec2 delta(io.MousePos.x - io.MouseClickedPos[ImGuiMouseButton_Left].x,
				io.MousePos.y - io.MouseClickedPos[ImGuiMouseButton_Left].y);
			return (delta.x * delta.x + delta.y * delta.y) > (io.MouseDragThreshold * io.MouseDragThreshold);
		}

		struct ComponentClipboardPayload {
			std::string SerializedName;
			Json::Value Data;
		};

		bool TryReadComponentClipboard(const std::string& clipboardJson, ComponentClipboardPayload& outPayload)
		{
			if (clipboardJson.empty()) {
				return false;
			}

			Json::Value root;
			std::string parseError;
			if (!Json::TryParse(clipboardJson, root, &parseError) || !root.IsObject()) {
				return false;
			}

			const Json::Value* componentValue = root.FindMember("component");
			const Json::Value* dataValue = root.FindMember("data");
			if (!componentValue || !componentValue->IsString() || !dataValue || dataValue->IsNull()) {
				return false;
			}

			outPayload.SerializedName = componentValue->AsStringOr();
			outPayload.Data = *dataValue;
			return !outPayload.SerializedName.empty();
		}

		struct LogSourceLocation {
			std::string FilePath;
			std::string LinkText;
			int Line = 0;
			// Byte offsets of the "<path>:N" link substring in the message; console renderer styles that span inline as the clickable link.
			size_t MessageStart = 0;
			size_t MessageEnd = 0;
		};

		bool IsLineBoundary(std::string_view text, size_t pos) {
			return pos == std::string_view::npos
				|| pos >= text.size()
				|| text[pos] == '\n'
				|| text[pos] == '\r';
		}

		bool IsExtensionBoundary(std::string_view text, size_t pos) {
			if (pos >= text.size()) return true;
			const unsigned char c = static_cast<unsigned char>(text[pos]);
			return !std::isalnum(c) && text[pos] != '_';
		}

		bool StartsWithIgnoreCase(std::string_view text, size_t pos, std::string_view prefix) {
			if (pos + prefix.size() > text.size()) return false;
			for (size_t i = 0; i < prefix.size(); i++) {
				const unsigned char a = static_cast<unsigned char>(text[pos + i]);
				const unsigned char b = static_cast<unsigned char>(prefix[i]);
				if (std::tolower(a) != std::tolower(b)) {
					return false;
				}
			}
			return true;
		}

		void SkipWhitespace(std::string_view text, size_t& pos) {
			while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r'
				&& std::isspace(static_cast<unsigned char>(text[pos]))) {
				pos++;
			}
		}

		bool ParsePositiveInt(std::string_view text, size_t& pos, int& outValue) {
			SkipWhitespace(text, pos);
			if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
				return false;
			}

			int value = 0;
			while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
				value = value * 10 + (text[pos] - '0');
				pos++;
			}

			outValue = value;
			return value > 0;
		}

		bool TryParseLineNumber(std::string_view text, size_t pos, int& outLine, size_t* outEnd = nullptr) {
			SkipWhitespace(text, pos);
			if (pos >= text.size() || IsLineBoundary(text, pos)) {
				return false;
			}

			if (text[pos] == '(') {
				pos++;
				if (!ParsePositiveInt(text, pos, outLine)) return false;
				if (pos < text.size() && text[pos] == ')') pos++;
				if (outEnd) *outEnd = pos;
				return true;
			}

			if (text[pos] == ':' || text[pos] == ',') {
				pos++;
				SkipWhitespace(text, pos);
			}

			if (StartsWithIgnoreCase(text, pos, "line")) {
				pos += 4;
				SkipWhitespace(text, pos);
			}

			if (!ParsePositiveInt(text, pos, outLine)) return false;
			if (outEnd) *outEnd = pos;
			return true;
		}

		std::string ToLowerCopy(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		std::string CanonicalFilePath(const std::filesystem::path& path) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) || ec) {
				return {};
			}
			std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
			return (ec ? path : canonical).lexically_normal().make_preferred().string();
		}

		std::string FindFileByName(const std::filesystem::path& root, const std::string& fileNameLower) {
			std::error_code ec;
			if (!std::filesystem::is_directory(root, ec) || ec) {
				return {};
			}

			std::filesystem::recursive_directory_iterator it(
				root,
				std::filesystem::directory_options::skip_permission_denied,
				ec);
			const std::filesystem::recursive_directory_iterator end;
			for (; it != end && !ec; it.increment(ec)) {
				const std::filesystem::directory_entry& entry = *it;
				std::error_code entryEc;
				if (!entry.is_regular_file(entryEc) || entryEc) continue;
				if (ToLowerCopy(entry.path().filename().string()) == fileNameLower) {
					return CanonicalFilePath(entry.path());
				}
			}
			return {};
		}

		std::string ResolveLogSourceFile(std::string rawPath) {
			if (rawPath.rfind("file:///", 0) == 0) {
				rawPath.erase(0, 8);
			}
			if (rawPath.empty()) {
				return {};
			}

			const std::filesystem::path sourcePath(rawPath);
			std::vector<std::filesystem::path> candidates;
			if (sourcePath.is_absolute()) {
				candidates.push_back(sourcePath);
			}
			else {
				candidates.push_back(std::filesystem::current_path() / sourcePath);
				if (IndexProject* project = ProjectManager::GetCurrentProject()) {
					if (!project->RootDirectory.empty()) candidates.emplace_back(std::filesystem::path(project->RootDirectory) / sourcePath);
					if (!project->ScriptsDirectory.empty()) candidates.emplace_back(std::filesystem::path(project->ScriptsDirectory) / sourcePath);
					if (!project->AssetsDirectory.empty()) candidates.emplace_back(std::filesystem::path(project->AssetsDirectory) / sourcePath);
				}
			}

			for (const std::filesystem::path& candidate : candidates) {
				if (std::string resolved = CanonicalFilePath(candidate); !resolved.empty()) {
					return resolved;
				}
			}

			if (!sourcePath.has_filename()) {
				return {};
			}

			IndexProject* project = ProjectManager::GetCurrentProject();
			if (!project) {
				return {};
			}

			const std::string fileNameLower = ToLowerCopy(sourcePath.filename().string());
			const std::array<std::filesystem::path, 3> roots = {
				std::filesystem::path(project->ScriptsDirectory),
				std::filesystem::path(project->AssetsDirectory),
				std::filesystem::path(project->RootDirectory)
			};
			for (const std::filesystem::path& root : roots) {
				if (std::string resolved = FindFileByName(root, fileNameLower); !resolved.empty()) {
					return resolved;
				}
			}

			return {};
		}

		std::string ExtractRawSourcePath(std::string_view message, size_t extStart, size_t extEnd, size_t* outPathStart = nullptr) {
			size_t lineStart = message.rfind('\n', extStart);
			lineStart = (lineStart == std::string_view::npos) ? 0 : lineStart + 1;

			size_t start = std::string_view::npos;
			const size_t doubleQuote = message.rfind('"', extStart);
			const size_t singleQuote = message.rfind('\'', extStart);
			if (doubleQuote != std::string_view::npos && doubleQuote >= lineStart) {
				start = doubleQuote + 1;
			}
			if (singleQuote != std::string_view::npos && singleQuote >= lineStart
				&& (start == std::string_view::npos || singleQuote + 1 > start)) {
				start = singleQuote + 1;
			}

			const size_t inToken = message.rfind(" in ", extStart);
			if (start == std::string_view::npos && inToken != std::string_view::npos && inToken >= lineStart) {
				start = inToken + 4;
			}

			if (start == std::string_view::npos) {
				start = extStart;
				while (start > lineStart) {
					const char prev = message[start - 1];
					if (std::isspace(static_cast<unsigned char>(prev))
						|| prev == '"' || prev == '\'' || prev == '<' || prev == '>'
						|| prev == '[' || prev == ']') {
						break;
					}
					start--;
				}
			}

			while (start < extStart && std::isspace(static_cast<unsigned char>(message[start]))) {
				start++;
			}

			if (outPathStart) *outPathStart = start;
			return std::string(message.substr(start, extEnd - start));
		}

		std::optional<LogSourceLocation> FindLogSourceLocation(std::string_view message) {
			static constexpr std::array<std::string_view, 14> kScriptExtensions = {
				".cs", ".cpp", ".hpp", ".h", ".c", ".lua", ".py",
				".js", ".ts", ".json", ".xml", ".yaml", ".yml", ".shader"
			};

			for (size_t pos = 0; pos < message.size(); pos++) {
				for (std::string_view ext : kScriptExtensions) {
					if (!StartsWithIgnoreCase(message, pos, ext)) {
						continue;
					}
					const size_t extEnd = pos + ext.size();
					if (!IsExtensionBoundary(message, extEnd)) {
						continue;
					}

					int line = 0;
					size_t lineEndPos = 0;
					if (!TryParseLineNumber(message, extEnd, line, &lineEndPos)) {
						continue;
					}

					size_t pathStartPos = 0;
					std::string rawPath = ExtractRawSourcePath(message, pos, extEnd, &pathStartPos);
					if (rawPath.empty()) {
						continue;
					}

					std::filesystem::path rawExtPath(rawPath);
					std::string loweredExt = rawExtPath.extension().string();
					std::transform(loweredExt.begin(), loweredExt.end(), loweredExt.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (!ExternalEditor::IsScriptExtension(loweredExt)) {
						continue;
					}

					std::string resolved = ResolveLogSourceFile(rawPath);
					if (resolved.empty()) {
						continue;
					}

					LogSourceLocation location;
					location.FilePath = resolved;
					location.Line = line;
					location.LinkText = std::filesystem::path(resolved).filename().string()
						+ ":" + std::to_string(line);
					location.MessageStart = pathStartPos;
					location.MessageEnd = lineEndPos;
					return location;
				}
			}

			return std::nullopt;
		}

		const ComponentInfo* FindComponentInfoBySerializedName(const ComponentRegistry& registry, const std::string& serializedName)
		{
			const ComponentInfo* found = nullptr;
			registry.ForEachComponentInfo([&](const std::type_index&, const ComponentInfo& info) {
				if (!found && info.category == ComponentCategory::Component && info.serializedName == serializedName) {
					found = &info;
				}
			});
			return found;
		}

		float DivideOrKeep(float numerator, float denominator, float fallback)
		{
			constexpr float k_MinScaleAxis = 0.0001f;
			return std::abs(denominator) > k_MinScaleAxis ? numerator / denominator : fallback;
		}

		void SetLocalTransformFromWorld(
			Transform2DComponent& child,
			const Transform2DComponent* parent,
			const Vec2& worldPosition,
			float worldRotation,
			const Vec2& worldScale)
		{
			if (parent) {
				Vec2 parentRelative = worldPosition - parent->Position;
				parentRelative = Rotate(parentRelative, -parent->Rotation);
				child.LocalPosition = {
					DivideOrKeep(parentRelative.x, parent->Scale.x, child.LocalPosition.x),
					DivideOrKeep(parentRelative.y, parent->Scale.y, child.LocalPosition.y)
				};
				child.LocalRotation = worldRotation - parent->Rotation;
				child.LocalScale = {
					DivideOrKeep(worldScale.x, parent->Scale.x, child.LocalScale.x),
					DivideOrKeep(worldScale.y, parent->Scale.y, child.LocalScale.y)
				};
			}
			else {
				child.LocalPosition = worldPosition;
				child.LocalRotation = worldRotation;
				child.LocalScale = worldScale;
			}

			child.Position = worldPosition;
			child.Rotation = worldRotation;
			child.Scale = worldScale;
			child.MarkDirty();
		}
	}



	// ──────────────────────────────────────────────
	//  Lifecycle
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::OnAttach(Application& app) {
		// Start the splash first: SignalSplashAttached (inside Begin) must run
		// before Application's deferred-scene check so the project scene load is
		// held behind the splash.
		m_Splash.Begin();
		Application::SetIsPlaying(false);
		ApplicationEditorAccess::SetGameInputEnabled(false);
		// Scope the project's custom game cursor to the Game View: disable engine
		// cursor application now (before Application applies the project cursor in
		// Initialize) so it never goes editor-wide. OnUpdate re-enables it only
		// while the Game View is hovered/focused.
		if (Window* w = app.GetWindow()) {
			w->SetGameCursorEnabled(false);
		}
		Gizmo::SetShowInRuntime(false);
		if (app.GetRenderer2D()) {
			app.GetRenderer2D()->SetSkipBeginFrameRender(true);
		}
		// UI must render into the per-panel FBO, not the OS backbuffer; RenderSceneIntoFBO drives this manually.
		if (app.GetGuiRenderer()) {
			app.GetGuiRenderer()->SetSkipBeginFrameRender(true);
		}

		if (m_LogSubscriptionId.value != 0) {
			Log::OnLog.Remove(m_LogSubscriptionId);
		}

		m_ProfilerPanel.Initialize();
		m_SpriteEditorPanel.Initialize();

		// Load BEFORE ApplyTheme: ImGuiContextLayer already ran ApplyIndexTheme on its OnAttach; this reskins the live style with the user's saved choice.
		EditorPreferences::Load();
		EditorPreferences::ApplyTheme();
		m_EditorPreferencesPanel.Initialize();

		// One-time migration: seed EditorPreferences from legacy inline project fields if no prefs file existed yet.
		if (EditorPreferences::WasFreshlyCreated()) {
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				const auto& lp = project->LegacyEditorPrefs;
				if (lp.ShowFileExtensionsPresent) {
					EditorPreferences::SetShowFileExtensions(lp.ShowFileExtensions);
				}
				if (lp.AutoSaveScenesPresent) {
					EditorPreferences::SetAutoSaveScenes(lp.AutoSaveScenes);
				}
				if (lp.AutoSaveIntervalSecondsPresent) {
					EditorPreferences::SetAutoSaveIntervalSeconds(lp.AutoSaveIntervalSeconds);
				}
				if (lp.AutoRecompileScriptsPresent) {
					EditorPreferences::SetAutoRecompileScripts(lp.AutoRecompileScripts);
				}
				if (lp.RecompileScriptsOnPlayPresent) {
					EditorPreferences::SetRecompileScriptsOnPlay(lp.RecompileScriptsOnPlay);
				}
			}
		}
		Application::SetRunInBackground(EditorPreferences::GetRunInBackground());
		ScriptSystem::SetAutoRecompileEnabled(EditorPreferences::GetAutoRecompileScripts());

		ClearLogEntries();
		m_LogDispatchState = std::make_shared<LogDispatchState>();
		std::weak_ptr<LogDispatchState> weakLogDispatchState = m_LogDispatchState;
		m_LogSubscriptionId = Log::OnLog.Add([weakLogDispatchState](const Log::Entry& entry) {
			if (entry.Source == Log::Type::Core) return; // Core logs go to stdout only, not editor UI

			std::shared_ptr<LogDispatchState> state = weakLogDispatchState.lock();
			if (!state) return;

			// try_lock to avoid blocking worker threads; entries that miss the lock go to a per-thread buffer and are drained on the next successful lock (entries from threads that never log again are lost, acceptable).
			thread_local std::vector<LogEntry> tlBuf;

			if (state->Mutex.try_lock()) {
				std::lock_guard<std::mutex> guard(state->Mutex, std::adopt_lock);
				if (!tlBuf.empty()) {
					state->PendingEntries.insert(state->PendingEntries.end(),
						std::make_move_iterator(tlBuf.begin()),
						std::make_move_iterator(tlBuf.end()));
					tlBuf.clear();
				}
				state->PendingEntries.push_back({ entry.Message, entry.Level });
			}
			else {
				tlBuf.push_back({ entry.Message, entry.Level });
			}
			});
	}

	void ImGuiEditorLayer::OnDetach(Application& app) {
		(void)app;
		// MUST precede WebGPU teardown: Texture2D destructor touches the device.
		m_Splash.Shutdown();
		m_ProfilerPanel.Shutdown();
		m_SpriteEditorPanel.Shutdown();
		m_EditorPreferencesPanel.Shutdown();
		ApplicationEditorAccess::SetGameInputEnabled(true);
		Gizmo::SetShowInRuntime(true);
		if (m_LogSubscriptionId.value != 0) {
			Log::OnLog.Remove(m_LogSubscriptionId);
			m_LogSubscriptionId = EventId{};
		}
		m_AssetWatcher.Stop();
		m_AssetWatcherRoot.clear();
		m_LogDispatchState.reset();
		// Reset before further teardown: destructor unsubscribes from Log::OnLog, which must run while the layer is still live.
		m_GameViewLogOverlay.reset();
		ClearPreviewTextureCache();

		// Reverse construction order: panels → icons → FBOs.
		m_AssetBrowser.Shutdown();
		m_PackageManagerPanel.Shutdown();
		m_PackageManager.Shutdown();
		ExternalEditor::JoinPendingLaunchThreads();
		ReferencePicker::Shutdown();
		ScriptComponentInspector::Shutdown();
		EditorIcons::Shutdown();
		Icons::Shutdown();
		m_EditorViewFBO.Destroy();
		m_GameViewFBO.Destroy();
	}

	void ImGuiEditorLayer::OnEvent(Application& /*app*/, IndexEvent& event) {
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowFocusEvent>([this](WindowFocusEvent&) {
			m_AssetBrowser.RequestRefresh();
			return false;
			});

		// Scene swap: must clear all editor entity handles (selection, order cache, drag state) before any consumer sees the new world, or stale handles reach the hierarchy clipper and cause asserts.
		auto resetEditorEntityState = [this]() {
			m_SelectedEntity = entt::null;
			m_SelectedEntities.clear();
			m_SelectedEntitySet.clear();
			m_PressedEntity = entt::null;
			m_RenamingEntity = entt::null;
			m_EntityRenameFrameCounter = 0;
			m_EntityOrder.clear();
			m_RenderedEntityOrder.clear();
			m_RenderedEntityDepths.clear();
			m_VisibleEntityOrder.clear();
			m_VisibleEntityDepths.clear();
			m_EntityOrderDirty = true;
			m_EntityOrderSceneId = nullptr;
			m_CollapsedHierarchyEntities.clear();
			m_CutEntities.clear();
			m_LastInteractedSceneName.clear();
			++m_SelectionVersion;
		};
		dispatcher.Dispatch<ScenePreStopEvent>([&resetEditorEntityState](ScenePreStopEvent&) {
			resetEditorEntityState();
			return false;
			});
		dispatcher.Dispatch<ScenePostStartEvent>([&resetEditorEntityState](ScenePostStartEvent&) {
			// Belt-and-braces: additive loads skip PreStop, so PostStart must also clear.
			resetEditorEntityState();
			return false;
			});
	}

	void ImGuiEditorLayer::OnUpdate(Application& app, float dt) {
		// Scope the project's custom game cursor to the Game View. The engine
		// cursor gate keeps it off elsewhere; NoMouseCursorChange lets ImGui own
		// the cursor over the rest of the editor and reassert cleanly on exit.
		// Uses last frame's hover/focus — a 1-frame settle is imperceptible.
		if (Window* win = app.GetWindow(); win && win->HasGameCursor()) {
			ImGuiIO& io = ImGui::GetIO();
			const bool overGameView = m_IsGameViewHovered || m_IsGameViewFocused;
			if (overGameView) {
				io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
				win->SetGameCursorEnabled(true);
			}
			else {
				io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
				win->SetGameCursorEnabled(false);
			}
		}

		DrainPendingLogEntries();
		RunAutoSaveTick(app, dt);
		// MUST precede RunPrefabAutoSaveTick: if the prefab file was deleted, exit edit mode first so auto-save doesn't recreate it at a stale path.
		if (m_PrefabEditScene && !m_PrefabEditPath.empty() && !File::Exists(m_PrefabEditPath)) {
			ClosePrefabEditing(false);
		}
		RunPrefabAutoSaveTick();

		if (IndexProject* project = ProjectManager::GetCurrentProject()) {
			if (!project->AssetsDirectory.empty() && project->AssetsDirectory != m_AssetWatcherRoot) {
				m_AssetWatcherRoot = project->AssetsDirectory;
				m_AssetWatcher.Watch(
					std::vector<std::string>{ m_AssetWatcherRoot },
					std::vector<std::string>{ ".png", ".jpg", ".jpeg", ".bmp", ".tga" },
					[this]() {
						const size_t reloaded = TextureManager::ReloadTexturesFromDisk();
						AssetRegistry::MarkDirty();
						AssetRegistry::Sync();
						m_AssetBrowser.RequestRefresh();
						// Clear thumbnail tile cache too — otherwise the asset
						// browser keeps showing the pre-edit file icon until
						// LRU evicts the stale entry.
						m_AssetBrowser.InvalidateAllThumbnails();
						ClearPreviewTextureCache();
						if (reloaded > 0) {
							IDX_INFO_TAG("Editor", "Hot-reloaded {} texture(s)", reloaded);
						}
					});
			}
			m_AssetWatcher.Poll(0.25f);
		}
		else if (!m_AssetWatcherRoot.empty()) {
			m_AssetWatcher.Stop();
			m_AssetWatcherRoot.clear();
		}

		// Script-driven Application.Quit() in editor routes through the same stop path as the Stop button.
		if (Application::GetIsPlaying() && ApplicationEditorAccess::ConsumeQuitStopPlayRequest()) {
			Scene* act = SceneManager::Get().GetActiveScene();
			if (act) {
				for (auto [ent, audio] : act->GetRegistry().view<AudioSourceComponent>().each()) {
					if (audio.IsPlaying() || audio.IsPaused()) audio.Stop();
				}
			}
			RestoreEditorSceneAfterPlaymode();
		}

		Input& input = app.GetInput();
		Scene* activeScene = app.GetSceneManager() ? app.GetSceneManager()->GetActiveScene() : nullptr;
		if (!activeScene) {
			return;
		}

		Scene& scene = *activeScene;

		// CRITICAL: validate against GetContextScene(), not activeScene — in prefab-edit mode the selection lives in the detached prefab registry; using activeScene would clear the selection every frame.
		Scene* selectionContextScene = GetContextScene();
		if (!selectionContextScene) selectionContextScene = activeScene;
		Scene& selectionScene = *selectionContextScene;

		if (m_SelectedEntity != entt::null && !selectionScene.IsValid(m_SelectedEntity)) {
			m_SelectedEntity = entt::null;
		}
		if (m_PressedEntity != entt::null && !selectionScene.IsValid(m_PressedEntity)) {
			m_PressedEntity = entt::null;
		}
		if (!m_SelectedEntities.empty()) {
			auto isStale = [&selectionScene](EntityHandle h) {
				return h == entt::null || !selectionScene.IsValid(h);
			};
			const std::size_t before = m_SelectedEntities.size();
			m_SelectedEntities.erase(
				std::remove_if(m_SelectedEntities.begin(), m_SelectedEntities.end(), isStale),
				m_SelectedEntities.end());
			if (m_SelectedEntities.size() != before) {
				RebuildSelectionSet();
			}
		}
		UpdateEditorCameraFocus(dt);
		const bool hasShortcutFocus = HasEntityShortcutFocus();
		const ImGuiIO& io = ImGui::GetIO();
		// Shortcut handlers use the context scene so they work on prefab entities in prefab-edit mode.
		Scene& shortcutScene = selectionScene;
		const bool hasEntitySelection = !GetSelectedEntities(shortcutScene).empty();

		if (!io.WantTextInput && !ImGui::IsAnyItemActive()
			&& input.GetKeyDown(KeyCode::F) && hasShortcutFocus && hasEntitySelection) {
			FocusSelectedEntity(shortcutScene);
		}

		if (!hasShortcutFocus) {
			return;
		}
		if (m_RenamingEntity != entt::null || io.WantTextInput || ImGui::IsAnyItemActive()) {
			return;
		}

		if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper
			&& ImGui::IsKeyPressed(ImGuiKey_C, false)) {
			FinishCreatedEditorEntity(shortcutScene, Entity::Null, shortcutScene.CreateEmptyEntity());
			return;
		}

		// Arrow-key hierarchy navigation follows the currently rendered row list.
		if (m_IsEntitiesPanelFocused && hasEntitySelection && !m_VisibleEntityOrder.empty()) {
			const int direction = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ? 1
				: (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) ? -1 : 0);
			if (direction != 0) {
				int currentIndex = -1;
				for (int i = 0; i < static_cast<int>(m_VisibleEntityOrder.size()); ++i) {
					if (m_VisibleEntityOrder[static_cast<std::size_t>(i)] == m_SelectedEntity) {
						currentIndex = i;
						break;
					}
				}

				if (currentIndex < 0) {
					for (EntityHandle selected : GetSelectedEntities(shortcutScene)) {
						for (int i = 0; i < static_cast<int>(m_VisibleEntityOrder.size()); ++i) {
							if (m_VisibleEntityOrder[static_cast<std::size_t>(i)] == selected) {
								currentIndex = i;
								break;
							}
						}
						if (currentIndex >= 0) break;
					}
				}

				if (currentIndex >= 0) {
					const int targetIndex = std::clamp(
						currentIndex + direction,
						0,
						static_cast<int>(m_VisibleEntityOrder.size()) - 1);
					const EntityHandle target = m_VisibleEntityOrder[static_cast<std::size_t>(targetIndex)];
					if (target != entt::null && shortcutScene.IsValid(target)) {
						SetSingleEntitySelection(target, targetIndex);
					}
				}
			}
		}

		// Use io.KeyCtrl + ImGui::IsKeyPressed (not raw Input): covers right Ctrl/Cmd and prevents auto-repeat.
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && hasEntitySelection) {
			CopySelectedEntities(shortcutScene);
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && hasEntitySelection) {
			CutSelectedEntities(shortcutScene);
			return;
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
			PasteEntities(shortcutScene);
		}
		// Esc cancels a pending Cut: entities stay alive and un-dim. The
		// clipboard JSON itself is left intact so Ctrl+V still pastes a
		// copy, matching the behaviour of typical OS file managers.
		if (!m_CutEntities.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)
			&& m_RenamingEntity == entt::null) {
			m_CutEntities.clear();
		}
		if (!hasEntitySelection) {
			return;
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
			DuplicateSelectedEntity(shortcutScene);
		}
		if (input.GetKeyDown(KeyCode::Delete) || input.GetKeyDown(KeyCode::KpDecimal)) {
			DeleteSelectedEntity(shortcutScene);
		}
		if (input.GetKeyDown(KeyCode::F2)) {
			BeginRenameSelectedEntity(shortcutScene);
		}
	}

	void ImGuiEditorLayer::DrainPendingLogEntries() {
		if (!m_LogDispatchState) {
			return;
		}

		std::vector<LogEntry> pendingEntries;
		{
			std::scoped_lock lock(m_LogDispatchState->Mutex);
			if (m_LogDispatchState->PendingEntries.empty()) {
				return;
			}

			pendingEntries.swap(m_LogDispatchState->PendingEntries);
		}

		for (LogEntry& entry : pendingEntries) {
			AppendLogEntry(std::move(entry));
		}
	}

	void ImGuiEditorLayer::AppendLogEntry(LogEntry entry) {
		if (entry.Level >= Log::Level::Warn) {
			if (std::optional<LogSourceLocation> location = FindLogSourceLocation(entry.Message)) {
				entry.SourceFilePath = std::move(location->FilePath);
				entry.SourceLinkText = std::move(location->LinkText);
				entry.SourceLine = location->Line;
				entry.SourceLinkStart = location->MessageStart;
				entry.SourceLinkEnd = location->MessageEnd;
			}
		}

		m_LogEntries.push_back(std::move(entry));
		if (m_LogEntries.size() > 2000) {
			m_LogEntries.erase(m_LogEntries.begin(), m_LogEntries.begin() + 500);
		}
	}

	void ImGuiEditorLayer::RunAutoSaveTick(Application& app, float dt) {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project || !EditorPreferences::GetAutoSaveScenes()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Skip in Play mode: the pre-play snapshot was already saved; auto-saving mid-play would persist transient mutations that Stop discards.
		if (Application::GetIsPlaying()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Reset accumulator on scene change so the new scene gets a full interval before its first auto-save.
		Scene* active = app.GetSceneManager() ? app.GetSceneManager()->GetActiveScene() : nullptr;
		if (!active) {
			m_AutoSaveAccumulator = 0.0f;
			m_AutoSaveLastScenePath.clear();
			return;
		}
		const std::string scenePath = project->GetSceneFilePath(active->GetName());
		if (scenePath != m_AutoSaveLastScenePath) {
			m_AutoSaveAccumulator = 0.0f;
			m_AutoSaveLastScenePath = scenePath;
		}

		if (!active->IsDirty()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		m_AutoSaveAccumulator += dt;
		const float interval = std::max(5.0f, EditorPreferences::GetAutoSaveIntervalSeconds());
		if (m_AutoSaveAccumulator < interval) {
			return;
		}
		m_AutoSaveAccumulator = 0.0f;

		if (scenePath.empty()) {
			return;
		}
		if (SceneSerializer::SaveToFile(*active, scenePath, false)) {
			active->ClearDirty();
		}
	}

	void ImGuiEditorLayer::RunPrefabAutoSaveTick() {
		if (!m_PrefabEditScene) return;
		if (!EditorPreferences::GetAutoSavePrefabs()) return;
		if (!m_PrefabEditScene->IsDirty()) return;
		// Debounce: skip while any ImGui widget is active (e.g. dragging a slider) to avoid thrashing disk on every intermediate value.
		if (ImGui::IsAnyItemActive()) return;

		SavePrefabEditChanges();
	}

	void ImGuiEditorLayer::ClearLogEntries() {
		m_LogEntries.clear();
		if (m_LogDispatchState) {
			std::scoped_lock lock(m_LogDispatchState->Mutex);
			m_LogDispatchState->PendingEntries.clear();
		}
	}

	// ──────────────────────────────────────────────
	//  Dockspace & Menu
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::RenderDockspaceRoot() {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		// MUST precede dockspace Begin: SetSize above mutates MainViewport that UI systems fall back to.
		const float titlebarH = EditorRuntime::GetTitlebarRowHeight();
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + titlebarH));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - titlebarH));
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

		ImGui::Begin("EditorDockspace", nullptr, flags);
		ImGui::PopStyleVar(3);

		ImGuiID dockspaceId = ImGui::GetID("IndexEditorDockspace");
		ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
		// End() is in OnPreRender after RenderMainMenu — must stay open for the menubar.
	}

	void ImGuiEditorLayer::RenderMainMenu(Scene& scene) {
		if (!ImGui::BeginMenuBar()) {
			return;
		}


		if (ImGui::BeginMenu("Application")) {
			if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !Application::GetIsPlaying())) {
				// Save all dirty loaded scenes; fall back to active scene so the menu acts as a force-save even when nothing is dirty.
				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project) {
					bool savedAny = false;
					for (auto& weakScene : SceneManager::Get().GetLoadedScenes()) {
						auto loadedScene = weakScene.lock();
						if (!loadedScene || !loadedScene->IsDirty()) continue;
						std::string scenePath = project->GetSceneFilePath(loadedScene->GetName());
						if (SceneSerializer::SaveToFile(*loadedScene, scenePath)) {
							savedAny = true;
						}
					}
					if (!savedAny) {
						std::string scenePath = project->GetSceneFilePath(scene.GetName());
						SceneSerializer::SaveToFile(scene, scenePath);
					}
					project->LastOpenedScene = scene.GetName();
					project->Save();
				}
			}

			if (ImGui::MenuItem("Quit")) {
				Application::RequestQuit();
			}
			if (ImGui::MenuItem("Reload App")) {
				Application::Reload();
			}
			ImGui::EndMenu();
		}




		if (ImGui::BeginMenu("Project")) {
			bool hasProject = ProjectManager::HasProject();
			if (ImGui::MenuItem("Build", nullptr, false, hasProject && !Application::GetIsPlaying())) {
				m_ShowBuildPanel = true;
				if (m_BuildOutputDir.empty()) {
					IndexProject* project = ProjectManager::GetCurrentProject();
					if (project) {
						m_BuildOutputDir = Path::Combine(project->RootDirectory, "Builds", "Windows");
						std::snprintf(m_BuildOutputDirBuffer, sizeof(m_BuildOutputDirBuffer), "%s", m_BuildOutputDir.c_str());
					}
				}
			}
			if (ImGui::MenuItem("Build Profiles…", nullptr, false, hasProject)) {
				m_ShowBuildProfilesPanel = true;
			}
			if (ImGui::MenuItem("Project Settings", nullptr, false, hasProject)) {
				m_ShowProjectSettings = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Package Manager", nullptr, false, hasProject)) {
				m_ShowPackageManager = true;
			}

			ImGui::EndMenu();
		}

		// Edit — user-scoped editor preferences (theme, fonts, layout
		// presets, external script editor, asset-browser / auto-save
		// toggles). All routed through EditorPreferencesPanel.
		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Preferences...")) {
				m_ShowEditorPreferences = true;
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Tools")) {
			ImGui::MenuItem("Profiler", "Ctrl+F6", &m_ShowProfiler);
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	static bool IconButton(const char* id, const char* iconName, float iconSize, const ImVec2& btnSize) {
		uint64_t tex = EditorIcons::Get(iconName, (int)iconSize);
		if (tex)
			return ImGui::ImageButton(id, (ImTextureID)(intptr_t)tex, btnSize, ImVec2(0, 1), ImVec2(1, 0));
		return ImGui::Button(id + 2);
	}

	void ImGuiEditorLayer::BeginPlayModeRequest(Scene& scene) {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			// Snapshot every loaded scene (not just active): restore reloads each from disk, so all must be current at play-entry.
			m_PlayModeScenes.clear();
			m_PlayModeActiveScene = scene.GetName();
			bool savedAny = false;
			SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
				std::string scenePath = project->GetSceneFilePath(s.GetName());
				if (s.IsDirty()) {
					SceneSerializer::SaveToFile(s, scenePath);
					savedAny = true;
				}
				m_PlayModeScenes.push_back({ s.GetName(), scenePath });
			});
			if (savedAny) {
				project->LastOpenedScene = scene.GetName();
				project->Save();
			}
		}

		m_LogEntries.clear();
		ApplicationEditorAccess::SetPlaymodePaused(false);

		ScriptSystem* scriptSys = scene.HasSystem<ScriptSystem>() ? scene.GetSystem<ScriptSystem>() : nullptr;
		if (scriptSys && EditorPreferences::GetRecompileScriptsOnPlay()) {
			const bool rebuildStarted = scriptSys->RequestRebuildAndReloadAll();
			if (rebuildStarted || scriptSys->IsRebuilding()) {
				m_PlayModeRecompilePending = true;
				return;
			}
		}
		if (scriptSys && scriptSys->IsRebuilding()) {
			m_PlayModeRecompilePending = true;
			return;
		}
		if (scriptSys && !scriptSys->DidLastRebuildSucceed()) {
			IDX_ERROR_TAG("PlayMode", "Script compilation failed. Play Mode was not started.");
			return;
		}

		CompletePlayModeEntry(scene);
	}

	void ImGuiEditorLayer::CompletePlayModeEntry(Scene& scene) {
		m_PlayModeRecompilePending = false;
		// Step counter is panel-frame state, not playmode state — without a reset
		// here a Step that didn't finish draining before the previous Stop would
		// trip the bottom-of-RenderToolbar auto-pause on this fresh session's first
		// frame, freezing the new Play instantly.
		m_StepFrames = 0;
		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(true);
		// Reset Time.TimeSinceStartup / Time.RealtimeSinceStartup so each
		// play session starts at t=0 from a script's perspective.
		ApplicationEditorAccess::MarkGameStart();
		scene.StartManagedSceneScriptsForPlayMode();
		// Reset Box2D sleep timers so bodies that sat through editor
		// idle don't immediately freeze on the first physics step.
		if (PhysicsSystem2D::IsInitialized()) {
			PhysicsSystem2D::WakeAllBodies();
		}
		StartPlayOnAwakeComponents(scene);
	}

	void ImGuiEditorLayer::PollPendingPlayModeRequest(Scene& scene) {
		if (!m_PlayModeRecompilePending || Application::GetIsPlaying()) {
			return;
		}

		ScriptSystem* scriptSys = scene.HasSystem<ScriptSystem>() ? scene.GetSystem<ScriptSystem>() : nullptr;
		if (!scriptSys || !scriptSys->IsRebuilding()) {
			if (!scriptSys || scriptSys->DidLastRebuildSucceed()) {
				CompletePlayModeEntry(scene);
			}
			else {
				m_PlayModeRecompilePending = false;
				IDX_ERROR_TAG("PlayMode", "Script compilation failed. Play Mode was not started.");
			}
		}
	}

	void ImGuiEditorLayer::RenderToolbar() {
		ImGui::Begin("Toolbar");

		const float iconSize = ImGui::GetTextLineHeight();
		const ImVec2 btnSize(iconSize, iconSize);
		const bool isPlaying = Application::GetIsPlaying();
		const bool isPaused = ApplicationEditorAccess::IsPlaymodePaused();

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));

		if (!isPlaying) {
			const bool playButtonDisabled = m_PlayModeRecompilePending;
			if (playButtonDisabled) {
				ImGui::BeginDisabled();
			}
			if (IconButton("##Play", "play", iconSize, btnSize)) {
				Scene* active = SceneManager::Get().GetActiveScene();
				if (active) {
					BeginPlayModeRequest(*active);
				}
			}
			if (playButtonDisabled) {
				ImGui::EndDisabled();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("Play (Enter playmode)");
			}
		}
		else {
			if (isPaused) {
				if (IconButton("##Continue", "play", iconSize, btnSize)) {
					ApplicationEditorAccess::SetPlaymodePaused(false);
					Scene* active = SceneManager::Get().GetActiveScene();
					if (active) {
						for (auto ent : m_EditorPausedAudioEntities) {
							if (active->IsValid(ent) && active->HasComponent<AudioSourceComponent>(ent)) {
								auto& audio = active->GetComponent<AudioSourceComponent>(ent);
								if (audio.IsPaused()) audio.Resume();
							}
						}
					}
					m_EditorPausedAudioEntities.clear();
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Continue");
			}
			else {
				if (IconButton("##Pause", "pause", iconSize, btnSize)) {
					ApplicationEditorAccess::SetPlaymodePaused(true);
					m_EditorPausedAudioEntities.clear();
					Scene* active = SceneManager::Get().GetActiveScene();
					if (active) {
						for (auto [ent, audio] : active->GetRegistry().view<AudioSourceComponent>().each()) {
							if (audio.IsPlaying()) {
								audio.Pause();
								m_EditorPausedAudioEntities.push_back(ent);
							}
						}
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause");
			}

			ImGui::SameLine();
			if (IconButton("##Step", "step_forward", iconSize, btnSize)) {
				ApplicationEditorAccess::SetPlaymodePaused(false);
				m_StepFrames = 2;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step (advance one frame)");

			ImGui::SameLine();
			if (IconButton("##Stop", "stop", iconSize, btnSize)) {
				{
					Scene* act = SceneManager::Get().GetActiveScene();
					if (act) {
						for (auto [ent, audio] : act->GetRegistry().view<AudioSourceComponent>().each()) {
							if (audio.IsPlaying() || audio.IsPaused()) audio.Stop();
						}
					}
				}

				RestoreEditorSceneAfterPlaymode();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop (exit playmode)");
		}

		// Disabled mid-build to avoid racing the file-copy step that touches the runtime binary.
		ImGui::SameLine();
		const bool standaloneDisabled = (m_BuildState > 0)
			|| !ProjectManager::GetCurrentProject();
		if (standaloneDisabled) ImGui::BeginDisabled();
		if (IconButton("##LaunchStandalone", "launch_standalone", iconSize, btnSize)) {
			LaunchStandalone();
		}
		if (standaloneDisabled) ImGui::EndDisabled();

		ImGui::PopStyleVar();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (m_BuildState > 0)
				ImGui::SetTooltip("Launch Standalone (disabled — build in progress)");
			else if (!ProjectManager::GetCurrentProject())
				ImGui::SetTooltip("Launch Standalone (disabled — no project loaded)");
			else
				ImGui::SetTooltip(
					"Saves the project and spawns Index-Runtime.exe in a separate\n"
					"window. Use this to verify aspect-lock, OS input, fullscreen\n"
					"mode, and other behaviour the Game View can't fully mirror.");
		}
		ImGui::SameLine();
		const char* statusText = !isPlaying ? "Editor" : (isPaused ? "Paused" : "Playing");
		ImGui::TextUnformatted(statusText);
		if (m_PlayModeRecompilePending) {
			ImGui::SameLine();
			ImGui::TextDisabled("Waiting for script compilation...");
		}

		ImGui::End();

		// Live playing flag (not the top-of-function snapshot): a Stop click
		// earlier in this frame already cleared m_IsPlaying via
		// RestoreEditorSceneAfterPlaymode + reset m_StepFrames, but if a
		// future handler resets one and not the other, the live read keeps
		// us from pausing after the editor has left playmode.
		if (Application::GetIsPlaying() && m_StepFrames > 0) {
			m_StepFrames--;
			if (m_StepFrames == 0) {
				ApplicationEditorAccess::SetPlaymodePaused(true);
				// Audio runs on its own thread; mirror the Pause button's manual capture so Continue can resume it.
				Scene* active = SceneManager::Get().GetActiveScene();
				if (active) {
					for (auto [ent, audio] : active->GetRegistry().view<AudioSourceComponent>().each()) {
						if (audio.IsPlaying()) {
							audio.Pause();
							m_EditorPausedAudioEntities.push_back(ent);
						}
					}
				}
			}
		}
	}

	void ImGuiEditorLayer::LaunchStandalone() {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			IDX_CORE_WARN_TAG("Editor",
				"LaunchStandalone: no project loaded — nothing to launch.");
			return;
		}

		// Tries two layouts: dev-tree sibling dir, then same dir as editor exe.
		const std::filesystem::path exeDir(Path::ExecutableDir());
#ifdef IDX_PLATFORM_WINDOWS
		constexpr const char* k_RuntimeExe = "Index-Runtime.exe";
#else
		constexpr const char* k_RuntimeExe = "Index-Runtime";
#endif
		std::filesystem::path runtimePath = exeDir.parent_path() / "Index-Runtime" / k_RuntimeExe;
		if (!std::filesystem::exists(runtimePath)) {
			runtimePath = exeDir / k_RuntimeExe;
		}
		if (!std::filesystem::exists(runtimePath)) {
			IDX_CORE_ERROR_TAG("Editor",
				"LaunchStandalone: Index-Runtime executable not found near editor "
				"(tried '{}' and '{}'). Build the engine's Runtime project first.",
				(exeDir.parent_path() / "Index-Runtime" / k_RuntimeExe).string(),
				(exeDir / k_RuntimeExe).string());
			return;
		}

		// Flush project settings; scene files are not auto-saved (standalone shows on-disk state).
		project->Save();

		const std::string runtimeStr = runtimePath.string();
		const std::string projectArg = "--project=" + project->RootDirectory;
		IDX_CORE_INFO_TAG("Editor",
			"LaunchStandalone: spawning '{}' for project '{}'",
			runtimeStr, project->Name);

		if (!Process::LaunchDetached({ runtimeStr, projectArg },
			runtimePath.parent_path()))
		{
			IDX_CORE_ERROR_TAG("Editor",
				"LaunchStandalone: failed to spawn '{}' (Process::LaunchDetached "
				"returned false). See earlier log for the OS-level reason.",
				runtimeStr);
		}
	}

	void ImGuiEditorLayer::RestoreEditorSceneAfterPlaymode() {
		// Reset before reload: m_PressedEntity holds a play-mode handle that entt may recycle, causing a stale click on the restored entity.
		m_PressedEntity = entt::null;

		// Prefab-edit mode: selection lives in the detached scene which play doesn't touch; skip the clear/restore dance.
		const bool inPrefabEdit = IsInPrefabEditMode();

		const bool wasSceneNodeSelected = m_IsSceneNodeSelected;
		uint64_t selectedUUID = 0;
		Scene* active = SceneManager::Get().GetActiveScene();
		if (!inPrefabEdit
			&& active && m_SelectedEntity != entt::null && active->IsValid(m_SelectedEntity)
			&& active->HasComponent<UUIDComponent>(m_SelectedEntity)) {
			selectedUUID = static_cast<uint64_t>(active->GetComponent<UUIDComponent>(m_SelectedEntity).Id);
		}

		if (!inPrefabEdit) {
			m_SelectedEntity = entt::null;
			m_SelectedEntities.clear();
			m_SelectedEntitySet.clear();
			++m_SelectionVersion;
			m_LastEntitySelectionIndex = -1;
			m_IsSceneNodeSelected = false;
			m_RenamingEntity = entt::null;
			m_EntityOrder.clear(); m_EntityOrderDirty = true;
		}
		m_PlayModeRecompilePending = false;
		// Drop the pending Step so a click-Step→click-Stop in adjacent frames
		// doesn't leave a stale counter that auto-pauses the next session.
		m_StepFrames = 0;

		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(false);

		if (!m_PlayModeScenes.empty()) {
			// Snapshot-set restore. The legacy version only iterated currently-
			// loaded scenes and reloaded ones whose names matched the snapshot —
			// that left scripts' `LoadScene` results sitting in the editor and
			// dropped any pre-play scene the script had replaced. We instead:
			//   1. unload everything currently loaded that's not in the snapshot
			//      (those were brought in by play-mode scripts),
			//   2. for each snapshot entry, reload in place if still loaded,
			//      otherwise load it back via SceneManager + LoadFromFile,
			//   3. restore the active scene to whatever was active before play.
			std::vector<std::string> currentlyLoaded;
			SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
				currentlyLoaded.push_back(s.GetName());
			});

			std::unordered_set<std::string> expected;
			expected.reserve(m_PlayModeScenes.size());
			for (const auto& [name, _path] : m_PlayModeScenes) {
				expected.insert(name);
			}

			for (const std::string& name : currentlyLoaded) {
				if (expected.find(name) == expected.end()) {
					SceneManager::Get().UnloadScene(name);
				}
			}

			std::unordered_set<std::string> stillLoaded;
			SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
				stillLoaded.insert(s.GetName());
			});

			for (const auto& [sceneName, scenePath] : m_PlayModeScenes) {
				if (stillLoaded.count(sceneName) > 0) {
					if (auto loaded = SceneManager::Get().GetLoadedScene(sceneName).lock()) {
						SceneSerializer::LoadFromFile(*loaded, scenePath);
					}
				}
				else {
					if (auto loaded = SceneManager::Get().LoadSceneAdditive(sceneName).lock()) {
						SceneSerializer::LoadFromFile(*loaded, scenePath);
					}
				}
			}

			if (!m_PlayModeActiveScene.empty()) {
				SceneManager::Get().SetActiveScene(m_PlayModeActiveScene);
			}

			m_PlayModeScenes.clear();
			m_PlayModeActiveScene.clear();
		}

		// MUST run after scene reload so OnDisable thunks fire first; well-behaved scripts that pair +=/-= leave nothing for this sweep.
		ScriptEngine::OnPlayModeExited();

		if (inPrefabEdit) {
			return;
		}

		if (selectedUUID == 0) {
			if (wasSceneNodeSelected) {
				SelectSceneNode();
			}
			return;
		}

		active = SceneManager::Get().GetActiveScene();
		if (!active) {
			return;
		}

		auto view = active->GetRegistry().view<UUIDComponent>();
		for (auto [ent, uuid] : view.each()) {
			if (static_cast<uint64_t>(uuid.Id) == selectedUUID) {
				SelectEntity(ent);
				break;
			}
		}
	}

	void ImGuiEditorLayer::SelectSceneNode() {
		ResetEditorFocusCycle();
		m_SelectedEntity = entt::null;
		m_PressedEntity = entt::null;
		m_SelectedEntities.clear();
		m_SelectedEntitySet.clear();
		++m_SelectionVersion;
		m_LastEntitySelectionIndex = -1;
		m_RenamingEntity = entt::null;
		m_EntityRenameFrameCounter = 0;
		m_IsSceneNodeSelected = true;
		m_AssetBrowser.ClearSelection();
	}

	void ImGuiEditorLayer::SelectEntity(EntityHandle entity) {
		ResetEditorFocusCycle();
		m_SelectedEntity = entity;
		m_SelectedEntities.clear();
		m_SelectedEntitySet.clear();
		if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
			m_SelectedEntitySet.insert(entity);
		}
		++m_SelectionVersion;
		m_LastEntitySelectionIndex = -1;
		m_IsSceneNodeSelected = false;
		if (entity != entt::null) {
			m_AssetBrowser.ClearSelection();
		}
	}

	void ImGuiEditorLayer::ClearEntitySelection() {
		ResetEditorFocusCycle();
		m_SelectedEntity = entt::null;
		m_PressedEntity = entt::null;
		m_SelectedEntities.clear();
		m_SelectedEntitySet.clear();
		++m_SelectionVersion;
		m_LastEntitySelectionIndex = -1;
		m_RenamingEntity = entt::null;
		m_EntityRenameFrameCounter = 0;
		m_IsSceneNodeSelected = false;
	}

	void ImGuiEditorLayer::RebuildSelectionSet() {
		m_SelectedEntitySet.clear();
		m_SelectedEntitySet.reserve(m_SelectedEntities.size());
		for (EntityHandle e : m_SelectedEntities) {
			if (e != entt::null) m_SelectedEntitySet.insert(e);
		}
		++m_SelectionVersion;
	}

	bool ImGuiEditorLayer::IsEntitySelected(EntityHandle entity) const {
		// O(1) lookup; linear scan over large multi-selections was a dominant frame cost.
		return m_SelectedEntitySet.find(entity) != m_SelectedEntitySet.end();
	}

	std::vector<EntityHandle> ImGuiEditorLayer::GetSelectedEntities(const Scene& scene) const {
		// Dedup via hash set; old std::find was O(N²) at 25k-entity selections.
		std::vector<EntityHandle> entities;
		entities.reserve(m_SelectedEntities.size());
		std::unordered_set<EntityHandle> seen;
		seen.reserve(m_SelectedEntities.size());
		for (EntityHandle entity : m_SelectedEntities) {
			if (entity == entt::null || !scene.IsValid(entity)) continue;
			if (seen.insert(entity).second) {
				entities.push_back(entity);
			}
		}

		if (entities.empty() && m_SelectedEntity != entt::null && scene.IsValid(m_SelectedEntity)) {
			entities.push_back(m_SelectedEntity);
		}

		return entities;
	}

	void ImGuiEditorLayer::SetSingleEntitySelection(EntityHandle entity, int index) {
		ResetEditorFocusCycle();
		m_SelectedEntity = entity;
		m_SelectedEntities.clear();
		m_SelectedEntitySet.clear();
		if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
			m_SelectedEntitySet.insert(entity);
			m_AssetBrowser.ClearSelection();
		}
		++m_SelectionVersion;
		m_LastEntitySelectionIndex = index;
		m_IsSceneNodeSelected = false;
	}

	void ImGuiEditorLayer::ToggleEntitySelection(EntityHandle entity, int index) {
		ResetEditorFocusCycle();
		auto setIt = m_SelectedEntitySet.find(entity);
		if (setIt != m_SelectedEntitySet.end()) {
			m_SelectedEntitySet.erase(setIt);
			auto it = std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity);
			if (it != m_SelectedEntities.end()) {
				m_SelectedEntities.erase(it);
			}
			if (m_SelectedEntity == entity) {
				m_SelectedEntity = m_SelectedEntities.empty() ? entt::null : m_SelectedEntities.back();
			}
		}
		else if (entity != entt::null) {
			m_SelectedEntities.push_back(entity);
			m_SelectedEntitySet.insert(entity);
			m_SelectedEntity = entity;
			m_AssetBrowser.ClearSelection();
		}

		++m_SelectionVersion;
		m_LastEntitySelectionIndex = index;
		m_IsSceneNodeSelected = false;
	}

	void ImGuiEditorLayer::SelectEntityRange(int index) {
		ResetEditorFocusCycle();
		const auto& order = !m_VisibleEntityOrder.empty() ? m_VisibleEntityOrder : m_EntityOrder;
		if (order.empty()) {
			return;
		}

		if (index < 0 || index >= static_cast<int>(order.size())) {
			return;
		}

		int anchorIndex = m_LastEntitySelectionIndex;
		if (anchorIndex < 0 || anchorIndex >= static_cast<int>(order.size())) {
			for (int i = 0; i < static_cast<int>(order.size()); ++i) {
				if (order[static_cast<std::size_t>(i)] == m_SelectedEntity) {
					anchorIndex = i;
					break;
				}
			}
		}
		if (anchorIndex < 0 || anchorIndex >= static_cast<int>(order.size())) {
			for (EntityHandle selected : m_SelectedEntities) {
				for (int i = 0; i < static_cast<int>(order.size()); ++i) {
					if (order[static_cast<std::size_t>(i)] == selected) {
						anchorIndex = i;
						break;
					}
				}
				if (anchorIndex >= 0) break;
			}
		}

		if (anchorIndex < 0 || anchorIndex >= static_cast<int>(order.size())) {
			SetSingleEntitySelection(order[static_cast<std::size_t>(index)], index);
			return;
		}

		const int first = std::min(anchorIndex, index);
		const int last = std::max(anchorIndex, index);
		m_SelectedEntities.clear();
		for (int i = first; i <= last; ++i) {
			m_SelectedEntities.push_back(order[static_cast<std::size_t>(i)]);
		}
		RebuildSelectionSet();
		m_SelectedEntity = order[static_cast<std::size_t>(index)];
		m_LastEntitySelectionIndex = index;
		m_IsSceneNodeSelected = false;
		m_AssetBrowser.ClearSelection();
	}

	void ImGuiEditorLayer::RecomputeInspectorSelectionCache(Scene& scene) {
		// Runs once per selection version bump; avoids O(N×M) sweep over (selection × components) every frame.
		m_InspectorCache.Handles = GetSelectedEntities(scene);
		m_InspectorCache.CommonComponentTypes.clear();
		m_InspectorCache.PartialComponents.clear();

		if (m_InspectorCache.Handles.empty()) {
			m_InspectorCache.NameUniform = true;
			m_InspectorCache.FirstName.clear();
			m_InspectorCache.EnabledUniform = true;
			m_InspectorCache.EnabledFirst = true;
			m_InspectorCache.StaticUniform = true;
			m_InspectorCache.StaticFirst = false;
			return;
		}

		std::vector<Entity> entities;
		entities.reserve(m_InspectorCache.Handles.size());
		for (EntityHandle h : m_InspectorCache.Handles) {
			entities.push_back(scene.GetEntity(h));
		}

		{
			// Empty FirstName means no NameComponent; InputText renders a hint instead of pre-filling to avoid accidental creation.
			Entity& first = entities[0];
			m_InspectorCache.FirstName = first.HasComponent<NameComponent>()
				? first.GetComponent<NameComponent>().Name
				: std::string{};
			m_InspectorCache.NameUniform = true;
			const std::string_view firstView = m_InspectorCache.FirstName;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				Entity& e = entities[i];
				const std::string_view name = e.HasComponent<NameComponent>()
					? std::string_view(e.GetComponent<NameComponent>().Name)
					: std::string_view{};
				if (name != firstView) { m_InspectorCache.NameUniform = false; break; }
			}
		}

		{
			auto enabledOf = [](Entity& e) {
				return !e.HasComponent<DisabledTag>() || e.HasComponent<InheritedDisabledTag>();
			};
			auto staticOf = [](Entity& e) { return e.HasComponent<StaticTag>(); };

			m_InspectorCache.EnabledFirst = enabledOf(entities[0]);
			m_InspectorCache.StaticFirst = staticOf(entities[0]);
			m_InspectorCache.EnabledUniform = true;
			m_InspectorCache.StaticUniform = true;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (m_InspectorCache.EnabledUniform && enabledOf(entities[i]) != m_InspectorCache.EnabledFirst) {
					m_InspectorCache.EnabledUniform = false;
				}
				if (m_InspectorCache.StaticUniform && staticOf(entities[i]) != m_InspectorCache.StaticFirst) {
					m_InspectorCache.StaticUniform = false;
				}
				if (!m_InspectorCache.EnabledUniform && !m_InspectorCache.StaticUniform) break;
			}
		}

		const auto& registry = SceneManager::Get().GetComponentRegistry();
		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return;

			std::size_t haveCount = 0;
			for (const Entity& e : entities) {
				if (info.has(e)) ++haveCount;
			}
			if (haveCount == 0) return;
			if (haveCount < entities.size()) {
				m_InspectorCache.PartialComponents.push_back(info.displayName);
			}
			else {
				m_InspectorCache.CommonComponentTypes.insert(typeId);
			}
		});
	}

	void ImGuiEditorLayer::ResetEditorFocusCycle() {
		m_EditorCameraFocusActive = false;
		m_EditorCameraFocusElapsed = 0.0f;
		m_FocusLastEntity = entt::null;
		m_FocusNextPressTight = false;
	}

	void ImGuiEditorLayer::UpdateEditorCameraFocus(float dt) {
		if (!m_EditorCameraFocusActive) {
			return;
		}

		// Fixed-duration (not exponential-decay) so perceived animation time is distance-independent.
		constexpr float k_FocusDuration = 0.25f;
		m_EditorCameraFocusElapsed += Max(0.0f, dt);
		const float t = Min(1.0f, m_EditorCameraFocusElapsed / k_FocusDuration);
		const float eased = t * t * (3.0f - 2.0f * t);

		const Vec2 nextPosition = Lerp(m_EditorCameraFocusStartPosition, m_EditorCameraFocusTarget, eased);
		const float nextSize = m_EditorCameraFocusStartOrthoSize
			+ (m_EditorCameraFocusOrthoSize - m_EditorCameraFocusStartOrthoSize) * eased;

		m_EditorCamera.SetPosition(nextPosition);
		m_EditorCamera.SetOrthographicSize(nextSize);

		if (t >= 1.0f) {
			m_EditorCamera.SetPosition(m_EditorCameraFocusTarget);
			m_EditorCamera.SetOrthographicSize(m_EditorCameraFocusOrthoSize);
			m_EditorCameraFocusActive = false;
			m_EditorCameraFocusElapsed = 0.0f;
		}
	}

	bool ImGuiEditorLayer::TryBuildEntityAABB(Scene& scene, EntityHandle entity, bool includeChildren, AABB& outAABB) {
		if (entity == entt::null || !scene.IsValid(entity)) {
			return false;
		}

		TransformHierarchySystem::Propagate(scene);
		ComputeUILayout(scene);

		bool hasBounds = false;
		auto includeAABB = [&](const AABB& aabb) {
			if (!hasBounds) {
				outAABB = aabb;
				hasBounds = true;
				return;
			}
			outAABB.Min.x = std::min(outAABB.Min.x, aabb.Min.x);
			outAABB.Min.y = std::min(outAABB.Min.y, aabb.Min.y);
			outAABB.Max.x = std::max(outAABB.Max.x, aabb.Max.x);
			outAABB.Max.y = std::max(outAABB.Max.y, aabb.Max.y);
		};

		std::unordered_set<uint32_t> visited;
		std::function<void(EntityHandle)> visit = [&](EntityHandle current) {
			if (current == entt::null || !scene.IsValid(current)) {
				return;
			}
			if (!visited.insert(static_cast<uint32_t>(current)).second) {
				return;
			}

			Transform2DComponent* transform = nullptr;
			if (scene.TryGetComponent<Transform2DComponent>(current, transform) && transform) {
				includeAABB(AABB::FromTransform(*transform));
			}

			RectTransform2DComponent* rect = nullptr;
			if (scene.TryGetComponent<RectTransform2DComponent>(current, rect) && rect) {
				const float worldScale = GuiRenderer::ComputeWorldUIPixelScale();
				const Vec2 bl = rect->GetBottomLeft();
				const Vec2 tr = rect->GetTopRight();
				const Vec2 pivot = rect->ResolvedValid ? rect->ResolvedPivot
					: Vec2{ (bl.x + tr.x) * 0.5f, (bl.y + tr.y) * 0.5f };

				Vec2 corners[4] = {
					Vec2{ bl.x * worldScale, bl.y * worldScale },
					Vec2{ tr.x * worldScale, bl.y * worldScale },
					Vec2{ tr.x * worldScale, tr.y * worldScale },
					Vec2{ bl.x * worldScale, tr.y * worldScale },
				};

				if (rect->Rotation != 0.0f) {
					const Vec2 worldPivot{ pivot.x * worldScale, pivot.y * worldScale };
					for (Vec2& corner : corners) {
						corner = worldPivot + Rotated(corner - worldPivot, rect->Rotation);
					}
				}

				Vec2 minPoint = corners[0];
				Vec2 maxPoint = corners[0];
				for (int i = 1; i < 4; ++i) {
					minPoint.x = std::min(minPoint.x, corners[i].x);
					minPoint.y = std::min(minPoint.y, corners[i].y);
					maxPoint.x = std::max(maxPoint.x, corners[i].x);
					maxPoint.y = std::max(maxPoint.y, corners[i].y);
				}
				includeAABB({ minPoint, maxPoint });
			}

			if (includeChildren && scene.HasComponent<HierarchyComponent>(current)) {
				for (EntityHandle child : scene.GetComponent<HierarchyComponent>(current).Children) {
					visit(child);
				}
			}
		};

		visit(entity);
		return hasBounds;
	}

	void ImGuiEditorLayer::FocusSelectedEntity(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		const bool tightFocus = m_FocusLastEntity == m_SelectedEntity && m_FocusNextPressTight;
		AABB bounds;
		if (!TryBuildEntityAABB(scene, m_SelectedEntity, !tightFocus, bounds)) {
			return;
		}

		const Vec2 size = bounds.Scale();
		const Vec2 center{
			(bounds.Min.x + bounds.Max.x) * 0.5f,
			(bounds.Min.y + bounds.Max.y) * 0.5f
		};

		const AABB viewport = m_EditorCamera.GetViewportAABB();
		const Vec2 viewportSize = viewport.Scale();
		const float aspect = viewportSize.y > 0.0001f ? viewportSize.x / viewportSize.y : 16.0f / 9.0f;
		const float paddedHalfWidth = std::max(std::abs(size.x) * 0.5f, 0.5f) * 1.25f;
		const float paddedHalfHeight = std::max(std::abs(size.y) * 0.5f, 0.5f) * 1.25f;
		const float targetHalfHeight = std::max(paddedHalfHeight, paddedHalfWidth / std::max(aspect, 0.0001f));
		const float zoom = std::max(m_EditorCamera.GetZoom(), 0.0001f);

		m_EditorCameraFocusTarget = center;
		m_EditorCameraFocusOrthoSize = Clamp(targetHalfHeight / zoom,
			EditorCamera::k_MinOrthographicSize, EditorCamera::k_MaxOrthographicSize);
		m_EditorCameraFocusStartPosition = m_EditorCamera.GetPosition();
		m_EditorCameraFocusStartOrthoSize = m_EditorCamera.GetOrthographicSize();
		m_EditorCameraFocusElapsed = 0.0f;
		m_EditorCameraFocusActive = true;
		m_FocusLastEntity = m_SelectedEntity;
		m_FocusNextPressTight = !tightFocus;
	}

	void ImGuiEditorLayer::DuplicateSelectedEntity(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		if (selectedEntities.empty()) {
			return;
		}

		std::vector<EntityHandle> createdEntities;
		for (EntityHandle sourceEntity : selectedEntities) {
			Json::Value entityValue = SceneSerializer::SerializeEntityForClipboard(scene, sourceEntity);
			if (!entityValue.IsObject()) {
				continue;
			}

			EntityHandle clone = SceneSerializer::DeserializeEntityFromValue(scene, entityValue);
			if (clone != entt::null) {
				Entity source = scene.GetEntity(sourceEntity);
				Entity sourceParent = source.GetParent();
				if (sourceParent.IsValid()) {
					Entity cloneEntity = scene.GetEntity(clone);
					cloneEntity.SetParent(sourceParent);
					MoveSiblingNextTo(scene, clone, sourceEntity, true);
					m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(sourceParent.GetHandle()));
				}
				createdEntities.push_back(clone);
			}
		}

		if (!createdEntities.empty()) {
			EnsureEditorUniqueEntityNames(scene, createdEntities);
			m_SelectedEntities = createdEntities;
			RebuildSelectionSet();
			m_SelectedEntity = createdEntities.back();
			m_LastEntitySelectionIndex = -1;
			m_IsSceneNodeSelected = false;
			scene.MarkDirty();
			m_EntityOrder.clear(); m_EntityOrderDirty = true;
		}
	}

	void ImGuiEditorLayer::DeleteSelectedEntity(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = GetSelectedEntities(scene);
		if (selectedEntities.empty()) {
			return;
		}

		m_PressedEntity = entt::null;

		for (EntityHandle entity : selectedEntities) {
			if (scene.IsValid(entity)) {
				scene.DestroyEntity(entity);
			}
		}
		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
	}

	void ImGuiEditorLayer::BeginRenameSelectedEntity(Scene& scene) {
		if (m_SelectedEntity == entt::null || !scene.IsValid(m_SelectedEntity)) {
			return;
		}

		Entity entity = scene.GetEntity(m_SelectedEntity);
		m_RenamingEntity = m_SelectedEntity;
		m_EntityRenameFrameCounter = 0;
		// Empty buffer for nameless entities: pre-filling GetName()'s placeholder would commit a real NameComponent with synthesized text.
		if (entity.HasComponent<NameComponent>()) {
			std::snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s",
				entity.GetComponent<NameComponent>().Name.c_str());
		}
		else {
			m_EntityRenameBuffer[0] = '\0';
		}
	}

	void ImGuiEditorLayer::UnpackSelectedPrefabs(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = GetSelectedEntities(scene);
		if (selectedEntities.empty()) {
			return;
		}

		bool anyUnpacked = false;
		for (EntityHandle entity : selectedEntities) {
			if (!scene.IsValid(entity)) continue;
			if (!scene.HasComponent<PrefabInstanceComponent>(entity)) continue;
			scene.SetEntityMetaData(entity, EntityOrigin::Scene);
			anyUnpacked = true;
		}
		if (anyUnpacked) {
			scene.MarkDirty();
		}
	}

	void ImGuiEditorLayer::CopySelectedEntities(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		if (selectedEntities.empty()) {
			return;
		}

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("type", Json::Value("EntityClipboard"));

		Json::Value entities = Json::Value::MakeArray();
		for (EntityHandle entity : selectedEntities) {
			Json::Value entityValue = SceneSerializer::SerializeEntityForClipboard(scene, entity);
			if (entityValue.IsObject()) {
				entities.Append(std::move(entityValue));
			}
		}

		if (entities.GetArray().empty()) {
			return;
		}

		root.AddMember("entities", std::move(entities));
		m_EntityClipboardJson = Json::Stringify(root, false);
	}

	void ImGuiEditorLayer::CutSelectedEntities(Scene& scene) {
		const std::vector<EntityHandle> selectedEntities = GetSelectedEntities(scene);
		if (selectedEntities.empty()) {
			return;
		}

		// New cut supersedes prior cut: drop old marker (entities stay) and snapshot new selection.
		m_CutEntities.clear();
		m_EntityClipboardJson.clear();
		CopySelectedEntities(scene);
		if (!m_EntityClipboardJson.empty()) {
			for (EntityHandle h : selectedEntities) {
				m_CutEntities.insert(static_cast<uint32_t>(h));
			}
		}
	}

	void ImGuiEditorLayer::PasteEntities(Scene& scene) {
		if (m_EntityClipboardJson.empty()) {
			return;
		}

		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(m_EntityClipboardJson, root, &parseError) || !root.IsObject()) {
			return;
		}

		const Json::Value* typeValue = root.FindMember("type");
		const Json::Value* entitiesValue = root.FindMember("entities");
		if (!typeValue || typeValue->AsStringOr() != "EntityClipboard" || !entitiesValue || !entitiesValue->IsArray()) {
			return;
		}

		EntityHandle pasteParent = (!m_IsSceneNodeSelected
			&& m_SelectedEntity != entt::null
			&& scene.IsValid(m_SelectedEntity))
			? m_SelectedEntity
			: entt::null;
		// Prefab mode: rootless paste would create a sibling-to-root that the serializer silently drops; force under prefab root.
		if (pasteParent == entt::null
			&& IsInPrefabEditMode()
			&& m_PrefabEditScene
			&& m_PrefabEditRootEntity != entt::null
			&& m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			&& &scene == m_PrefabEditScene.get())
		{
			pasteParent = m_PrefabEditRootEntity;
		}

		std::vector<EntityHandle> createdEntities;
		for (const Json::Value& entityValue : entitiesValue->GetArray()) {
			EntityHandle entity = SceneSerializer::DeserializeEntityFromValue(scene, entityValue);
			if (entity != entt::null) {
				if (pasteParent != entt::null) {
					scene.GetEntity(entity).SetParent(scene.GetEntity(pasteParent));
				}
				createdEntities.push_back(entity);
			}
		}

		if (!createdEntities.empty()) {
			EnsureEditorUniqueEntityNames(scene, createdEntities);
			m_SelectedEntities = createdEntities;
			RebuildSelectionSet();
			m_SelectedEntity = createdEntities.back();
			m_LastEntitySelectionIndex = -1;
			m_IsSceneNodeSelected = false;
			if (pasteParent != entt::null) {
				m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(pasteParent));
			}
			scene.MarkDirty();
			m_EntityOrder.clear(); m_EntityOrderDirty = true;

			if (!m_CutEntities.empty()) {
				for (uint32_t raw : m_CutEntities) {
					EntityHandle h = static_cast<EntityHandle>(raw);
					if (scene.IsValid(h)) {
						scene.DestroyEntity(h);
					}
				}
				m_CutEntities.clear();
				m_EntityOrderDirty = true;
			}
		}
	}

	std::string ImGuiEditorLayer::MakeEditorUniqueEntityName(
		Scene& scene,
		std::string_view baseName,
		EntityHandle ignoreEntity) const
	{
		IndexProject* project = ProjectManager::GetCurrentProject();
		std::string original = baseName.empty() ? std::string("Entity") : std::string(baseName);
		if (!project || !project->EditorEnsureUniqueEntityNames) {
			return original;
		}

		const auto style = project->EditorEntityNameSuffix;
		std::unordered_set<std::string> existingNames;
		auto view = scene.GetRegistry().view<NameComponent>();
		for (auto [entity, name] : view.each()) {
			if (entity == ignoreEntity) {
				continue;
			}
			existingNames.insert(name.Name);
		}

		if (!existingNames.contains(original)) {
			return original;
		}

		std::string base = StripEditorNumericSuffix(original);
		if (base.empty()) {
			base = "Entity";
		}
		const bool originalHadSuffix = base != original;
		if (!originalHadSuffix && !existingNames.contains(base)) {
			return base;
		}

		for (int suffix = 1; suffix < std::numeric_limits<int>::max(); ++suffix) {
			std::string candidate = FormatEditorEntitySuffix(base, suffix, style);
			if (!existingNames.contains(candidate)) {
				return candidate;
			}
		}

		return base;
	}

	void ImGuiEditorLayer::EnsureEditorUniqueEntityName(Scene& scene, EntityHandle entity) {
		if (entity == entt::null || !scene.IsValid(entity) || !scene.HasComponent<NameComponent>(entity)) {
			return;
		}

		NameComponent& name = scene.GetComponent<NameComponent>(entity);
		name.Name = MakeEditorUniqueEntityName(scene, name.Name, entity);
	}

	void ImGuiEditorLayer::EnsureEditorUniqueEntityNames(Scene& scene, const std::vector<EntityHandle>& roots) {
		if (roots.empty()) {
			return;
		}

		std::unordered_set<uint32_t> visited;
		std::function<void(EntityHandle)> visit = [&](EntityHandle entity) {
			if (entity == entt::null || !scene.IsValid(entity)) {
				return;
			}
			if (!visited.insert(static_cast<uint32_t>(entity)).second) {
				return;
			}

			EnsureEditorUniqueEntityName(scene, entity);
			if (scene.HasComponent<HierarchyComponent>(entity)) {
				for (EntityHandle child : scene.GetComponent<HierarchyComponent>(entity).Children) {
					visit(child);
				}
			}
		};

		for (EntityHandle root : roots) {
			visit(root);
		}
	}

	EntityHandle ImGuiEditorLayer::FinishCreatedEditorEntity(Scene& scene, Entity parent, Entity created) {
		if (!created.IsValid()) {
			return entt::null;
		}
		// Prefab mode: entities created without a parent must be forced under the prefab root; siblings-of-root are silently dropped on save.
		Entity effectiveParent = parent;
		if (!effectiveParent.IsValid()
			&& IsInPrefabEditMode()
			&& m_PrefabEditScene
			&& m_PrefabEditRootEntity != entt::null
			&& m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			&& &scene == m_PrefabEditScene.get())
		{
			effectiveParent = m_PrefabEditScene->GetEntity(m_PrefabEditRootEntity);
		}

		if (effectiveParent.IsValid()) {
			created.SetParent(effectiveParent);
			m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(effectiveParent.GetHandle()));
		}

		const EntityHandle createdHandle = created.GetHandle();
		EnsureEditorUniqueEntityNames(scene, { createdHandle });
		SelectEntity(createdHandle);
		scene.MarkDirty();
		m_EntityOrder.clear();
		m_EntityOrderDirty = true;
		return createdHandle;
	}

	EntityHandle ImGuiEditorLayer::RenderCreateEntityMenu(Scene& scene, Entity parent) {
		EntityHandle createdHandle = entt::null;

		auto finishCreated = [&](Entity created) {
			createdHandle = FinishCreatedEditorEntity(scene, parent, created);
		};

		if (ImGui::MenuItem("Create (Empty)", "Shift + C"))
		{
			finishCreated(scene.CreateEmptyEntity());
		}
		if (ImGui::MenuItem("Create (Entity)"))
		{
			finishCreated(scene.CreateEntity("Entity"));
		}

		auto renderPackagePresets = [&](std::string_view menuPath) {
			bool renderedAny = false;
			for (const SceneManager::EntityPresetInfo& preset : SceneManager::Get().GetEntityPresets()) {
				if (preset.MenuPath != menuPath) {
					continue;
				}
				if (!renderedAny) {
					ImGui::Separator();
					renderedAny = true;
				}
				if (ImGui::MenuItem(preset.Label.c_str())) {
					finishCreated(preset.Create(scene));
				}
			}
		};

		ImGui::Separator();

		if (ImGui::BeginMenu("2D Entity"))
		{
			if (ImGui::BeginMenu("Sprite"))
			{
				if (ImGui::MenuItem("Square"))
				{
					Entity created = scene.CreateEntity("Square");
					created.AddComponent<SpriteRendererComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Circle"))
				{
					Entity created = scene.CreateEntity("Circle");
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::Circle);
					finishCreated(created);
				}
				if (ImGui::MenuItem("9Sliced"))
				{
					Entity created = scene.CreateEntity("9Sliced");
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::_9Sliced);
					finishCreated(created);
				}
				if (ImGui::MenuItem("Pixel"))
				{
					Entity created = scene.CreateEntity("Pixel");
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::GetDefaultTexture(DefaultTexture::Pixel);
					finishCreated(created);
				}
				if (ImGui::MenuItem("Icon"))
				{
					Entity created = scene.CreateEntity("Icon");
					auto& sprite = created.AddComponent<SpriteRendererComponent>();
					sprite.TextureHandle = TextureManager::LoadTexture("icon.png");
					finishCreated(created);
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Physics")) {
				if (ImGui::MenuItem("Dynamic Body"))
				{
					Entity created = scene.CreateEntity("Dynamic Body");
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>();
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Kinematic Body"))
				{
					Entity created = scene.CreateEntity("Kinematic Body");
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>().SetBodyType(BodyType::Kinematic);
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}
				if (ImGui::MenuItem("Static Body"))
				{
					Entity created = scene.CreateEntity("Static Body");
					created.AddComponent<SpriteRendererComponent>();
					created.AddComponent<Rigidbody2DComponent>().SetBodyType(BodyType::Static);
					created.AddComponent<BoxCollider2DComponent>();
					finishCreated(created);
				}

				ImGui::EndMenu();
			}

			if (ImGui::MenuItem("Particle System"))
			{
				Entity created = scene.CreateEntity("Particle System");
				created.AddComponent<ParticleSystem2DComponent>();
				finishCreated(created);
			}

			// Package-registered presets can extend this menu without editor hardcoding.
			renderPackagePresets("2D Entity");

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("UI"))
		{
			if (ImGui::MenuItem("Panel")) {
				finishCreated(EntityHelper::CreateUIPanel(scene));
			}
			if (ImGui::MenuItem("Image")) {
				Entity created = EntityHelper::CreateImageEntity(scene);
				if (!created.HasComponent<NameComponent>()) {
					created.AddComponent<NameComponent>(NameComponent("Image"));
				}
				finishCreated(created);
			}
			if (ImGui::MenuItem("Text")) {
				// Use CreateWith (not CreateEntity): avoids seeding Transform2DComponent which conflicts with RectTransform2D.
				Entity created = EntityHelper::CreateWith<RectTransform2DComponent, TextRendererComponent>(scene);
				created.GetComponent<TextRendererComponent>().HAlign = TextAlignment::Center;
				if (!created.HasComponent<NameComponent>()) {
					created.AddComponent<NameComponent>(NameComponent("Text"));
				}
				finishCreated(created);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Button")) {
				finishCreated(EntityHelper::CreateUIButton(scene));
			}
			if (ImGui::MenuItem("Slider")) {
				finishCreated(EntityHelper::CreateUISlider(scene));
			}
			if (ImGui::MenuItem("Progress Bar")) {
				finishCreated(EntityHelper::CreateUIProgressBar(scene));
			}
			if (ImGui::MenuItem("Circular Slider")) {
				finishCreated(EntityHelper::CreateUICircularSlider(scene));
			}
			if (ImGui::MenuItem("Scrollbar")) {
				finishCreated(EntityHelper::CreateUIScrollbar(scene));
			}
			if (ImGui::MenuItem("Scroll View")) {
				finishCreated(EntityHelper::CreateUIScrollRect(scene));
			}
			if (ImGui::MenuItem("Input Field")) {
				finishCreated(EntityHelper::CreateUIInputField(scene));
			}
			if (ImGui::MenuItem("Dropdown")) {
				finishCreated(EntityHelper::CreateUIDropdown(scene));
			}
			if (ImGui::MenuItem("Toggle")) {
				finishCreated(EntityHelper::CreateUIToggle(scene));
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Horizontal Layout")) {
				finishCreated(EntityHelper::CreateUIHorizontalLayout(scene));
			}
			if (ImGui::MenuItem("Vertical Layout")) {
				finishCreated(EntityHelper::CreateUIVerticalLayout(scene));
			}
			if (ImGui::MenuItem("Grid Layout")) {
				finishCreated(EntityHelper::CreateUIGridLayout(scene));
			}
			renderPackagePresets("UI");
			ImGui::EndMenu();
		}

		std::vector<std::string> packageMenuPaths;
		for (const SceneManager::EntityPresetInfo& preset : SceneManager::Get().GetEntityPresets()) {
			if (preset.MenuPath.empty() || preset.MenuPath == "2D Entity" || preset.MenuPath == "UI") {
				continue;
			}
			if (std::find(packageMenuPaths.begin(), packageMenuPaths.end(), preset.MenuPath) == packageMenuPaths.end()) {
				packageMenuPaths.push_back(preset.MenuPath);
			}
		}
		for (const std::string& menuPath : packageMenuPaths) {
			if (ImGui::BeginMenu(menuPath.c_str())) {
				for (const SceneManager::EntityPresetInfo& preset : SceneManager::Get().GetEntityPresets()) {
					if (preset.MenuPath == menuPath && ImGui::MenuItem(preset.Label.c_str())) {
						finishCreated(preset.Create(scene));
					}
				}
				ImGui::EndMenu();
			}
		}

		ImGui::Separator();

		if (ImGui::MenuItem("Camera"))
		{
			Entity created = scene.CreateEntity("Camera");
			created.AddComponent<Camera2DComponent>();
			finishCreated(created);
		}

		if (ImGui::MenuItem("Audio Source"))
		{
			Entity created = scene.CreateEntity("Audio Source");
			created.AddComponent<AudioSourceComponent>();
			finishCreated(created);
		}

		return createdHandle;
	}

	bool ImGuiEditorLayer::SetEntityParentPreservingWorld(Scene& scene, EntityHandle childHandle, Entity parent) {
		if (childHandle == entt::null || !scene.IsValid(childHandle)) {
			return false;
		}
		if (parent.IsValid() && parent.GetScene() != &scene) {
			return false;
		}

		// Prefab mode: refuse unparenting-to-root (creates second root) and reparenting the prefab root (orphans subtree).
		if (IsInPrefabEditMode() && m_PrefabEditScene && &scene == m_PrefabEditScene.get()) {
			if (!parent.IsValid()) {
				return false;
			}
			if (childHandle == m_PrefabEditRootEntity) {
				return false;
			}
		}

		Entity child = scene.GetEntity(childHandle);
		if (parent.IsValid() && (parent.GetHandle() == childHandle || child.IsAncestorOf(parent))) {
			return false;
		}
		if (child.GetParent() == parent) {
			return false;
		}

		TransformHierarchySystem::Propagate(scene);

		Transform2DComponent* childTransform = nullptr;
		const bool hasTransform = scene.TryGetComponent<Transform2DComponent>(childHandle, childTransform) && childTransform;
		Vec2 worldPosition{ 0.0f, 0.0f };
		Vec2 worldScale{ 1.0f, 1.0f };
		float worldRotation = 0.0f;
		if (hasTransform) {
			worldPosition = childTransform->Position;
			worldRotation = childTransform->Rotation;
			worldScale = childTransform->Scale;
		}

		child.SetParent(parent);

		if (hasTransform) {
			Transform2DComponent* parentTransform = nullptr;
			if (parent.IsValid()) {
				scene.TryGetComponent<Transform2DComponent>(parent.GetHandle(), parentTransform);
			}
			SetLocalTransformFromWorld(*childTransform, parentTransform, worldPosition, worldRotation, worldScale);
			TransformHierarchySystem::Propagate(scene);
		}

		return true;
	}

	std::vector<EntityHandle> ImGuiEditorLayer::MigrateEntitiesToScene(Scene& sourceScene, Scene& targetScene,
		const std::vector<EntityHandle>& sourceRoots, Entity targetParent) {
		std::vector<EntityHandle> migratedRoots;
		if (&sourceScene == &targetScene) {
			IDX_CORE_WARN_TAG("Editor", "MigrateEntitiesToScene called with sourceScene == targetScene; ignoring");
			return migratedRoots;
		}
		if (sourceRoots.empty()) return migratedRoots;

		// Snapshot first, destroy after: destroying a parent before snapshotting its children would dangle JSON references.
		std::vector<Json::Value> snapshots;
		std::vector<EntityHandle> consumed;
		snapshots.reserve(sourceRoots.size());
		consumed.reserve(sourceRoots.size());
		for (EntityHandle root : sourceRoots) {
			if (!sourceScene.IsValid(root)) continue;
			Json::Value snapshot = SceneSerializer::SerializeEntityForClipboard(sourceScene, root);
			if (!snapshot.IsObject()) continue;
			snapshots.push_back(std::move(snapshot));
			consumed.push_back(root);
		}
		if (snapshots.empty()) return migratedRoots;

		migratedRoots.reserve(snapshots.size());
		for (Json::Value& snapshot : snapshots) {
			EntityHandle clone = SceneSerializer::DeserializeEntityFromValue(targetScene, snapshot);
			if (clone == entt::null) continue;
			if (targetParent.IsValid() && targetScene.IsValid(targetParent.GetHandle())) {
				Entity cloneEntity = targetScene.GetEntity(clone);
				cloneEntity.SetParent(targetParent);
			}
			migratedRoots.push_back(clone);
		}

		// Destroy originals even on partial deserialize failure: the gesture was "move", so leaving originals would silently duplicate.
		for (EntityHandle source : consumed) {
			if (sourceScene.IsValid(source)) {
				sourceScene.DestroyEntity(source);
			}
		}

		if (!migratedRoots.empty()) {
			EnsureEditorUniqueEntityNames(targetScene, migratedRoots);
			sourceScene.MarkDirty();
			targetScene.MarkDirty();
		}
		return migratedRoots;
	}

	std::vector<EntityHandle> ImGuiEditorLayer::ResolveDraggedHierarchyEntities(Scene& scene, EntityHandle primary) const {
		std::vector<EntityHandle> result;

		// Drag an unselected entity: single-entity move. Drag within selection: move the whole selection.
		const bool draggedIsInSelection =
			std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), primary) != m_SelectedEntities.end();
		if (draggedIsInSelection && m_SelectedEntities.size() > 1) {
			result = FilterSelectedHierarchyRoots(scene, GetSelectedEntities(scene));
		}

		if (result.empty()) {
			if (primary != entt::null && scene.IsValid(primary)) {
				result.push_back(primary);
			}
		}
		return result;
	}

	bool ImGuiEditorLayer::MoveSiblingNextTo(Scene& scene, EntityHandle dragged, EntityHandle target, bool insertAfter)
	{
		if (dragged == target || !scene.IsValid(dragged) || !scene.IsValid(target)) return false;

		Entity draggedEntity = scene.GetEntity(dragged);
		Entity targetEntity = scene.GetEntity(target);

		// Reparenting `dragged` under one of its own descendants would create
		// a cycle; Entity::SetParent already refuses this silently, but we
		// also guard the sibling reorder path so we don't claim success.
		if (draggedEntity.IsAncestorOf(targetEntity)) return false;

		Entity targetParent = targetEntity.GetParent();
		Entity draggedParent = draggedEntity.GetParent();

		if (draggedParent != targetParent) {
			if (!SetEntityParentPreservingWorld(scene, dragged, targetParent)) {
				return false;
			}
		}

		auto repositionVec = [&](std::vector<EntityHandle>& vec) -> bool {
			auto draggedIt = std::find(vec.begin(), vec.end(), dragged);
			if (draggedIt == vec.end()) return false;
			vec.erase(draggedIt);
			auto targetIt = std::find(vec.begin(), vec.end(), target);
			const size_t insertAt = (targetIt == vec.end())
				? vec.size()
				: static_cast<size_t>((targetIt - vec.begin()) + (insertAfter ? 1 : 0));
			vec.insert(vec.begin() + insertAt, dragged);
			return true;
		};

		if (targetParent.IsValid()) {
			auto& childList = scene.GetComponent<HierarchyComponent>(targetParent.GetHandle()).Children;
			repositionVec(childList);
		}
		else {
			// Root-level reorder: mutate m_EntityOrder AND mirror into entt storage so the renderer/serializer (which iterate entt directly) see the new order.
			// entt iterates dense storage newest-first; UIDrawOrder reverses that, so m_EntityOrder.back() must be the "newest" entity in the storage sort.
			repositionVec(m_EntityOrder);
			m_EntityOrderDirty = true; // M30: force re-DFS next render.

			std::unordered_map<entt::entity, std::size_t> indexOf;
			indexOf.reserve(m_EntityOrder.size());
			for (std::size_t i = 0; i < m_EntityOrder.size(); ++i) {
				indexOf[m_EntityOrder[i]] = i;
			}
			const std::size_t fallbackBase = m_EntityOrder.size();
			auto& storage = scene.GetRegistry().storage<entt::entity>();
			storage.sort([&](entt::entity a, entt::entity b) {
				auto ita = indexOf.find(a);
				auto itb = indexOf.find(b);
				const std::size_t ai = (ita != indexOf.end())
					? ita->second
					: fallbackBase + static_cast<std::size_t>(entt::to_integral(a));
				const std::size_t bi = (itb != indexOf.end())
					? itb->second
					: fallbackBase + static_cast<std::size_t>(entt::to_integral(b));
				return ai > bi;
			});
		}

		return true;
	}

	bool ImGuiEditorLayer::OpenPrefabForEditing(const std::string& path) {
		if (path.empty() || !File::Exists(path)) {
			IDX_CORE_WARN_TAG("PrefabEdit", "Prefab file not found: {}", path);
			return false;
		}

		// Auto-save before swapping: a modal would be invasive; SavePrefabEditChanges is a no-op when nothing changed.
		if (m_PrefabEditScene) {
			if (m_PrefabEditScene->IsDirty()) {
				SavePrefabEditChanges();
			}
			ClosePrefabEditing(false);
		}

		// Use ReadRootFromFile (not raw JSON parse): project may use binary serialization format.
		Json::Value root;
		std::string parseError;
		if (!SceneSerializerStorage::ReadRootFromFile(path, root, &parseError) || !root.IsObject()) {
			IDX_CORE_ERROR_TAG("PrefabEdit", "Failed to parse {}: {}", path, parseError);
			return false;
		}

		const Json::Value* entityValue = root.FindMember("Entity");
		if (!entityValue) entityValue = root.FindMember("prefab");
		if (!entityValue || !entityValue->IsObject()) {
			IDX_CORE_WARN_TAG("PrefabEdit", "No Entity/prefab block in {}", path);
			return false;
		}

		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_CollapsedHierarchyEntities.clear();
		m_CutEntities.clear();

		// Save camera state to restore on Close; repoint at prefab root so it's not off-screen (prefabs typically authored at origin).
		m_PrefabEditSavedCameraPosition = m_EditorCamera.GetPosition();
		m_PrefabEditSavedCameraOrthoSize = m_EditorCamera.GetOrthographicSize();
		m_PrefabEditSavedCameraZoom = m_EditorCamera.GetZoom();
		m_PrefabEditHasSavedCameraState = true;

		auto detached = Scene::CreateDetachedScene("##PrefabEdit");
		const EntityHandle rootEntity = SceneSerializer::DeserializeEntityFromValue(*detached, root);
		// DeserializeEntityFromValue marks the scene dirty as a side
		// effect; clear so our flag only flips on actual user edits.
		detached->ClearDirty();

		// Run one-shot propagation so world transforms are valid before we read the root position for camera focus.
		TransformHierarchySystem::Propagate(*detached);

		m_PrefabEditScene = std::move(detached);
		m_PrefabEditPath = path;
		m_PrefabEditRootEntity = rootEntity;
		m_PrefabEditDirty = false;

		if (rootEntity != entt::null && m_PrefabEditScene->IsValid(rootEntity)) {
			SelectEntity(rootEntity);
			Vec2 focusPos{ 0.0f, 0.0f };
			Transform2DComponent* rootTransform = nullptr;
			if (m_PrefabEditScene->TryGetComponent<Transform2DComponent>(rootEntity, rootTransform) && rootTransform) {
				focusPos = rootTransform->Position;
			}
			m_EditorCamera.SetPosition(focusPos);
			// Reset zoom/ortho size to a sane default so a prior heavily-
			// zoomed-in scene-edit framing doesn't make the prefab fill the
			// viewport (or vice-versa).
			m_EditorCamera.SetZoom(1.0f);
			m_EditorCamera.SetOrthographicSize(5.0f);
		}

		IDX_INFO_TAG("PrefabEdit", "Editing {}", path);
		return true;
	}

	bool ImGuiEditorLayer::SavePrefabEditChanges() {
		if (!m_PrefabEditScene) return false;
		if (m_PrefabEditRootEntity == entt::null
			|| !m_PrefabEditScene->IsValid(m_PrefabEditRootEntity)
			|| m_PrefabEditPath.empty()) {
			return false;
		}

		// Capture OLD source before overwriting: propagation diffs overrides against it; diffing against the new source loses per-instance overrides.
		Json::Value previousSourceRoot;
		bool havePreviousSource = false;
		if (File::Exists(m_PrefabEditPath)) {
			Json::Value previousRoot;
			std::string readError;
			if (SceneSerializerStorage::ReadRootFromFile(m_PrefabEditPath, previousRoot, &readError) && previousRoot.IsObject()) {
				previousSourceRoot = previousRoot;
				havePreviousSource = true;
			}
			else {
				// File unreadable: bail and leave dirty so the next auto-save tick retries.
				IDX_CORE_WARN_TAG("PrefabEdit",
					"Pre-save read of '{}' failed ({}); leaving prefab dirty for retry.",
					m_PrefabEditPath, readError);
				return false;
			}
		}

		if (!SceneSerializer::SaveEntityToFile(*m_PrefabEditScene, m_PrefabEditRootEntity, m_PrefabEditPath)) {
			IDX_CORE_ERROR_TAG("PrefabEdit", "Failed to save {}", m_PrefabEditPath);
			return false;
		}

		// Propagation BEFORE clearing dirty flag: if it fails mid-loop the prefab stays dirty and auto-save retries.
		if (havePreviousSource) {
			const uint64_t prefabGuid = AssetRegistry::GetOrCreateAssetUUID(m_PrefabEditPath);
			if (prefabGuid != 0) {
				SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
					if (&s == m_PrefabEditScene.get()) return;
					std::vector<EntityHandle> targets;
					auto view = s.GetRegistry().view<EntityMetaDataComponent>();
					for (entt::entity ent : view) {
						const auto& meta = view.get<EntityMetaDataComponent>(ent).MetaData;
						if (meta.Origin != EntityOrigin::Prefab) continue;
						if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
						targets.push_back(ent);
					}
					bool anyRefreshed = false;
					for (EntityHandle t : targets) {
						if (!s.IsValid(t)) continue;
						EntityHandle replacement = SceneSerializer::RefreshPrefabInstance(s, t, previousSourceRoot);
						if (replacement != entt::null && replacement != t) { // orphaned source returns original t (nothing refreshed)
							anyRefreshed = true;
						}
					}
					if (anyRefreshed) s.MarkDirty();
				});
			}
		}

		// Mark the detached scene clean so the toolbar no longer shows the
		// "*" suffix and the per-frame auto-save tick doesn't loop on the
		// same edits.
		m_PrefabEditScene->ClearDirty();
		m_PrefabEditDirty = false;
		return true;
	}

	void ImGuiEditorLayer::ClosePrefabEditing(bool save) {
		if (!m_PrefabEditScene) return;

		if (save) {
			SavePrefabEditChanges();
		}

		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_CollapsedHierarchyEntities.clear();
		// Drop any cut markers — they refer to entities in the detached
		// prefab scene we're about to discard, and a leftover marker
		// would otherwise dim a freshly-recycled handle in the next scene.
		m_CutEntities.clear();
		m_PrefabEditRootEntity = entt::null;
		m_PrefabEditPath.clear();
		m_PrefabEditDirty = false;
		// unique_ptr destruction tears down the registry; destroy hooks
		// gated by Scene::IsDetached() short-circuit so global physics/
		// audio/script subsystems aren't touched.
		m_PrefabEditScene.reset();

		// Position MUST come last: SetOrthographicSize/SetZoom recompute projection only; position rebuilds the view matrix.
		if (m_PrefabEditHasSavedCameraState) {
			m_EditorCamera.SetOrthographicSize(m_PrefabEditSavedCameraOrthoSize);
			m_EditorCamera.SetZoom(m_PrefabEditSavedCameraZoom);
			m_EditorCamera.SetPosition(m_PrefabEditSavedCameraPosition);
			m_PrefabEditHasSavedCameraState = false;
		}
	}

	Scene* ImGuiEditorLayer::GetContextScene() const {
		if (m_PrefabEditScene) return m_PrefabEditScene.get();
		// Last-interacted scene wins so Create Entity lands where expected with multi-scene.
		if (!m_LastInteractedSceneName.empty()) {
			if (auto weak = SceneManager::Get().GetLoadedScene(m_LastInteractedSceneName);
				auto shared = weak.lock()) {
				return shared.get();
			}
		}
		return SceneManager::Get().GetActiveScene();
	}

	bool ImGuiEditorLayer::HasEntityShortcutFocus() const {
		return m_IsEditorViewActive || m_IsEditorViewFocused || m_IsEntitiesPanelFocused || m_IsInspectorPanelFocused;
	}



	// ──────────────────────────────────────────────
	//  Entities Panel (with rename support)
	// ──────────────────────────────────────────────

	void ImGuiEditorLayer::RenderEntitiesPanel() {
		INDEX_PROFILE_SCOPE("Hierarchy Panel");
		ImGui::Begin("Entities");

		if (IsInPrefabEditMode()) {
			// Mirror IsDirty onto m_PrefabEditDirty so callers (e.g. breadcrumb) read the canonical dirty state.
			m_PrefabEditDirty = m_PrefabEditScene->IsDirty();

			const std::string prefabName = std::filesystem::path(m_PrefabEditPath).stem().string();
			const bool showDirtyMark = m_PrefabEditDirty && !EditorPreferences::GetAutoSavePrefabs();
			const std::string title = showDirtyMark ? (prefabName + " *") : prefabName;

			const float backButtonStartX = ImGui::GetCursorPosX();
			if (ImGui::SmallButton("<")) {
				if (m_PrefabEditDirty) {
					m_ShowPrefabEditDiscardPrompt = true;
				}
				else {
					ClosePrefabEditing(false);
				}
			}

			const float rowEndX = backButtonStartX + ImGui::GetContentRegionAvail().x;
			const float textWidth = ImGui::CalcTextSize(title.c_str()).x;
			const float centerX = backButtonStartX + (rowEndX - backButtonStartX - textWidth) * 0.5f;
			ImGui::SameLine();
			// Guard against very narrow panels where the back button would
			// overlap the centered text; fall back to a normal SameLine gap.
			const float minCenterX = ImGui::GetCursorPosX();
			ImGui::SetCursorPosX(centerX > minCenterX ? centerX : minCenterX);
			ImGui::TextUnformatted(title.c_str());
			ImGui::Separator();
		}

		Scene* activeScene = GetContextScene();

		// Right-click on empty space: create entities in the active scene
		if (activeScene && ImGui::BeginPopupContextWindow("EntityCreateContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			Scene& scene = *activeScene;
			RenderCreateEntityMenu(scene, Entity::Null);
			ImGui::EndPopup();
		}

		std::vector<Scene*> scenesToShow;
		if (IsInPrefabEditMode()) {
			scenesToShow.push_back(m_PrefabEditScene.get());
		}
		else {
			auto loadedScenes = SceneManager::Get().GetLoadedScenes();
			scenesToShow.reserve(loadedScenes.size());
			for (auto& weakScene : loadedScenes) {
				if (auto scenePtr = weakScene.lock()) {
					scenesToShow.push_back(scenePtr.get());
				}
			}
		}
		std::string sceneToRemove;
		struct PendingSceneReorder {
			std::string SourceName;
			size_t TargetIndex;
		};
		std::optional<PendingSceneReorder> pendingSceneReorder;

		for (size_t sceneIdx = 0; sceneIdx < scenesToShow.size(); ++sceneIdx) {
			Scene* scenePtrRaw = scenesToShow[sceneIdx];
			if (!scenePtrRaw) continue;
			Scene& scene = *scenePtrRaw;

			const uint64_t sceneIdValue = static_cast<uint64_t>(scene.GetSceneId());
			// Scene* (not sceneId) for the ImGui scope: two scenes can collide on
			// sceneId — duplicated .scene files keep their serialized id, and an
			// additively loaded script-only scene starts at the UUID default.
			// Scene* is always distinct among currently-loaded scenes.
			ImGui::PushID(scenePtrRaw);

			// Prefab mode: skip scene header; drop target would create a second root in the single-rooted prefab.
			bool sceneOpen = true;
			if (!IsInPrefabEditMode()) {
				ImGuiTreeNodeFlags sceneFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow
					| ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
				const std::string fullSceneLabel = scene.IsDirty() ? scene.GetName() + " *" : scene.GetName();
				bool sceneLabelTruncated = false;
				const std::string sceneLabel = ImGuiUtils::Ellipsize(fullSceneLabel, ImGui::GetContentRegionAvail().x, &sceneLabelTruncated);
				// Use stable str_id (not the label) so the fold state survives the
				// dirty-asterisk toggle that flips on save. Label is supplied via fmt.
				sceneOpen = ImGui::TreeNodeEx("##scene_node", sceneFlags, "%s", sceneLabel.c_str());
				if (sceneLabelTruncated && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", fullSceneLabel.c_str());
				}
				// Capture immediately after TreeNodeEx; subsequent calls would shift the "last item" rect.
				const ImVec2 sceneItemRectMin = ImGui::GetItemRectMin();
				const ImVec2 sceneItemRectMax = ImGui::GetItemRectMax();
				if (ImGui::IsItemActivated()) {
					m_LastInteractedSceneName = scene.GetName();
				}
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
					SelectSceneNode();
				}

				if (scenesToShow.size() > 1 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
					HierarchySceneDragData sceneDrag{ sceneIdValue };
					ImGui::SetDragDropPayload("HIERARCHY_SCENE", &sceneDrag, sizeof(sceneDrag));
					ImGui::Text("Move scene: %s", scene.GetName().c_str());
					ImGui::EndDragDropSource();
				}

				if (ImGui::BeginDragDropTarget()) {
					const float mouseY = ImGui::GetMousePos().y;
					const float headerMidY = (sceneItemRectMin.y + sceneItemRectMax.y) * 0.5f;
					const bool sceneInTopZone = mouseY < headerMidY;
					if (const ImGuiPayload* peek = ImGui::GetDragDropPayload()) {
						if (peek->IsDataType("HIERARCHY_SCENE")) {
							const float lineY = sceneInTopZone ? sceneItemRectMin.y : sceneItemRectMax.y;
							const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
							ImGui::GetWindowDrawList()->AddLine(
								ImVec2(sceneItemRectMin.x, lineY),
								ImVec2(sceneItemRectMax.x, lineY),
								color, 2.0f);
						}
					}

					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_SCENE")) {
						auto* dragData = static_cast<const HierarchySceneDragData*>(payload->Data);
						if (dragData->SceneId != sceneIdValue) {
							size_t sourceIdx = SIZE_MAX;
							std::string sourceName;
							for (size_t i = 0; i < scenesToShow.size(); ++i) {
								Scene* candidate = scenesToShow[i];
								if (candidate && static_cast<uint64_t>(candidate->GetSceneId()) == dragData->SceneId) {
									sourceIdx = i;
									sourceName = candidate->GetName();
									break;
								}
							}
							if (sourceIdx != SIZE_MAX) {
								// Adjust for the removal shift: when source < target, removing source shifts target index down by 1.
								size_t finalIdx;
								if (sceneInTopZone) {
									finalIdx = (sourceIdx < sceneIdx) ? (sceneIdx - 1) : sceneIdx;
								}
								else {
									finalIdx = (sourceIdx < sceneIdx) ? sceneIdx : (sceneIdx + 1);
								}
								pendingSceneReorder = PendingSceneReorder{ sourceName, finalIdx };
							}
						}
					}

					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
						EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
						const bool sameScene = dragData->SourceSceneId == sceneIdValue;
						if (sameScene) {
							std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(scene, primaryDraggedHandle);
							bool didMove = false;
							for (EntityHandle h : draggedHandles) {
								Entity draggedEntity = scene.GetEntity(h);
								if (!draggedEntity.HasParent()) continue;
								if (SetEntityParentPreservingWorld(scene, h, Entity::Null)) {
									didMove = true;
								}
							}
							if (didMove) {
								// Force a re-DFS — reparenting changes nesting but
								// not the registry count, so the size-mismatch
								// fallback won't trigger.
								m_EntityOrderDirty = true;
								scene.MarkDirty();
							}
						}
						else {
							// Cross-scene drag: locate the source scene by
							// the SceneId carried in the payload, resolve
							// the multi-selection against it, then migrate.
							Scene* sourceScene = nullptr;
							for (Scene* candidate : scenesToShow) {
								if (candidate && static_cast<uint64_t>(candidate->GetSceneId()) == dragData->SourceSceneId) {
									sourceScene = candidate;
									break;
								}
							}
							if (sourceScene && sourceScene != &scene) {
								std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(*sourceScene, primaryDraggedHandle);
								std::vector<EntityHandle> migrated = MigrateEntitiesToScene(*sourceScene, scene, draggedHandles, Entity::Null);
								if (!migrated.empty()) {
									// Selection is keyed by raw entt handle; clear source-scene selection so inspector shows the migrated entities.
									ClearEntitySelection();
									m_SelectedEntities = migrated;
									RebuildSelectionSet();
									m_SelectedEntity = migrated.back();
									m_LastInteractedSceneName = scene.GetName();
									m_EntityOrderDirty = true;
								}
							}
						}
					}
					ImGui::EndDragDropTarget();
				}

				// Right-click context menu on scene tree node. "Remove"
				// only makes sense with more than one scene loaded.
				if (ImGui::BeginPopupContextItem()) {
					const bool canRemove = scenesToShow.size() > 1;
					if (canRemove) {
						if (ImGui::MenuItem("Remove")) {
							sceneToRemove = scene.GetName();
						}
					} else {
						ImGui::MenuItem("Remove", nullptr, false, false);
					}
					// Manual reorder fallback for users without drag-and-drop
					// muscle memory (and a keyboard-only accessibility path).
					// Disabled at the boundaries.
					ImGui::Separator();
					const bool canMoveUp = sceneIdx > 0;
					const bool canMoveDown = sceneIdx + 1 < scenesToShow.size();
					if (ImGui::MenuItem("Move Up", nullptr, false, canMoveUp)) {
						pendingSceneReorder = PendingSceneReorder{ scene.GetName(), sceneIdx - 1 };
					}
					if (ImGui::MenuItem("Move Down", nullptr, false, canMoveDown)) {
						pendingSceneReorder = PendingSceneReorder{ scene.GetName(), sceneIdx + 1 };
					}
					ImGui::EndPopup();
				}
			}

			if (sceneOpen) {
				auto view = scene.GetRegistry().view<entt::entity>();

				// Rebuild only when dirty or registry size changed; view.size() is O(1) (swap_only policy).
				const auto registryCount = static_cast<std::size_t>(view.size());
				// Force rebuild on scene-id change: same entity count from a different scene leaves m_VisibleEntityOrder pointing at destroyed handles.
				if (m_EntityOrderSceneId != scenePtrRaw) {
					m_EntityOrderDirty = true;
					m_EntityOrderSceneId = scenePtrRaw;
				}
				const bool needsRebuild = m_EntityOrderDirty
					|| m_EntityOrder.size() != registryCount;

				if (needsRebuild) {
					INDEX_PROFILE_SCOPE("Editor.HierarchyRebuild");
					// Set-diff sync: never blanket-reset (drag-reorder lives here); new entities appended at end (walk entt view backwards = oldest-first).
					{
						std::vector<entt::entity> viewEntities(view.begin(), view.end());

						std::unordered_set<uint32_t> viewSet;
						viewSet.reserve(viewEntities.size());
						for (auto e : viewEntities) viewSet.insert(static_cast<uint32_t>(e));
						m_EntityOrder.erase(
							std::remove_if(m_EntityOrder.begin(), m_EntityOrder.end(),
								[&](entt::entity e) { return viewSet.find(static_cast<uint32_t>(e)) == viewSet.end(); }),
							m_EntityOrder.end());

						std::unordered_set<uint32_t> orderSet;
						orderSet.reserve(m_EntityOrder.size());
						for (auto e : m_EntityOrder) orderSet.insert(static_cast<uint32_t>(e));
						for (auto it = viewEntities.rbegin(); it != viewEntities.rend(); ++it) {
							if (orderSet.find(static_cast<uint32_t>(*it)) == orderSet.end()) {
								m_EntityOrder.push_back(*it);
							}
						}
					}

					// DFS pass: emit each child immediately after its parent; also builds m_VisibleEntityOrder (pruned to exclude collapsed subtrees).
					m_RenderedEntityDepths.clear();
					m_RenderedEntityDepths.reserve(m_EntityOrder.size());
					std::vector<entt::entity> visibleReordered;
					std::vector<int> visibleDepths;
					visibleReordered.reserve(m_EntityOrder.size());
					visibleDepths.reserve(m_EntityOrder.size());
					{
						auto& reg = scene.GetRegistry();
						std::vector<entt::entity> reordered;
						reordered.reserve(m_EntityOrder.size());
						std::unordered_set<uint32_t> emitted;

						std::function<void(EntityHandle, int, bool)> emitSubtree =
							[&](EntityHandle e, int depth, bool insideCollapsed) {
								if (!reg.valid(e)) return;
								const uint32_t key = static_cast<uint32_t>(e);
								if (!emitted.insert(key).second) return; // already in tree
								reordered.push_back(e);
								m_RenderedEntityDepths.push_back(depth);
								if (!insideCollapsed) {
									visibleReordered.push_back(e);
									visibleDepths.push_back(depth);
								}
								const bool descendInsideCollapsed = insideCollapsed
									|| m_CollapsedHierarchyEntities.contains(key);
								if (reg.all_of<HierarchyComponent>(e)) {
									for (EntityHandle child : reg.get<HierarchyComponent>(e).Children) {
										emitSubtree(child, depth + 1, descendInsideCollapsed);
									}
								}
							};

						// Pass 1: emit each root-of-subtree from the user-ordered
						// list. Skip entities that already appeared as a child.
						for (EntityHandle e : m_EntityOrder) {
							if (!reg.valid(e)) continue;
							EntityHandle parent = entt::null;
							if (reg.all_of<HierarchyComponent>(e)) {
								parent = reg.get<HierarchyComponent>(e).Parent;
							}
							if (parent == entt::null) {
								emitSubtree(e, 0, /*insideCollapsed*/ false);
							}
						}
						// Pass 2: anything still missing — orphaned children
						// whose parent is not in the registry — render flat.
						for (EntityHandle e : m_EntityOrder) {
							if (!reg.valid(e)) continue;
							const uint32_t key = static_cast<uint32_t>(e);
							if (emitted.contains(key)) continue;
							emitSubtree(e, 0, /*insideCollapsed*/ false);
						}
						m_EntityOrder = std::move(reordered);
					}

					m_RenderedEntityOrder = m_EntityOrder;
					m_VisibleEntityOrder = std::move(visibleReordered);
					m_VisibleEntityDepths = std::move(visibleDepths);
					m_EntityOrderDirty = false;
				}

				const ImVec2 defaultItemSpacing = ImGui::GetStyle().ItemSpacing;
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(defaultItemSpacing.x, 1.0f));

				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_VisibleEntityOrder.size()));
					// Gate selection on scene identity: the same entt handle value can exist in multiple loaded scenes.
					Scene* const selectionScene = GetContextScene();
				while (clipper.Step()) {
				for (int entityIdx = clipper.DisplayStart; entityIdx < clipper.DisplayEnd; ++entityIdx) {
					const EntityHandle entityHandle = m_VisibleEntityOrder[static_cast<std::size_t>(entityIdx)];
					if (!scene.IsValid(entityHandle)) continue;
					Entity entity = scene.GetEntity(entityHandle);
					const bool selected = IsEntitySelected(entityHandle) && &scene == selectionScene;

					const int targetDepth = entityIdx < static_cast<int>(m_VisibleEntityDepths.size())
						? m_VisibleEntityDepths[static_cast<std::size_t>(entityIdx)] : 0;

					const int visibleIndex = entityIdx;

					const float rowIndent = static_cast<float>(targetDepth) * 14.0f;
					if (rowIndent > 0.0f) ImGui::Indent(rowIndent);

					ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entityHandle)));

					const bool hasChildren = scene.HasComponent<HierarchyComponent>(entityHandle)
						&& !scene.GetComponent<HierarchyComponent>(entityHandle).Children.empty();
					bool isCollapsed = m_CollapsedHierarchyEntities.contains(static_cast<uint32_t>(entityHandle));
					{
						const float arrowSize = ImGui::GetTextLineHeight();
						if (hasChildren) {
							const ImVec2 arrowTopLeft = ImGui::GetCursorScreenPos();
							if (ImGui::InvisibleButton("##fold", ImVec2(arrowSize, arrowSize))) {
								if (isCollapsed) {
									m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
								}
								else {
									m_CollapsedHierarchyEntities.insert(static_cast<uint32_t>(entityHandle));
								}
								isCollapsed = !isCollapsed;
								// Collapse state feeds into the visible-list
								// pruning done by the DFS rebuild, so any
								// toggle has to trigger a rebuild next frame.
								m_EntityOrderDirty = true;
							}
							const ImU32 arrowColor = ImGui::GetColorU32(
								ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_Text);
							ImGui::RenderArrow(ImGui::GetWindowDrawList(), arrowTopLeft,
								arrowColor,
								isCollapsed ? ImGuiDir_Right : ImGuiDir_Down,
								0.7f);
						}
						else {
							ImGui::Dummy(ImVec2(arrowSize, arrowSize));
						}
						ImGui::SameLine(0.0f, 2.0f);
					}

					bool entityIsDisabled = scene.HasComponent<DisabledTag>(entityHandle);
					if (entityIsDisabled)
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

					bool entityIsPrefabTinted = false;
					if (!entityIsDisabled && !Application::GetIsPlaying() && scene.GetEntityOrigin(entityHandle) == EntityOrigin::Prefab) {
						const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entityHandle));
						const bool resolvable = prefabGuid != 0 && !AssetRegistry::ResolvePath(prefabGuid).empty();
						ImGui::PushStyleColor(ImGuiCol_Text,
							resolvable ? EditorTheme::Colors::PrefabInstance : EditorTheme::Colors::PrefabOrphan);
						entityIsPrefabTinted = true;
					}

					bool entityIsCutMarked = m_CutEntities.contains(static_cast<uint32_t>(entityHandle));
					if (entityIsCutMarked)
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));

					if (m_RenamingEntity == entityHandle) {
						m_EntityRenameFrameCounter++;

						ImGui::PushItemWidth(-1);
						if (m_EntityRenameFrameCounter == 1) {
							ImGui::SetKeyboardFocusHere();
						}

						bool committed = ImGui::InputText("##EntityRename", m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer),
							ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

						if (committed) {
							std::string newName(m_EntityRenameBuffer);
							if (newName.empty()) {
								if (entity.HasComponent<NameComponent>()) {
									entity.RemoveComponent<NameComponent>();
									scene.MarkDirty();
								}
							}
							else if (entity.HasComponent<NameComponent>()) {
								entity.GetComponent<NameComponent>().Name = newName;
								scene.MarkDirty();
							}
							else {
								scene.AddComponent<NameComponent>(entity.GetHandle(), newName);
								scene.MarkDirty();
							}
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
							++m_SelectionVersion; // Bump so inspector re-reads FirstName; stale cache keeps showing pre-rename value.
						}
						else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
						}
						else if (m_EntityRenameFrameCounter > 2 && !ImGui::IsItemActive()) {
							std::string newName(m_EntityRenameBuffer);
							if (newName.empty()) {
								if (entity.HasComponent<NameComponent>()) {
									entity.RemoveComponent<NameComponent>();
									scene.MarkDirty();
								}
							}
							else if (entity.HasComponent<NameComponent>()) {
								entity.GetComponent<NameComponent>().Name = newName;
								scene.MarkDirty();
							}
							else {
								scene.AddComponent<NameComponent>(entity.GetHandle(), newName);
								scene.MarkDirty();
							}
							m_RenamingEntity = entt::null;
							m_EntityRenameFrameCounter = 0;
							// Same cache-invalidation reason as the committed branch above.
							++m_SelectionVersion;
						}

						ImGui::PopItemWidth();
					}
					else {
						bool entityLabelTruncated = false;
						const std::string entityLabel = ImGuiUtils::Ellipsize(entity.GetName(), ImGui::GetContentRegionAvail().x, &entityLabelTruncated);
						// Rounded highlight: suppress Selectable's square fill (transparent colors) and repaint a rounded rect on channel 0, behind the text on channel 1.
						ImDrawList* rowDraw = ImGui::GetWindowDrawList();
						rowDraw->ChannelsSplit(2);
						rowDraw->ChannelsSetCurrent(1);
						ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(0, 0, 0, 0));
						ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
						ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(0, 0, 0, 0));
						ImGui::Selectable(entityLabel.c_str(), selected);
						ImGui::PopStyleColor(3);
						const bool rowHovered = ImGui::IsItemHovered();
						const bool rowActive  = ImGui::IsItemActive();
						rowDraw->ChannelsSetCurrent(0);
						if (selected || rowHovered) {
							const ImU32 col = ImGui::GetColorU32(
								(rowActive && rowHovered) ? ImGuiCol_HeaderActive :
								rowHovered                ? ImGuiCol_HeaderHovered :
								                            ImGuiCol_Header);
							rowDraw->AddRectFilled(
								ImGui::GetItemRectMin(),
								ImGui::GetItemRectMax(),
								col,
								ImGui::GetStyle().FrameRounding);
						}
						rowDraw->ChannelsMerge();
						if (entityLabelTruncated && ImGui::IsItemHovered()) {
							ImGui::SetTooltip("%s", entity.GetName().c_str());
						}
						if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
							m_PressedEntity = entityHandle;
						}
						if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
							if (ImGui::IsItemHovered() && m_PressedEntity == entityHandle && !IsLeftMouseDragPastClickThreshold()) {
								const ImGuiIO& io = ImGui::GetIO();
								if (io.KeyShift) {
									SelectEntityRange(visibleIndex);
								}
								else if (io.KeyCtrl) {
									ToggleEntitySelection(entityHandle, visibleIndex);
								}
								else {
									SetSingleEntitySelection(entityHandle, visibleIndex);
								}
								// Record the owning scene so a follow-up
								// "Create Entity" lands in the same scene the
								// user is selecting from.
								if (!IsInPrefabEditMode()) {
									m_LastInteractedSceneName = scene.GetName();
								}
							}
							if (m_PressedEntity == entityHandle) {
								m_PressedEntity = entt::null;
							}
						}

						if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							m_RenamingEntity = entityHandle;
							m_EntityRenameFrameCounter = 0;
							std::snprintf(m_EntityRenameBuffer, sizeof(m_EntityRenameBuffer), "%s", entity.GetName().c_str());
						}
					}

					// SourceSceneId distinguishes same-scene reorder from cross-scene migration at the drop site.
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						HierarchyDragData dragData{
							entityIdx,
							static_cast<uint32_t>(entityHandle),
							static_cast<uint64_t>(scene.GetSceneId()),
						};
						ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragData, sizeof(dragData));
						const bool draggingMulti = IsEntitySelected(entityHandle) && &scene == selectionScene && m_SelectedEntities.size() > 1;
						if (draggingMulti) {
							ImGui::Text("Move: %zu entities", m_SelectedEntities.size());
						} else {
							ImGui::Text("Move: %s", entity.GetName().c_str());
						}
						ImGui::EndDragDropSource();
					}

					const ImVec2 itemRectMin = ImGui::GetItemRectMin();
					const ImVec2 itemRectMax = ImGui::GetItemRectMax();
					const float itemHeight = itemRectMax.y - itemRectMin.y;
					const float zoneHeight = itemHeight * 0.25f;

					// Suppress sibling zones on prefab root: the gate refuses them anyway but without suppression the indicator still flashes.
					const bool suppressSiblingZones = IsInPrefabEditMode()
						&& entityHandle == m_PrefabEditRootEntity;

					if (ImGui::BeginDragDropTarget()) {
						const float mouseY = ImGui::GetMousePos().y;
						const bool inTopZone = !suppressSiblingZones && mouseY < itemRectMin.y + zoneHeight;
						const bool inBottomZone = !suppressSiblingZones && mouseY > itemRectMax.y - zoneHeight;

						if (const ImGuiPayload* peek = ImGui::GetDragDropPayload()) {
							if (peek->IsDataType("HIERARCHY_ENTITY") && (inTopZone || inBottomZone)) {
								const float lineY = inTopZone ? itemRectMin.y : itemRectMax.y;
								const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
								ImGui::GetWindowDrawList()->AddLine(
									ImVec2(itemRectMin.x, lineY),
									ImVec2(itemRectMax.x, lineY),
									color, 2.0f);
							}
						}

						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
							auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
							EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
							const uint64_t targetSceneIdValue = static_cast<uint64_t>(scene.GetSceneId());
							const bool sameScene = dragData->SourceSceneId == targetSceneIdValue;
							// Reject cross-scene migration into prefab edit mode: would import foreign PrefabGUID/origin metadata into the asset.
							if (!sameScene && IsInPrefabEditMode()) {
								// drop ignored
							}
							else if (!sameScene) {
								Scene* sourceScene = nullptr;
								for (Scene* candidate : scenesToShow) {
									if (candidate && static_cast<uint64_t>(candidate->GetSceneId()) == dragData->SourceSceneId) {
										sourceScene = candidate;
										break;
									}
								}
								if (sourceScene && sourceScene != &scene) {
									std::vector<EntityHandle> sourceHandles = ResolveDraggedHierarchyEntities(*sourceScene, primaryDraggedHandle);
									std::vector<EntityHandle> migrated = MigrateEntitiesToScene(*sourceScene, scene, sourceHandles, Entity::Null);
									if (!migrated.empty()) {
										// Splice migrated handles into m_EntityOrder immediately; MoveSiblingNextTo only repositions entries already in the list.
										for (EntityHandle h : migrated) {
											m_EntityOrder.push_back(h);
										}
										if (inTopZone || inBottomZone) {
											EntityHandle anchor = entityHandle;
											if (inBottomZone) {
												for (EntityHandle h : migrated) {
													MoveSiblingNextTo(scene, h, anchor, /*insertAfter*/true);
													anchor = h;
												}
											}
											else {
												for (auto it = migrated.rbegin(); it != migrated.rend(); ++it) {
													MoveSiblingNextTo(scene, *it, anchor, /*insertAfter*/false);
													anchor = *it;
												}
											}
										}
										else {
											// Middle-zone reparent under `entity`.
											for (EntityHandle h : migrated) {
												SetEntityParentPreservingWorld(scene, h, entity);
											}
											m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
										}
										ClearEntitySelection();
										m_SelectedEntities = migrated;
										RebuildSelectionSet();
										m_SelectedEntity = migrated.back();
										m_LastInteractedSceneName = scene.GetName();
										m_EntityOrderDirty = true;
									}
								}
							}
							else {
							std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(scene, primaryDraggedHandle);
							bool didMove = false;
							if (inTopZone || inBottomZone) {
								// Multi-drop: advance anchor so each entity lands next to the previous one in selection order.
								EntityHandle anchor = entityHandle;
								if (inBottomZone) {
									for (EntityHandle h : draggedHandles) {
										if (h == anchor) continue;
										if (MoveSiblingNextTo(scene, h, anchor, /*insertAfter*/true)) {
											didMove = true;
											anchor = h;
										}
									}
								}
								else {
									for (auto it = draggedHandles.rbegin(); it != draggedHandles.rend(); ++it) {
										EntityHandle h = *it;
										if (h == anchor) continue;
										if (MoveSiblingNextTo(scene, h, anchor, /*insertAfter*/false)) {
											didMove = true;
											anchor = h;
										}
									}
								}
							}
							else {
								for (EntityHandle h : draggedHandles) {
									if (h == entityHandle) continue;
									Entity draggedEntity = scene.GetEntity(h);
									if (draggedEntity.IsAncestorOf(entity)) continue;
									if (SetEntityParentPreservingWorld(scene, h, entity)) {
										didMove = true;
									}
								}
								if (didMove) {
									m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
									// Force re-DFS: reparenting doesn't change entity count so the size-mismatch heuristic won't trigger it automatically.
									m_EntityOrderDirty = true;
								}
							}
							if (didMove) {
								scene.MarkDirty();
							}
							}
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
							// 1-arg form: asset-browser sends size()+1 bytes with a trailing '\0'; 2-arg would embed it in the string, breaking extension comparisons.
							std::string droppedPath(static_cast<const char*>(payload->Data));
							std::string ext = std::filesystem::path(droppedPath).extension().string();
							std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
							if (ext == ".prefab") {
								// Reject nested-prefab drop in prefab edit mode: would create a second root or allow self-recursion.
								if (!IsInPrefabEditMode()) {
									EntityHandle loaded = SceneSerializer::LoadEntityFromFile(scene, droppedPath);
									if (loaded != entt::null) {
										EnsureEditorUniqueEntityNames(scene, { loaded });
									}
									m_EntityOrder.clear(); m_EntityOrderDirty = true;
								}
							}
							else {
								std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
								EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
								bool scriptAttached = false;
								for (const auto& scriptEntry : droppedScripts) {
									if (scriptEntry.IsSceneScript || scriptEntry.IsGlobalScript) {
										continue;
									}
									if (scriptEntry.IsManagedComponent) {
										scriptAttached |= AttachManagedComponentToEntity(entity, scene, scriptEntry);
									}
									else {
										scriptAttached |= AttachScriptToEntity(entity, scene, scriptEntry);
									}
								}
								// Invalidate the inspector cache so the new
								// component shows up immediately if the dropped-on
								// entity happens to be selected.
								if (scriptAttached) ++m_SelectionVersion;
							}
						}
						ImGui::EndDragDropTarget();
					}

					// Pop tints before context menu: popups inherit the live style stack, so tints would bleed into menu item text.
					if (entityIsCutMarked)
						ImGui::PopStyleColor();
					if (entityIsPrefabTinted)
						ImGui::PopStyleColor();
					if (entityIsDisabled)
						ImGui::PopStyleColor();

					if (ImGui::BeginPopupContextItem())
					{
						RenderCreateEntityMenu(scene, entity);


						ImGui::Separator();

						if (!(IsEntitySelected(entityHandle) && &scene == selectionScene)) {
							SetSingleEntitySelection(entityHandle, visibleIndex);
							if (!IsInPrefabEditMode()) m_LastInteractedSceneName = scene.GetName();
						}

						if (ImGui::MenuItem("Delete", "Del"))
						{
							DeleteSelectedEntity(scene);
						}

						if (ImGui::MenuItem("Cut", "Ctrl + X"))
						{
							CutSelectedEntities(scene);
						}

						if (ImGui::MenuItem("Duplicate", "Ctrl + D"))
						{
							DuplicateSelectedEntity(scene);
						}

						if (ImGui::MenuItem("Rename", "F2"))
						{
							BeginRenameSelectedEntity(scene);
						}

						// "Unpack Prefab" only shows up when at least one
						// selected entity is currently a prefab instance —
						// no point cluttering the menu otherwise.
						bool anySelectedIsPrefabInstance = false;
						for (EntityHandle h : GetSelectedEntities(scene)) {
							if (scene.HasComponent<PrefabInstanceComponent>(h)) {
								anySelectedIsPrefabInstance = true;
								break;
							}
						}
						if (anySelectedIsPrefabInstance) {
							ImGui::Separator();
							if (ImGui::MenuItem("Unpack Prefab", "Shift + U")) {
								UnpackSelectedPrefabs(scene);
							}
						}

						ImGui::EndPopup();
					}

					ImGui::PopID();

					if (rowIndent > 0.0f) ImGui::Unindent(rowIndent);
				}
				} // while (clipper.Step())
				clipper.End();

				ImGui::PopStyleVar(); // ItemSpacing

				// TreePop matches the TreeNodeEx above, which prefab-edit
				// mode skips — balance the call accordingly.
				if (!IsInPrefabEditMode()) {
					ImGui::TreePop();
				}
			}

			ImGui::PopID();
		}

		// Deferred scene removal (after iteration)
		if (!sceneToRemove.empty()) {
			ClearEntitySelection();
			// Drop the last-interacted reference if it points at the
			// scene about to disappear, so GetContextScene falls back
			// to SceneManager's active scene next frame.
			if (m_LastInteractedSceneName == sceneToRemove) {
				m_LastInteractedSceneName.clear();
			}
			// Drop cut markers — without per-scene partitioning we can't
			// tell which cut entities belonged to the removed scene, so
			// clear the lot. Cheap and safe; the user can re-cut.
			m_CutEntities.clear();
			SceneManager::Get().UnloadScene(sceneToRemove);
		}

		if (pendingSceneReorder) {
			SceneManager::Get().MoveLoadedScene(pendingSceneReorder->SourceName, pendingSceneReorder->TargetIndex);
			m_EntityOrderDirty = true;
		}

		// Drag-drop target: only present during an active drag so it doesn't block right-click menus
		if (ImGui::GetDragDropPayload() != nullptr) {
			ImVec2 avail = ImGui::GetContentRegionAvail();
			if (avail.y > 0) {
				ImGui::InvisibleButton("##SceneDropTarget", ImVec2(-1, avail.y));
				if (ImGui::BeginDragDropTarget()) {
					// Drop to bottom panel: detaches entity to root and moves it to end of m_EntityOrder. Disabled in prefab mode (would create second root).
					if (!IsInPrefabEditMode())
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
						EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
						Scene* dropScene = GetContextScene();
						if (dropScene) {
							const uint64_t dropSceneIdValue = static_cast<uint64_t>(dropScene->GetSceneId());
							const bool sameScene = dragData->SourceSceneId == dropSceneIdValue;
							if (!sameScene) {
								Scene* sourceScene = nullptr;
								auto loadedScenes = SceneManager::Get().GetLoadedScenes();
								for (auto& weakScene : loadedScenes) {
									if (auto scenePtr = weakScene.lock();
										scenePtr && static_cast<uint64_t>(scenePtr->GetSceneId()) == dragData->SourceSceneId) {
										sourceScene = scenePtr.get();
										break;
									}
								}
								if (sourceScene && sourceScene != dropScene) {
									std::vector<EntityHandle> sourceHandles = ResolveDraggedHierarchyEntities(*sourceScene, primaryDraggedHandle);
									std::vector<EntityHandle> migrated = MigrateEntitiesToScene(*sourceScene, *dropScene, sourceHandles, Entity::Null);
									if (!migrated.empty()) {
										ClearEntitySelection();
										m_SelectedEntities = migrated;
										RebuildSelectionSet();
										m_SelectedEntity = migrated.back();
										m_LastInteractedSceneName = dropScene->GetName();
										m_EntityOrderDirty = true;
									}
								}
							}
							else {
							std::vector<EntityHandle> draggedHandles = ResolveDraggedHierarchyEntities(*dropScene, primaryDraggedHandle);
							bool didMove = false;
							for (EntityHandle h : draggedHandles) {
								Entity draggedEntity = dropScene->GetEntity(h);
								if (draggedEntity.HasParent()) {
									if (SetEntityParentPreservingWorld(*dropScene, h, Entity::Null)) {
										didMove = true;
									}
								}
								auto orderIt = std::find(m_EntityOrder.begin(), m_EntityOrder.end(), h);
								if (orderIt != m_EntityOrder.end() && std::next(orderIt) != m_EntityOrder.end()) {
									m_EntityOrder.erase(orderIt);
									m_EntityOrder.push_back(h);
									m_EntityOrderDirty = true; // M30: re-DFS next render.
								}
							}
							if (didMove) {
								m_EntityOrderDirty = true;
								dropScene->MarkDirty();
							}
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						std::string droppedPath(static_cast<const char*>(payload->Data));
						std::string ext = std::filesystem::path(droppedPath).extension().string();
						std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
						// Scene-level drops create root entities; reject in prefab mode (violates single-root invariant; .prefab could recurse if same asset).
						if (IsInPrefabEditMode()) {
							// drop ignored
						}
						else if (ext == ".scene") {
							m_PendingSceneFileDrop = droppedPath;
						}
						else if (ext == ".prefab") {
							Scene* dropScene = GetContextScene();
							if (dropScene) {
								EntityHandle loaded = SceneSerializer::LoadEntityFromFile(*dropScene, droppedPath);
								if (loaded != entt::null) {
									EnsureEditorUniqueEntityNames(*dropScene, { loaded });
								}
								m_EntityOrder.clear(); m_EntityOrderDirty = true;
							}
						}
						else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
							Scene* dropScene = GetContextScene();
							if (dropScene) {
								std::string entityName = std::filesystem::path(droppedPath).stem().string();
								Entity newEntity = dropScene->CreateEntity(entityName);
								auto& sr = newEntity.AddComponent<SpriteRendererComponent>();
								sr.TextureHandle = TextureManager::LoadTexture(droppedPath);

								Texture2D* tex = TextureManager::GetTexture(sr.TextureHandle);
								if (tex && tex->IsValid() && tex->GetHeight() > 0) {
									float aspect = (float)tex->GetWidth() / (float)tex->GetHeight();
									auto& transform = newEntity.GetComponent<Transform2DComponent>();
									transform.Scale = { aspect, 1.0f };
								}

								EnsureEditorUniqueEntityNames(*dropScene, { newEntity.GetHandle() });
								SelectEntity(newEntity.GetHandle());
								dropScene->MarkDirty();
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			}
		}

		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
			&& ImGui::IsMouseReleased(ImGuiMouseButton_Left)
			&& !IsLeftMouseDragPastClickThreshold()
			&& !ImGui::IsAnyItemHovered()) {
			ClearEntitySelection();
		}

		m_IsEntitiesPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderSceneSystemsInspector(Scene& scene) {
		ImGui::Text("Scene: %s", scene.GetName().c_str());
		ImGui::SeparatorText("Systems");

		auto acceptDroppedSceneScripts = [&]() {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				// 1-arg form: 2-arg would embed trailing '\0' and break extension matching in CollectScriptFile.
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsSceneScript) {
						scene.AddSceneScript(scriptEntry.ClassName);
					}
				}
			}
		};

		const auto& systems = scene.GetSceneScriptClassNames();
		for (size_t i = 0; i < systems.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			const std::string& className = systems[i];

			// AllowOverlap lets trailing buttons hit-test over the header widget.
			bool open = ImGui::CollapsingHeader((className + "##system_header").c_str(),
				ImGuiTreeNodeFlags_AllowOverlap);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float btnW = ImGui::GetFrameHeight();
			const float spacing = style.ItemInnerSpacing.x;
			const float stripW = btnW * 4.0f + spacing * 3.0f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - stripW);

			bool enabled = scene.IsSceneScriptEnabled(className);
			if (ImGui::Checkbox("##enabled", &enabled)) {
				scene.SetSceneScriptEnabled(className, enabled);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", enabled ? "Disable SceneScript" : "Enable SceneScript");
			}

			ImGui::SameLine(0, spacing);
			if (ImGui::ArrowButton("##move_up", ImGuiDir_Up) && i > 0) {
				scene.MoveSceneScript(i, i - 1);
			}

			ImGui::SameLine(0, spacing);
			if (ImGui::ArrowButton("##move_down", ImGuiDir_Down) && i + 1 < systems.size()) {
				scene.MoveSceneScript(i, i + 1);
			}

			ImGui::SameLine(0, spacing);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.35f, 0.35f, 1.0f));
			const bool remove = ImGui::Button("X", ImVec2(btnW, btnW));
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove SceneScript");
			if (remove) {
				scene.RemoveSceneScript(i);
				ImGui::PopID();
				break;
			}

			if (open) {
				ImGui::Indent(8.0f);
				DrawSceneScriptFields(scene, className);
				ImGui::Unindent(8.0f);
			}

			ImGui::PopID();
		}

		// Pre-collect available systems so the button can be disabled when nothing
		// can be added — avoids the empty-popup sliver that looks broken.
		std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
		EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);
		size_t availableSystemCount = 0;
		for (const auto& entry : scriptEntries) {
			if (entry.IsSceneScript && !scene.HasSceneScript(entry.ClassName)) {
				++availableSystemCount;
			}
		}
		const bool hasAvailableSystem = availableSystemCount > 0;

		float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (!hasAvailableSystem) ImGui::BeginDisabled();
		if (Icons::ButtonWithIcon(Icons::Type::Plus, "Add System", ImVec2(buttonWidth, 0), true)) {
			ImGui::OpenPopup("AddSystemPopup");
			m_SystemSearchBuffer[0] = '\0';
		}
		// Scoped drag-drop target: only the "+ Add System" button accepts
		// dropped scripts. Previously the whole inspector InnerRect was a
		// drop zone which was confusing.
		if (ImGui::BeginDragDropTarget()) {
			acceptDroppedSceneScripts();
			ImGui::EndDragDropTarget();
		}
		if (!hasAvailableSystem) {
			ImGui::EndDisabled();
			// AllowWhenDisabled so hover still fires after BeginDisabled.
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("No game systems available.\nCreate one via Asset Browser > Create > Scripting > SceneScript (C#).");
			}
		}

		if (ImGui::BeginPopup("AddSystemPopup")) {
			Icons::TextIcon(Icons::Type::Search);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##SystemSearch", "Search systems...",
				m_SystemSearchBuffer, sizeof(m_SystemSearchBuffer));
			ImGui::Separator();

			std::string filter(m_SystemSearchBuffer);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			for (const auto& scriptEntry : scriptEntries) {
				if (!scriptEntry.IsSceneScript || scene.HasSceneScript(scriptEntry.ClassName)) {
					continue;
				}

				if (!filter.empty()) {
					std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
					std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
					if (lowerClassName.find(filter) == std::string::npos
						&& lowerPath.find(filter) == std::string::npos) {
						continue;
					}
				}

				const std::string label = BuildScriptMenuLabel(scriptEntry);
				const std::string path = scriptEntry.Path.string();
				if (ImGuiUtils::MenuItemEllipsis(label, path.c_str(), nullptr, false, true, 260.0f)) {
					scene.AddSceneScript(scriptEntry.ClassName);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

	}

	void ImGuiEditorLayer::RenderInspectorPanel(Scene& scene) {
		ImGui::Begin("Inspector");

		// Cache avoids O(N×M) loops per frame over (selection × components) at large selections.
		if (m_InspectorCache.Version != m_SelectionVersion) {
			RecomputeInspectorSelectionCache(scene);
			m_InspectorCache.Version = m_SelectionVersion;
		}
		const std::vector<EntityHandle>& selectedHandles = m_InspectorCache.Handles;
		if (selectedHandles.empty()) {
			m_SelectedEntity = entt::null;
			if (m_IsSceneNodeSelected) {
				RenderSceneSystemsInspector(scene);
				// Render picker popup here too: this early return bypasses the later RenderPopup call.
				ReferencePicker::RenderPopup();
			}
			else {
				RenderAssetInspector();
			}
			m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			ImGui::End();
			return;
		}

		std::vector<Entity> selectedEntities;
		selectedEntities.reserve(selectedHandles.size());
		for (EntityHandle h : selectedHandles) {
			selectedEntities.push_back(scene.GetEntity(h));
		}
		std::span<const Entity> entitySpan(selectedEntities);
		Entity entity = selectedEntities[0]; // primary, used for the drag-source label and prefab buttons
		if (!scene.IsValid(m_SelectedEntity)) {
			m_SelectedEntity = entity.GetHandle();
		}

		// ── Entity Header: Name + Toggles ──────────────────

		if (selectedEntities.size() > 1) {
			ImGui::TextDisabled("(%zu)", selectedEntities.size());
		}

		// Empty buffer + hint avoids the bug where Enter on a nameless entity (buffer pre-filled with "Entity") created a real NameComponent.
		const bool nameUniform = m_InspectorCache.NameUniform;
		char nameBuf[256]{};
		if (nameUniform && !m_InspectorCache.FirstName.empty()) {
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_InspectorCache.FirstName.c_str());
		}
		const char* nameHint = nameUniform ? "Entity" : "-";
		ImGui::SetNextItemWidth(-1);
		const bool nameSubmitted = ImGui::InputTextWithHint(
			"##EntityName", nameHint, nameBuf, sizeof(nameBuf),
			ImGuiInputTextFlags_EnterReturnsTrue);
		if (nameSubmitted) {
			std::string newName(nameBuf);
			for (Entity& e : selectedEntities) {
				if (newName.empty()) {
					if (e.HasComponent<NameComponent>()) {
						e.RemoveComponent<NameComponent>();
					}
				}
				else if (e.HasComponent<NameComponent>()) {
					e.GetComponent<NameComponent>().Name = newName;
				}
				else {
					scene.AddComponent<NameComponent>(e.GetHandle(), newName);
				}
			}
			scene.MarkDirty();
			// NameComponent can be added/removed and FirstName changes; the
			// next inspector frame must rebuild its cached header strings.
			++m_SelectionVersion;
		}

		// Tri-state checkboxes when values differ across selection; edits apply to all.
		{
			constexpr int kMixedValueFlag = 1 << 12;

			// Shows authored state, not effective in-hierarchy: InheritedDisabledTag doesn't flip the checkbox.
			bool isEnabled = m_InspectorCache.EnabledFirst;
			if (!m_InspectorCache.EnabledUniform) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
			const bool enabledChanged = ImGui::Checkbox("Enabled", &isEnabled);
			if (!m_InspectorCache.EnabledUniform) ImGui::PopItemFlag();
			if (enabledChanged) {
				for (Entity& e : selectedEntities) {
					e.SetEnabled(isEnabled);
				}
				scene.MarkDirty();
				// DisabledTag/InheritedDisabledTag presence changes — cached
				// EnabledFirst/EnabledUniform are now stale.
				++m_SelectionVersion;
			}

			ImGui::SameLine();

			bool isStatic = m_InspectorCache.StaticFirst;
			if (!m_InspectorCache.StaticUniform) ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
			const bool staticChanged = ImGui::Checkbox("Static", &isStatic);
			if (!m_InspectorCache.StaticUniform) ImGui::PopItemFlag();
			if (staticChanged) {
				for (Entity& e : selectedEntities) {
					if (isStatic && !e.HasComponent<StaticTag>()) e.AddComponent<StaticTag>();
					else if (!isStatic && e.HasComponent<StaticTag>()) e.RemoveComponent<StaticTag>();
				}
				scene.MarkDirty();
				// StaticTag presence changes — cached StaticFirst/StaticUniform
				// are now stale.
				++m_SelectionVersion;
			}
		}

		/* INFO(Ben-Scr): MetaData Doesn't need to be displayed
		if (const EntityMetaData* metaData = entity.GetMetaData()) {
			const char* originText = "Runtime";
			if (metaData->Origin == EntityOrigin::Scene) originText = "Scene";
			if (metaData->Origin == EntityOrigin::Prefab) originText = "Prefab";
			ImGui::TextDisabled("Origin: %s", originText);

			const uint64_t runtimeId = metaData->RuntimeID;
			ImGui::TextDisabled("RuntimeID: %llu", runtimeId);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
			if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(runtimeId).c_str());

			if (metaData->Origin == EntityOrigin::Scene && static_cast<uint64_t>(metaData->SceneGUID) != 0) {
				const uint64_t sceneGuid = static_cast<uint64_t>(metaData->SceneGUID);
				ImGui::TextDisabled("SceneGUID: %llu", sceneGuid);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
				if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(sceneGuid).c_str());
			}
			if (metaData->Origin == EntityOrigin::Prefab && static_cast<uint64_t>(metaData->PrefabGUID) != 0) {
				const uint64_t prefabGuid = static_cast<uint64_t>(metaData->PrefabGUID);
				ImGui::TextDisabled("PrefabGUID: %llu", prefabGuid);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to copy");
				if (ImGui::IsItemClicked()) ImGui::SetClipboardText(std::to_string(prefabGuid).c_str());

				if (ImGui::Button("Apply to Prefab")) {
					SceneSerializer::ApplyPrefabInstanceOverrides(scene, m_SelectedEntity);
				}
				ImGui::SameLine();
				if (ImGui::Button("Revert All")) {
					EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, m_SelectedEntity, {});
					if (replacement != entt::null) {
						SelectEntity(replacement);
						m_EntityOrder.clear(); m_EntityOrderDirty = true;
					}
				}
			}
		}
		*/

		m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		ImGui::Separator();

		const auto& registry = SceneManager::Get().GetComponentRegistry();

		std::type_index pendingRemoval = typeid(void);

		Json::Value prefabOverrides = Json::Value::MakeObject();
		const bool isSinglePrefabInstance = selectedEntities.size() == 1
			&& scene.GetEntityOrigin(entity.GetHandle()) == EntityOrigin::Prefab
			&& static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())) != 0;
		bool prefabSourceResolvable = false;
		// Always resolved to the actual prefab root: applying a child as if it were root would overwrite the prefab file with that child's subtree.
		EntityHandle prefabInstanceRoot = entt::null;
		// True iff ANY entity in the subtree has overrides; per-entity prefabOverrides only captures this entity's diff.
		bool hasSubtreeOverrides = false;
		if (isSinglePrefabInstance) {
			prefabSourceResolvable = SceneSerializer::ComputeInstanceOverrides(
				scene, entity.GetHandle(), prefabOverrides);
			if (prefabSourceResolvable) {
				prefabInstanceRoot = SceneSerializer::GetPrefabInstanceRoot(scene, entity.GetHandle());
				hasSubtreeOverrides = SceneSerializer::HasPrefabInstanceOverrides(scene, entity.GetHandle());
			}
		}

		if (isSinglePrefabInstance && prefabSourceResolvable && prefabInstanceRoot != entt::null) {
			// Disable Apply/Revert when there are no overrides: Apply would still trigger disk write + live-instance refresh for no semantic change.
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.66f, 0.95f, 1.0f));
			ImGui::TextUnformatted(entity.GetHandle() == prefabInstanceRoot
				? "Prefab Instance"
				: "Prefab Instance (child)");
			ImGui::PopStyleColor();
			ImGui::BeginDisabled(!hasSubtreeOverrides);
			if (ImGui::SmallButton("Apply All")) {
				// Always act on prefab root (not selected entity); capture OLD source before overwriting so live-instance propagation can diff overrides.
				const std::string prefabPath = AssetRegistry::ResolvePath(
					static_cast<uint64_t>(scene.GetPrefabGUID(prefabInstanceRoot)));
				Json::Value previousSourceEntity;
				bool havePrev = false;
				if (!prefabPath.empty() && File::Exists(prefabPath)) {
					Json::Value previousRoot;
					std::string readError;
					if (SceneSerializerStorage::ReadRootFromFile(prefabPath, previousRoot, &readError) && previousRoot.IsObject()) {
						if (const Json::Value* eb = previousRoot.FindMember("Entity")) { previousSourceEntity = *eb; havePrev = true; }
						else if (const Json::Value* lb = previousRoot.FindMember("prefab")) { previousSourceEntity = *lb; havePrev = true; }
					}
				}
				if (SceneSerializer::ApplyPrefabInstanceOverrides(scene, prefabInstanceRoot) && havePrev) {
					const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(prefabInstanceRoot));
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						std::vector<EntityHandle> targets;
						auto view = s.GetRegistry().view<EntityMetaDataComponent, PrefabInstanceComponent>();
						for (entt::entity e2 : view) {
							const auto& meta = view.get<EntityMetaDataComponent>(e2).MetaData;
							if (meta.Origin != EntityOrigin::Prefab) continue;
							if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
							if (e2 == prefabInstanceRoot && &s == &scene) continue; // skip the just-applied root
							targets.push_back(e2);
						}
						bool anyRefreshed = false;
						for (EntityHandle t : targets) {
							if (SceneSerializer::RefreshPrefabInstance(s, t, previousSourceEntity) != entt::null) {
								anyRefreshed = true;
							}
						}
						if (anyRefreshed) s.MarkDirty();
					});
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Revert All")) {
				// RevertPrefabInstanceOverride destroys the root and returns a replacement; bail out of the inspector body this frame to avoid iterating destroyed handles.
				const EntityHandle revertedRoot = prefabInstanceRoot;
				EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, revertedRoot, {});
				if (replacement != entt::null) {
					if (m_PrefabEditRootEntity == revertedRoot) {
						m_PrefabEditRootEntity = replacement;
					}
					SelectEntity(replacement); // also bumps m_SelectionVersion
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
					ImGui::EndDisabled();
					ImGui::Separator();
					ImGui::End();
					return;
				}
			}
			ImGui::EndDisabled();
			ImGui::Separator();
		}
		else if (isSinglePrefabInstance) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.30f, 1.0f));
			ImGui::TextUnformatted("Prefab Instance (orphaned — source missing)");
			ImGui::PopStyleColor();
			ImGui::Separator();
		}

		// Bucket override paths by component name once per frame for O(1) per-component lookup (was O(components × paths) per render).
		std::unordered_map<std::string, std::vector<std::string>> overridesByComponent;
		if (prefabSourceResolvable) {
			for (const auto& [path, _] : prefabOverrides.GetObject()) {
				const std::size_t dot = path.find('.');
				const std::string componentKey = (dot == std::string::npos)
					? path
					: path.substr(0, dot);
				overridesByComponent[componentKey].push_back(path);
			}
		}

		// Lambda: do any override paths fall under this component's serializedName?
		auto componentHasOverrides = [&](const std::string& serializedName) -> bool {
			if (!prefabSourceResolvable || serializedName.empty()) return false;
			auto it = overridesByComponent.find(serializedName);
			return it != overridesByComponent.end() && !it->second.empty();
		};

		const std::vector<std::string>& hiddenPartialComponents = m_InspectorCache.PartialComponents;

		// Latch: a Revert destroys+recreates `entity`; remaining ForEachComponentInfo iterations this frame must be no-ops to avoid use-after-destroy.
		bool selectionInvalidatedByRevert = false;
		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (selectionInvalidatedByRevert) return;
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return; // Shown in entity header

			if (m_InspectorCache.CommonComponentTypes.find(typeId)
				== m_InspectorCache.CommonComponentTypes.end()) { // O(1) cache gate vs per-frame O(N) info.has() sweep
				return;
			}

			// Scripts render their own per-script sections — skip the outer wrapper
			if (info.displayName == "Scripts") {
				DispatchComponentInspector(info, entitySpan);
				return;
			}

			// Dynamic components with no drawInspector/properties render as empty sections; their fields appear in the paired ScriptComponent.Scripts entry.
			if (info.isDynamic && !info.drawInspector && info.properties.empty()) {
				return;
			}

			bool removeRequested = false;
			bool copyRequested = false;
			bool pasteRequested = false;
			bool resetRequested = false;
			ComponentClipboardPayload clipboardPayload;
			const bool hasComponentClipboard = TryReadComponentClipboard(m_ComponentClipboardJson, clipboardPayload);
			const ComponentInfo* pastedComponentInfo = hasComponentClipboard
				? FindComponentInfoBySerializedName(registry, clipboardPayload.SerializedName)
				: nullptr;
			const bool canPasteComponent = hasComponentClipboard
				&& pastedComponentInfo
				&& pastedComponentInfo->add
				&& pastedComponentInfo->has
				&& clipboardPayload.Data.IsObject();
			const bool canSerializeComponent = !info.serializedName.empty();

			const std::string componentSerializedName = info.serializedName;
			const bool thisComponentOverridden = componentHasOverrides(componentSerializedName);

			bool revertComponentRequested = false;
			bool applyComponentRequested = false;
			std::string revertFieldRequested; // empty = none

			bool open = ImGuiUtils::BeginComponentSection(info.displayName.c_str(), removeRequested, [&]() {
				if (ImGui::MenuItem("Copy Component", nullptr, false, canSerializeComponent)) {
					copyRequested = true;
				}
				if (ImGui::MenuItem("Paste Component", nullptr, false, canPasteComponent)) {
					pasteRequested = true;
				}
				if (ImGui::MenuItem("Reset Component", nullptr, false, canSerializeComponent)) {
					resetRequested = true;
				}
				// Prefab override actions — only meaningful when this entity is a
				// prefab instance whose source resolves AND this component has
				// at least one diff against the source.
				if (isSinglePrefabInstance && prefabSourceResolvable && thisComponentOverridden) {
					ImGui::Separator();
					if (ImGui::BeginMenu("Revert Field")) {
						const std::string prefix = componentSerializedName + ".";
						for (const auto& [path, _] : prefabOverrides.GetObject()) {
							if (path != componentSerializedName && path.compare(0, prefix.size(), prefix) != 0) continue;
							const char* leaf = path.c_str() + (path.size() > prefix.size() ? prefix.size() : 0);
							if (ImGui::MenuItem(leaf)) {
								revertFieldRequested = path;
							}
						}
						ImGui::EndMenu();
					}
					if (ImGui::MenuItem("Revert Component")) {
						revertComponentRequested = true;
					}
					if (ImGui::MenuItem("Apply Component")) {
						applyComponentRequested = true;
					}
				}
			});

			if (thisComponentOverridden) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors::OverrideMarker);
				ImGui::TextUnformatted(" *");
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Component has per-field overrides relative to the source prefab.");
				}
			}

			// Embed persistent UUID (not RuntimeID): the drop target writes it to disk; RuntimeIDs are reallocated on reload.
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
				uint64_t persistentId = 0;
				if (Scene* scene = entity.GetScene()) {
					persistentId = scene->GetEntityPersistentID(entity.GetHandle());
				}
				if (persistentId == 0) {
					persistentId = entity.GetRuntimeID(); // fallback for entities with no UUIDComponent
				}
				std::string refStr = std::to_string(persistentId) + ":" + info.displayName;
				ImGui::SetDragDropPayload("COMPONENT_REF", refStr.c_str(), refStr.size() + 1);
				ImGui::Text("Ref: %s.%s", entity.GetName().c_str(), info.displayName.c_str());
				ImGui::EndDragDropSource();
			}

			if (removeRequested) {
				pendingRemoval = typeId;
			}
			if (copyRequested && canSerializeComponent) {
				Json::Value componentValue = SceneSerializer::SerializeComponent(scene, entity.GetHandle(), info.serializedName);
				if (!componentValue.IsNull()) {
					Json::Value root = Json::Value::MakeObject();
					root.AddMember("type", Json::Value("ComponentClipboard"));
					root.AddMember("component", Json::Value(info.serializedName));
					root.AddMember("displayName", Json::Value(info.displayName));
					root.AddMember("data", std::move(componentValue));
					m_ComponentClipboardJson = Json::Stringify(root, false);
				}
			}
			if (pasteRequested && canPasteComponent) {
				bool anyApplied = false;
				bool anyAdded = false;
				for (const Entity& e : selectedEntities) {
					if (!pastedComponentInfo->has(e)) {
						registry.AddWithDependencies(e, pastedComponentInfo->typeId);
						anyAdded = true;
					}
					if (SceneSerializer::DeserializeComponent(scene, e.GetHandle(),
						clipboardPayload.SerializedName, clipboardPayload.Data)) {
						anyApplied = true;
					}
				}
				if (anyApplied) scene.MarkDirty();
				// AddWithDependencies may have inserted a new component type
				// on one or more selected entities — the cached intersection
				// has to refresh so the new section appears.
				if (anyAdded) ++m_SelectionVersion;
			}
			if (resetRequested && canSerializeComponent) {
				bool anyReset = false;
				for (const Entity& e : selectedEntities) {
					if (SceneSerializer::ResetComponent(scene, e.GetHandle(), info.serializedName)) {
						anyReset = true;
					}
				}
				if (anyReset) scene.MarkDirty();
			}

			if (!revertFieldRequested.empty()) {
				EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(
					scene, entity.GetHandle(), revertFieldRequested);
				if (replacement != entt::null) {
					SelectEntity(replacement);
					m_EntityOrder.clear(); m_EntityOrderDirty = true;
					// Entity was recreated — `entity`/`entitySpan` are now stale.
					// Stop iterating components this frame (see latch above).
					selectionInvalidatedByRevert = true;
					return;
				}
			}
			if (revertComponentRequested) {
				const std::string prefix = componentSerializedName + ".";
				std::vector<std::string> paths;
				for (const auto& [path, _] : prefabOverrides.GetObject()) {
					if (path == componentSerializedName || path.compare(0, prefix.size(), prefix) == 0) {
						paths.push_back(path);
					}
				}
				EntityHandle current = entity.GetHandle();
				for (const std::string& p : paths) {
					EntityHandle replacement = SceneSerializer::RevertPrefabInstanceOverride(scene, current, p);
					if (replacement != entt::null) current = replacement;
				}
				SelectEntity(current);
				m_EntityOrder.clear(); m_EntityOrderDirty = true;
				// Entity may have been recreated by the reverts above — `entity`/
				// `entitySpan` are now stale. Stop iterating components this frame.
				selectionInvalidatedByRevert = true;
				return;
			}
			if (applyComponentRequested) {
				const std::string prefabPath = AssetRegistry::ResolvePath(
					static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())));
				Json::Value previousSourceEntity;
				bool havePrev = false;
				if (!prefabPath.empty() && File::Exists(prefabPath)) {
					Json::Value previousRoot;
					std::string readError;
					if (SceneSerializerStorage::ReadRootFromFile(prefabPath, previousRoot, &readError) && previousRoot.IsObject()) {
						if (const Json::Value* eb = previousRoot.FindMember("Entity")) { previousSourceEntity = *eb; havePrev = true; }
						else if (const Json::Value* lb = previousRoot.FindMember("prefab")) { previousSourceEntity = *lb; havePrev = true; }
					}
				}
				if (SceneSerializer::ApplyPrefabInstanceOverrides(scene, entity.GetHandle()) && havePrev) {
					const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle()));
					SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
						std::vector<EntityHandle> targets;
						auto view = s.GetRegistry().view<EntityMetaDataComponent>();
						for (entt::entity e2 : view) {
							const auto& meta = view.get<EntityMetaDataComponent>(e2).MetaData;
							if (meta.Origin != EntityOrigin::Prefab) continue;
							if (static_cast<uint64_t>(meta.PrefabGUID) != prefabGuid) continue;
							if (e2 == entity.GetHandle() && &s == &scene) continue;
							targets.push_back(e2);
						}
						bool anyRefreshed = false;
						for (EntityHandle t : targets) {
							if (SceneSerializer::RefreshPrefabInstance(s, t, previousSourceEntity) != entt::null) {
								anyRefreshed = true;
							}
						}
						if (anyRefreshed) s.MarkDirty();
					});
				}
			}

			if (open) {
				DispatchComponentInspector(info, entitySpan);
				ImGuiUtils::EndComponentSection();
			}
		});

		if (pendingRemoval != typeid(void)) {
			// Remove from EVERY selected entity that has it.
			registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
				if (typeId != pendingRemoval || !info.remove) return;
				for (const Entity& e : selectedEntities) {
					if (info.has(e)) info.remove(e);
				}
			});
			scene.MarkDirty();
			// Component-presence map changed — the cached
			// CommonComponentTypes / PartialComponents must rebuild.
			++m_SelectionVersion;
		}

		if (!hiddenPartialComponents.empty()) {
			std::string text = "Hidden: ";
			for (std::size_t i = 0; i < hiddenPartialComponents.size(); ++i) {
				if (i > 0) text += ", ";
				text += hiddenPartialComponents[i];
			}
			text += " - not on all selected";
			ImGui::TextDisabled("%s", text.c_str());
		}

		ReferencePicker::RenderPopup();

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (Icons::ButtonWithIcon(Icons::Type::Plus, "Add Component", ImVec2(buttonWidth, 0), true)) {
			ImGui::OpenPopup("AddComponentPopup");
			m_ComponentSearchBuffer[0] = '\0';
		}

		// MUST sit immediately after the button: placing it after RenderAddComponentPopup binds to a different LastItemData and silently no-ops.
		bool scriptDroppedSomething = false;
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				// 1-arg form: see entity-row drop comment above.
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsSceneScript || scriptEntry.IsGlobalScript) {
						continue;
					}
					for (const Entity& e : selectedEntities) {
						if (scriptEntry.IsManagedComponent) {
							scriptDroppedSomething |= AttachManagedComponentToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
						else {
							scriptDroppedSomething |= AttachScriptToEntity(const_cast<Entity&>(e), scene, scriptEntry);
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		bool addComponentChanged = false;
		RenderAddComponentPopup("AddComponentPopup", scene, entitySpan,
			m_ComponentSearchBuffer, sizeof(m_ComponentSearchBuffer),
			&addComponentChanged);
		if (addComponentChanged || scriptDroppedSomething) {
			++m_SelectionVersion;
		}

		// IsAnyItemActive fires on focus/click; ActiveIdHasBeenEditedThisFrame only fires on real value edits (drag step, keystroke).
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
			const ImGuiContext& g = *ImGui::GetCurrentContext();
			if (g.ActiveId != 0 && g.ActiveIdHasBeenEditedThisFrame) {
				scene.MarkDirty();
			}
		}
		m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		ImGui::End();
	}
}
