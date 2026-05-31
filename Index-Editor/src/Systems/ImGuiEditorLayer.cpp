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
		void StartPlayOnAwakeComponents(Scene& scene)
		{
			auto audioView = scene.GetRegistry().view<AudioSourceComponent>(entt::exclude<DisabledTag>);
			for (auto [ent, audio] : audioView.each()) {
				if (audio.GetPlayOnAwake() && audio.GetAudioHandle().IsValid()) {
					audio.Play();
				}
			}

			auto particleView = scene.GetRegistry().view<ParticleSystem2DComponent>(entt::exclude<DisabledTag>);
			for (auto [ent, particleSystem] : particleView.each()) {
				if (particleSystem.PlayOnAwake) {
					particleSystem.Play();
				}
			}
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

			// For `: IComponent` structs (DynamicComponentRegistrar registered
			// these at user-assembly load), also populate the backing
			// DynamicComponentStorage so the entity actually lands in the ECS
			// pool — not just in ScriptComponent.Scripts. Without this the
			// inspector renders fine (it reads the managed instance), but a
			// user script's Entity.HasNativeComponent<T>() / GetRef<T>() looks
			// at the storage and sees nothing.
			//
			// Done unconditionally (not gated on `!alreadyHadScript`) so a
			// re-attach after a prior failed registration — e.g. the .cs
			// added the script before the assembly successfully exposed the
			// IComponent struct, then the user re-tried after fixing it —
			// still seeds the dynamic storage on the second attempt.
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
			// Byte-offset span within the source message where the clickable
			// "<path>:line N" substring lives. The console renderer uses this
			// to style that exact substring as the link inline within the
			// message text — no separate "Open …" prefix widget required.
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
		Application::SetIsPlaying(false);
		ApplicationEditorAccess::SetGameInputEnabled(false);
		Gizmo::SetShowInRuntime(false);
		if (app.GetRenderer2D()) {
			app.GetRenderer2D()->SetSkipBeginFrameRender(true);
		}
		// UI must render *into* the per-panel FBO (Editor / Game View),
		// not the OS window backbuffer that ImGui will paint over. Skip
		// the auto BeginFrame render and drive RenderScene manually from
		// RenderSceneIntoFBO while the panel's FBO is bound.
		if (app.GetGuiRenderer()) {
			app.GetGuiRenderer()->SetSkipBeginFrameRender(true);
		}

		if (m_LogSubscriptionId.value != 0) {
			Log::OnLog.Remove(m_LogSubscriptionId);
		}

		// Profiler panel — initialize once when the editor layer attaches.
		// Lazy first-render still loads project settings, so doing this
		// before a project is loaded is fine.
		m_ProfilerPanel.Initialize();
		m_SpriteEditorPanel.Initialize();

		// User-scoped preferences (theme, editor font, asset-browser /
		// auto-save toggles). Load BEFORE ApplyTheme so the chosen mode
		// is honored on the first frame; the ImGuiContextLayer ran its
		// initial ApplyIndexTheme during its own OnAttach (before this
		// layer's OnAttach), so a subsequent ApplyTheme here just reskins
		// the already-initialized style.
		EditorPreferences::Load();
		EditorPreferences::ApplyTheme();
		m_EditorPreferencesPanel.Initialize();

		// Legacy-project migration: pre-2026-05 index-project.json files
		// stored ShowFileExtensions / AutoSaveScenes / AutoSaveIntervalSeconds
		// inline. Seed EditorPreferences from those values on a fresh
		// install (the user has no prefs file yet, so we can't be clobbering
		// anything). Subsequent project opens skip migration so we don't
		// trample the user's choices. The next IndexProject::Save() drops
		// the legacy fields regardless.
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
			// Only show user-facing logs in editor panel (Client + EditorConsole)
			// Core/engine logs still go to stdout but not the editor UI
			if (entry.Source == Log::Type::Core) return;

			std::shared_ptr<LogDispatchState> state = weakLogDispatchState.lock();
			if (!state) return;

			// M31: never block worker threads on the editor's log mutex.
			// `try_lock` first; if the main thread is mid-drain, stash the
			// entry in a per-thread buffer and drain it on the next
			// successful lock from the same thread. This means an entry
			// from a thread that never logs again is lost — acceptable for
			// debug log dispatch (the engine logger still wrote it to
			// stdout / spdlog upstream of OnLog).
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
		// Reset the game-view log overlay BEFORE the rest of teardown so its destructor
		// (which unsubscribes from Log::OnLog) runs while the editor layer is still in
		// a sane state. The Event<>::Remove now blocks until any in-flight Log::Invoke
		// finishes, but resetting early keeps the unsubscribe close to the subscribe
		// site for clarity.
		m_GameViewLogOverlay.reset();
		ClearPreviewTextureCache();

		// Reverse construction order: panels → icons → FBOs.
		m_AssetBrowser.Shutdown();
		m_PackageManagerPanel.Shutdown();
		m_PackageManager.Shutdown();
		// M29: join in-flight ExternalEditor launcher threads so they
		// don't outlive editor shutdown. May briefly block on the
		// Sleep(4000) cold-start VS path — see ExternalEditor.cpp.
		ExternalEditor::JoinPendingLaunchThreads();
		// Picker statics (TU-locals) survive Application::Reload otherwise —
		// reset them here so the next attach re-runs the built-ins scan
		// against whatever AssetRegistry the new project provides.
		// H21/H22: ReferencePicker. M28: ScriptComponentInspector script picker.
		ReferencePicker::Shutdown();
		ScriptComponentInspector::Shutdown();
		EditorIcons::Shutdown();
		// Drop the shared vector/PNG icon cache as well. Same rule as
		// EditorIcons::Shutdown — Texture2D destructors need the WebGPU
		// device still alive, so run this before the renderer winds down.
		Icons::Shutdown();
		// Framebuffers are RAII-managed; explicit destruction here mirrors
		// the historical OnDetach order (drop GPU resources before any
		// later teardown that might re-enter the renderer).
		m_EditorViewFBO.Destroy();
		m_GameViewFBO.Destroy();
	}

	void ImGuiEditorLayer::OnEvent(Application& /*app*/, IndexEvent& event) {
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowFocusEvent>([this](WindowFocusEvent&) {
			m_AssetBrowser.RequestRefresh();
			return false;
			});

		// Scene swaps invalidate every entity handle the editor was holding
		// (selection, hierarchy order cache, inspector cache, drag/cut state).
		// Without resetting these on Pre/Post stop, a script-driven LoadScene
		// (or any path that destroys+rebuilds the active scene) leaves the
		// hierarchy iterating destroyed EntityHandles — the IsValid guards
		// filter every row, ImGuiListClipper asserts "Failed to calculate
		// item height", and a downstream Scene::GetEntity hits its
		// IDX_CORE_ASSERT.  Wired here so the reset runs synchronously inside
		// SceneManager::LoadSceneInternal / ReleaseScene, before any consumer
		// of the editor's cached state sees the new world.
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
			m_EntityOrderSceneId = 0;
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
			// Belt-and-braces: PreStop already cleared everything, but the
			// load path can be called WITHOUT a prior unload (additive load,
			// runtime-only scenes). PostStart guarantees the editor is back
			// in a clean state regardless of how the scene reached "live".
			resetEditorEntityState();
			return false;
			});
	}

	void ImGuiEditorLayer::OnUpdate(Application& app, float dt) {
		DrainPendingLogEntries();
		RunAutoSaveTick(app, dt);
		// If the prefab source file was deleted while editing (e.g. via the
		// asset browser), bail out of edit mode before auto-save runs —
		// otherwise the auto-save tick would silently re-create the file at
		// the now-stale path and the editor would stay stuck on a phantom
		// asset. Run BEFORE RunPrefabAutoSaveTick so auto-save doesn't fire
		// on a path we're about to abandon.
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

		// Drain a script-driven Application.Quit() that fired this frame.
		// In editor it's wired to set m_EditorStopPlayRequested instead
		// of actually quitting; consume the flag and route through the
		// same audio-cleanup + RestoreEditorSceneAfterPlaymode path the
		// Stop button uses, so script and click produce identical state.
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

		// Per-frame stale-handle sweep: the inspector / hierarchy hold raw entt
		// handles across frames. EnTT's version field protects most reads (handle
		// recycling bumps the version, IsValid catches it), but a leftover stale
		// selection still confuses anything that doesn't IsValid-gate. Clear here
		// rather than peppering every consumer with checks.
		//
		// CRITICAL: when in prefab-edit mode the selection lives in the
		// detached prefab scene's registry, NOT the active scene's. Validating
		// against the active scene would IsValid()-fail every frame and clear
		// the selection — which is exactly what made prefab children
		// "unselectable" (clicking selected the entity, the next frame's sweep
		// reset it). Route through GetContextScene so the sweep matches the
		// scene the selection is *actually* drawn from.
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
			// Only resync the parallel set + bump the selection version when
			// the sweep actually removed something — otherwise we'd invalidate
			// the inspector's cached state every single frame for no reason.
			if (m_SelectedEntities.size() != before) {
				RebuildSelectionSet();
			}
		}
		UpdateEditorCameraFocus(dt);
		const bool hasShortcutFocus = HasEntityShortcutFocus();
		const ImGuiIO& io = ImGui::GetIO();
		// Shortcut handlers (focus / duplicate / rename / etc.) act on the
		// scene the user is editing, which is the prefab edit scene in prefab
		// mode. Hand them the same context-aware reference the sweep used so
		// F-to-focus / Ctrl+D / Delete / F2 work on prefab entities, not just
		// the (hidden) active scene.
		Scene& shortcutScene = selectionScene;
		const bool hasEntitySelection = !GetSelectedEntities(shortcutScene).empty();

		if (!io.WantTextInput && !ImGui::IsAnyItemActive()
			&& input.GetKeyDown(KeyCode::F) && hasShortcutFocus && hasEntitySelection) {
			FocusSelectedEntity(shortcutScene);
		}

		if (!hasShortcutFocus) {
			return;
		}
		// Hierarchy edit shortcuts (Ctrl+C/V/D, Delete, F2) stay live in
		// play mode — RestoreEditorSceneAfterPlaymode reloads the scene
		// from disk on stop, so any duplicate / paste / delete / rename
		// during play is purely transient and matches the user's mental
		// model of iteration. Save (Ctrl+S) is gated separately on the
		// menu, so it can't accidentally persist play-mode state.

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

		// Use ImGui's KeyCtrl + IsKeyPressed instead of the raw Input wrapper.
		// The engine's KeyCode::LeftControl misses RIGHT Ctrl (and Cmd on macOS),
		// which is why the prior code silently failed for users on RHS keyboards.
		// IsKeyPressed(_, repeat=false) also prevents auto-repeat from firing
		// the action multiple times when the key is held. Same pattern as the
		// Save shortcut wired up in ImGuiEditorLayerChrome.cpp.
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
		// Auto-save toggle + interval live on EditorPreferences (user-
		// scoped) since 2026-05; the project pointer is only needed
		// below to resolve the on-disk path for the active scene.
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project || !EditorPreferences::GetAutoSaveScenes()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Skip Play mode entirely — entering Play already saves the scene
		// to disk, and Play-mode mutations are deliberately discarded on
		// Stop. Auto-saving over the pre-Play snapshot would persist
		// transient state and surprise the user on Stop.
		if (Application::GetIsPlaying()) {
			m_AutoSaveAccumulator = 0.0f;
			return;
		}

		// Reset the timer when the active scene changes — the new scene
		// hasn't been edited yet, and counting against its lifetime
		// from the moment it loaded would auto-save it the instant the
		// user makes any change instead of after the configured interval.
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
			// Nothing to save; let the accumulator drain back to zero so
			// the next dirty edit gets the full interval before its first
			// auto-save fires.
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
		// Only relevant while a detached prefab scene is being edited.
		if (!m_PrefabEditScene) return;
		if (!EditorPreferences::GetAutoSavePrefabs()) return;
		// Prefab edits during play mode are persistent: the detached prefab
		// scene is independent of the active play scene, so auto-saving is
		// safe and matches the user's expectation that prefab work is real
		// authoring work even when play mode happens to be running.

		// Authoritative dirty bit lives on the detached scene; every
		// component edit, add, remove, parent, and entity create/destroy
		// routes through Scene::MarkDirty. m_PrefabEditDirty is just a
		// per-frame mirror updated during the entities panel.
		if (!m_PrefabEditScene->IsDirty()) return;

		// Debounce against in-flight widget interactions. While the user is
		// dragging a slider, holding an InputText, or otherwise has any
		// ImGui widget claimed as active, IsAnyItemActive() stays true and
		// we wait — saving every frame of a drag would thrash disk and
		// fire propagation on every intermediate value. On release the
		// flag clears and the next tick saves. Mirrors the proven pattern
		// in PrefabInspector::RenderImpl.
		//
		// Caveat: this only debounces ImGui-owned widgets. Transform-gizmo
		// drags and hierarchy drag-reorder use ImGui's active-id system
		// too, so in practice they're covered as well; if a future
		// non-ImGui drag interaction needs explicit handling, gate this on
		// `!ImGuizmo::IsUsing()` etc.
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
		// Offset below the custom titlebar row (0 when disabled). The
		// titlebar window pinned at viewport->Pos owns the top rows;
		// the dockspace takes the remaining client area.
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
				// Mirror the Ctrl+S handler: save every dirty loaded scene,
				// falling back to the active scene when nothing is dirty so
				// the menu still has its "force-save current scene" behavior.
				// Without the loop a menu save could silently skip an
				// additive scene the user has been editing.
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
			// Unified Project Settings window. Side-tab nav covers six
			// categories — Display, Graphics, Branding, Build, Editor,
			// Systems — that together replace the old Project/Player
			// Settings split.
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

		// Tools — runtime/diagnostic utilities. Profiler panel is the
		// first inhabitant; later additions (memory tracker, log filter,
		// etc.) belong here too.
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
			// Snapshot EVERY loaded scene, not just the active one. Play mode is
			// restored by reloading each scene from its on-disk file (discarding
			// runtime mutations), so every loaded scene's file must reflect the
			// current editor state at entry. Skipping additive scenes here AND at
			// restore is what let their play-mode state leak back into edit mode.
			m_PlayModeScenes.clear();
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
		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(true);
		// Reset Time.TimeSinceStartup / Time.RealtimeSinceStartup so each
		// play session starts at t=0 from a script's perspective.
		ApplicationEditorAccess::MarkGameStart();
		scene.StartManagedGameSystemsForPlayMode();
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
			// Play mode is allowed even while a prefab is open for editing:
			// the prefab edit scene is detached from SceneManager so play
			// runs against the active scene independently. The user keeps
			// the editor viewport on the prefab and the Game View shows the
			// running scene — both panels stay live.
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

		ImGui::PopStyleVar();

		// Launch Standalone — spawns Index-Runtime.exe pointed at the
		// current project so the user can verify their game in a real OS
		// window without going through Build & Play (no project-specific
		// .exe build required). Available regardless of in-editor play
		// state — it's a separate process. Disabled while a project
		// build is running because that path also touches the runtime
		// binary; co-launching mid-build can race the file-copy step.
		//
		// Text button (not the play icon) on purpose: the Play icon is
		// already used a few pixels to the left for in-editor playmode,
		// and using the same glyph twice in one row was visually
		// confusing in early testing.
		ImGui::SameLine();
		const bool standaloneDisabled = (m_BuildState > 0)
			|| !ProjectManager::GetCurrentProject();
		if (standaloneDisabled) ImGui::BeginDisabled();
		if (ImGui::Button("Launch Standalone")) {
			LaunchStandalone();
		}
		if (standaloneDisabled) ImGui::EndDisabled();
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

		// (Editor View draw-mode selector lives in RenderEditorView now,
		// alongside the viewport's per-window options — same UX as Game
		// View's aspect / VSync row. Removing it from the toolbar gives
		// the play / pause / step / stop controls breathing room.)

		ImGui::End();

		// Handle step-frame logic: pause again after stepping one frame
		if (isPlaying && m_StepFrames > 0) {
			m_StepFrames--;
			if (m_StepFrames == 0) {
				ApplicationEditorAccess::SetPlaymodePaused(true);
				// SetPlaymodePaused only suspends gameplay update; audio runs on
				// miniaudio's own thread, so we mirror the Pause button's manual
				// capture. List is appended (not cleared) so any entries from a
				// prior explicit Pause survive to be resumed by Continue.
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

		// Locate Index-Runtime.exe. Two layouts we ship:
		//   * Dev tree:    <bin>/Index-Editor/Index-Editor.exe
		//                  <bin>/Index-Runtime/Index-Runtime.exe   (sibling)
		//   * Distributed: <root>/Index-Editor.exe
		//                  <root>/Index-Runtime.exe                 (same dir)
		// Try both before bailing — neither path constant is hard-coded
		// in the build pipeline, but Premake / package script invariably
		// emits one of these two shapes.
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

		// Flush the project to disk so the runtime — which loads
		// index-project.json fresh — sees the latest BuildAspect, scene
		// list, etc. Scene files are NOT auto-saved (that's a deliberate
		// user action; the standalone shows on-disk state, just like
		// after a fresh editor restart).
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
		// Reset pressed-state before scene reload: m_PressedEntity references an
		// EntityHandle from the play-mode scene that is about to be destroyed and
		// repopulated. Without this, the next hierarchy click could re-use the same
		// entt id and dispatch a stale click against an unrelated restored entity.
		m_PressedEntity = entt::null;

		// Prefab-edit mode keeps its own selection & hierarchy state in the
		// detached prefab scene, which play mode doesn't touch. Skip the
		// selection clear + UUID restore dance; otherwise stopping play
		// mode while a prefab is open would wipe the user's prefab
		// selection and m_EntityOrder, surprising them.
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

		ApplicationEditorAccess::SetPlaymodePaused(false);
		Application::SetIsPlaying(false);

		if (!m_PlayModeScenes.empty()) {
			// Reload EVERY scene that was loaded at play-mode entry from its
			// on-disk file, discarding all runtime mutations. Collect the
			// targets first (matching the snapshot by name against the
			// still-loaded set), then reload — so a reload can't perturb the
			// loaded-scene set mid-iteration. A scene the game unloaded during
			// play is skipped; a scene spawned during play isn't in the snapshot
			// and is left untouched. LoadFromFile tears down play-mode entities
			// (firing OnDisable thunks) before the OnPlayModeExited sweep below.
			std::vector<std::pair<Scene*, std::string>> toReload;
			SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
				for (const auto& [sceneName, scenePath] : m_PlayModeScenes) {
					if (s.GetName() == sceneName) {
						toReload.push_back({ &s, scenePath });
						break;
					}
				}
			});
			for (auto& [scenePtr, scenePath] : toReload) {
				SceneSerializer::LoadFromFile(*scenePtr, scenePath);
			}
			m_PlayModeScenes.clear();
		}

		// Sweep any static-event subscriber whose backing method lives in
		// the user assembly. Runs AFTER the scene reload so every
		// EntityScript.OnDisable has had its chance to `-=` first — a
		// script that properly pairs `+=` in OnEnable with `-=` in
		// OnDisable should leave zero leftovers and trigger no warning.
		// Without this ordering the sweep fires before SceneSerializer::
		// LoadFromFile tears down play-mode entities (which is what
		// invokes the InvokeOnDisable thunks), so well-behaved scripts
		// still got the warning. Anything still attached at this point
		// is genuinely leaking — the managed side logs a "N registered
		// event(s) still active" warning pointing the user at the
		// OnEnable / OnDisable pattern.
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
		// Pressed-state must clear together with selection — m_PressedEntity is a
		// transient mouse-down cursor and surviving a selection change would dispatch
		// a click against an entity that is no longer the user's intended target.
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
		// O(1) hash lookup. Used per visible row in the hierarchy panel —
		// a linear scan over a 25k-entry vector here was the dominant
		// cost when the user had a large multi-selection.
		return m_SelectedEntitySet.find(entity) != m_SelectedEntitySet.end();
	}

	std::vector<EntityHandle> ImGuiEditorLayer::GetSelectedEntities(const Scene& scene) const {
		// Dedup via a local hash set rather than std::find — this is called
		// many times per frame (every shortcut handler, the inspector,
		// hierarchy context menus). With N=25k the old std::find pass was
		// O(N²) per call and dwarfed everything else in the frame.
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
		// O(1) membership check via the parallel hash set. The vector erase
		// is still O(N) but only runs on user toggle (not per-frame), so it's
		// not on the hot path.
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
		// One-shot rebuild of every per-selection derived fact the inspector
		// needs. Runs once per selection mutation (the per-frame inspector
		// check is just a version compare). Without this, the inspector did
		// a fresh O(N×M) sweep over (selection × components) every frame,
		// which is what produced the FPS collapse at large selections.
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
			// Empty FirstName encodes "no NameComponent" — the inspector's
			// InputText renders an "Entity" placeholder hint for that case,
			// instead of pre-filling the buffer with a literal "Entity"
			// (which the user could accidentally commit, creating a stray
			// NameComponent on the entity).
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

		// Fixed-duration animation so framing an entity takes the same
		// wall-clock time whether the camera is two units away or two
		// hundred. The previous exponential-decay version converged with
		// a distance-independent half-life, but the absolute completion
		// threshold meant the perceived duration grew with distance and
		// with how zoomed-out the start state was.
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

		// Reset pressed-state up-front: m_PressedEntity is a hierarchy-panel cursor
		// from a prior mouse-down and must clear whenever the selection invariants
		// change, otherwise the next click on a re-used entt id would dispatch a
		// click event against a stale (destroyed) entity.
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
		// Pre-fill only when the entity actually has a name. Entity::GetName()
		// synthesises "Unnamed Entity (<id>)" for nameless entities — committing
		// that placeholder verbatim would mint a real NameComponent with that
		// text. Empty buffer lets the user type fresh and lets the empty-commit
		// branch in the rename handler leave the entity nameless.
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

		// Reusing SetEntityMetaData(EntityOrigin::Scene) is the canonical
		// reverse of the create-prefab path: it clears PrefabGUID, drops the
		// PrefabInstanceComponent, allocates a fresh SceneGUID, and marks
		// the scene dirty when not playing — so unpack is the exact inverse
		// of "drag entity to Asset Browser".
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

		// New cut supersedes the previous one: drop the prior cut marker so
		// the previously-cut entities un-dim (they remain in the scene as
		// before — only the visual marker goes away). Then snapshot the
		// new selection into the clipboard and mark it as "pending move".
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
		// Prefab edit mode: a paste with no selection target would
		// otherwise create a sibling-to-root entity that the prefab
		// serializer drops on save (it walks the root's subtree only).
		// Force-route the paste under the prefab root to keep the
		// pasted entities visible in the saved .prefab.
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

			// Paste finalizes the move started by a Cut: destroy any
			// originals that were marked as cut. Skips entities that
			// already disappeared (stale handles) and entities that
			// somehow belong to a different scene. Clears the marker
			// either way so a follow-up Paste doesn't double-destroy.
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
		// Prefab edit mode constraint: every entity created inside a
		// prefab edit must live UNDER the prefab root (the prefab is a
		// single-rooted asset — siblings to the root would silently get
		// dropped on Save because SceneSerializer::SaveEntityToFile only
		// walks the root's subtree). Force-parent here so a "Create
		// Entity" via the right-click menu without a context entity
		// still ends up nested correctly. Auto-uses the root only for
		// "no explicit parent" calls; an explicit parent (e.g. via
		// "Create Child" on a deeper entity) wins.
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
				// Route through CreateWith (raw CreateEntityHandle) so the
				// entity isn't seeded with a Transform2DComponent — UI text
				// belongs on a RectTransform2D, and Transform2D conflicts
				// with it (declared via DeclareConflict in BuiltInComponent
				// Registration). Mirrors how the other UI presets are built.
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

		// Prefab edit-mode invariant: prefab files are single-rooted —
		// SceneSerializer::SaveEntityToFile only walks the prefab root's
		// subtree, so any entity outside that subtree silently disappears on
		// save. Refuse two operations that would break the invariant: (1)
		// unparenting any child to root (would create a second root), and
		// (2) moving the prefab root itself under another entity (would
		// orphan its subtree). Together these keep every non-root entity
		// inside the root's subtree at all times.
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

		// Snapshot first, destroy after — destroying a parent before its
		// children's snapshot finishes would dangle handles in the JSON
		// references. The clipboard format serializes the full subtree per
		// root so child entities don't need to be passed separately.
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

		// Destroy originals only after every clone landed safely. If a
		// deserialize aborted we still tear down the corresponding original
		// because the user gesture was "move", not "copy" — leaving the
		// original behind on a partial failure would silently duplicate the
		// entity instead.
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

		// If the dragged handle is part of the current multi-selection, the
		// gesture means "drag the whole selection." Otherwise (dragging an
		// unselected entity) we keep the single-entity behavior so the
		// existing selection isn't accidentally moved by clicking a
		// non-selected row and dragging it elsewhere.
		const bool draggedIsInSelection =
			std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), primary) != m_SelectedEntities.end();
		if (draggedIsInSelection && m_SelectedEntities.size() > 1) {
			// FilterSelectedHierarchyRoots collapses parent/descendant pairs
			// down to the parent — moving a parent already moves its
			// descendants, so processing both would double-move or fail.
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

		// Cross-branch move: re-attach to target's parent first. SetParent
		// appends to the new parent's Children, so the in-vector reposition
		// below still has work to do.
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
			// Root-level reorder: m_EntityOrder is the panel's master list and
			// drives the order roots render in. Mutating it here is what makes
			// the drop "stick" between frames — but the engine-side renderer
			// (UIDrawOrder::Build) and the serializer both iterate the entt
			// registry directly, NOT m_EntityOrder, so without the sort below
			// the new order is invisible to render and lost on save+reload.
			repositionVec(m_EntityOrder);
			m_EntityOrderDirty = true; // M30: force re-DFS next render.

			// Mirror m_EntityOrder into entt's entity storage. entt's view
			// iterates dense storage newest-first; UIDrawOrder + the
			// serializer both reverse that into oldest-first. So we want
			// m_EntityOrder.back() to be the "newest" entity in entt (last
			// in m_EntityOrder = drawn on top = highest DrawIndex). Higher
			// m_EntityOrder index → sorts first in entt iteration.
			// Entities not tracked in m_EntityOrder (transient runtime
			// entities created mid-frame, etc.) get a stable tiebreaker
			// from their raw integral value so the comparator stays a
			// strict weak ordering even on the rare missing case.
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

		// If a prefab is already being edited, auto-save any pending edits
		// before swapping to the new one. The user explicitly asked to open
		// a different prefab, so silent loss-of-work is the wrong default —
		// the alternative (a modal) would require deferring this entire
		// function until the user picks save/discard, which is invasive.
		// Auto-save is safe: SavePrefabEditChanges is a no-op when nothing
		// changed, and identical to clicking the Save button otherwise.
		if (m_PrefabEditScene) {
			if (m_PrefabEditScene->IsDirty()) {
				SavePrefabEditChanges();
			}
			ClosePrefabEditing(false);
		}

		// Format-aware read: the project may be configured to serialize in
		// binary, in which case File::ReadAllText + Json::TryParse fails at
		// byte 0. SceneSerializerStorage::ReadRootFromFile picks the right
		// reader for the file's actual format (same path PrefabInspector
		// uses for asset-side editing).
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

		// Drop the panel's current view of the active scene before
		// swapping. EntityHandles in m_SelectedEntities / m_EntityOrder
		// belong to the active scene's registry and would alias garbage
		// in the detached prefab registry.
		ClearEntitySelection();
		m_EntityOrder.clear(); m_EntityOrderDirty = true;
		m_CollapsedHierarchyEntities.clear();
		m_CutEntities.clear();

		// Snapshot the editor camera so the user returns to their previous
		// framing on Close. We then point the camera at the prefab's root
		// transform — most prefabs are authored at (0, 0) so without this
		// the user lands on whatever the active scene was framing and the
		// prefab is invisible / off-screen ("changes don't show up" report).
		m_PrefabEditSavedCameraPosition = m_EditorCamera.GetPosition();
		m_PrefabEditSavedCameraOrthoSize = m_EditorCamera.GetOrthographicSize();
		m_PrefabEditSavedCameraZoom = m_EditorCamera.GetZoom();
		m_PrefabEditHasSavedCameraState = true;

		auto detached = Scene::CreateDetachedScene("##PrefabEdit");
		const EntityHandle rootEntity = SceneSerializer::DeserializeEntityFromValue(*detached, root);
		// DeserializeEntityFromValue marks the scene dirty as a side
		// effect; clear so our flag only flips on actual user edits.
		detached->ClearDirty();

		// World transforms aren't valid until the hierarchy system runs, but
		// we want to read the root's world position immediately to focus the
		// camera on it. Run a one-shot propagation against the freshly-loaded
		// detached scene so child-of-child positions resolve correctly even
		// though the prefab usually authors at the origin.
		TransformHierarchySystem::Propagate(*detached);

		m_PrefabEditScene = std::move(detached);
		m_PrefabEditPath = path;
		m_PrefabEditRootEntity = rootEntity;
		m_PrefabEditDirty = false;

		// Auto-select + auto-focus on the root. Without selection the user
		// has to click into the hierarchy to find the (potentially deeply
		// nested-named) root before they can edit anything; without focus
		// the camera may be pointed at empty space (see camera-snapshot
		// rationale above). Both fixes target the "I can't see what I'm
		// editing" case the prefab UX was failing on.
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

		// Capture the OLD source JSON BEFORE overwriting on disk. Live
		// instance propagation uses this as the baseline for computing each
		// instance's per-field overrides — diffing against the post-save
		// (new) source loses the user's per-instance overrides. Mirrors
		// PrefabInspector::Save().
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
				// File exists but is unreadable (locked by external tool,
				// corrupt, transient I/O). Overwriting now would orphan every
				// live instance — propagation needs the OLD source as a
				// baseline for the override diff. Bail out and leave the
				// detached scene dirty so the next auto-save tick retries
				// once the file is readable again.
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

		// Refresh live instances of this prefab in every loaded scene with
		// override-preservation: RefreshPrefabInstance snapshots the
		// instance's overrides relative to the OLD source, re-instantiates
		// against the new source, and re-applies the overrides on top.
		//
		// Propagation runs BEFORE we clear the dirty flag. If propagation
		// throws or otherwise fails mid-loop, the prefab stays dirty and the
		// next auto-save tick retries the full save+propagate. Previously the
		// dirty flag cleared first, leaving live instances stale with no
		// retry hook.
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
						// Orphaned-source case returns the original `t`
						// handle (nothing refreshed). Only credit a real
						// swap so we don't spuriously dirty scenes whose
						// instances weren't actually touched.
						if (replacement != entt::null && replacement != t) {
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

		// Restore the editor camera. Order matters: position must come last
		// because SetOrthographicSize / SetZoom recompute projection-only and
		// don't touch the view matrix; setting position last ensures the view
		// matrix is rebuilt from the restored Position field.
		if (m_PrefabEditHasSavedCameraState) {
			m_EditorCamera.SetOrthographicSize(m_PrefabEditSavedCameraOrthoSize);
			m_EditorCamera.SetZoom(m_PrefabEditSavedCameraZoom);
			m_EditorCamera.SetPosition(m_PrefabEditSavedCameraPosition);
			m_PrefabEditHasSavedCameraState = false;
		}
	}

	Scene* ImGuiEditorLayer::GetContextScene() const {
		if (m_PrefabEditScene) return m_PrefabEditScene.get();
		// Last-interacted scene wins so "Create Entity" lands where the
		// user expects with multiple scenes loaded. Fall back to the
		// SceneManager's active scene if the cached name is empty or
		// the scene has been unloaded since the last interaction.
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

		// Prefab-edit-mode toolbar. Drawn above the hierarchy so the
		// user always sees what context they're in and how to leave it.
		// We render this BEFORE the hierarchy so the right-click /
		// drag-drop targets below don't span the toolbar zone.
		if (IsInPrefabEditMode()) {
			// Authoritative dirty flag: the detached scene's IsDirty
			// reflects every component edit, add, remove, parenting
			// change, etc. routed through Scene::MarkDirty. Mirror it
			// onto m_PrefabEditDirty so other code reading the local
			// flag (e.g. the breadcrumb) stays consistent.
			m_PrefabEditDirty = m_PrefabEditScene->IsDirty();

			// Stem only (no .prefab extension) — toolbar context already
			// makes "this is a prefab" obvious, so the extension would
			// just be visual noise. Dirty asterisk is suppressed when
			// auto-save is on because the user can't act on it: the next
			// auto-save tick clears the dirty bit, so showing "*" would
			// flicker for one frame on every edit.
			const std::string prefabName = std::filesystem::path(m_PrefabEditPath).stem().string();
			const bool showDirtyMark = m_PrefabEditDirty && !EditorPreferences::GetAutoSavePrefabs();
			const std::string title = showDirtyMark ? (prefabName + " *") : prefabName;

			// Back button: just "<". Dirty-aware — prompts to save /
			// discard / cancel when the prefab has unsaved changes;
			// closes cleanly otherwise. The discard prompt is rendered
			// at the dockspace level (ImGuiEditorLayerChrome) so the
			// modal lands on top of the hierarchy panel rather than
			// trapped inside it.
			const float backButtonStartX = ImGui::GetCursorPosX();
			if (ImGui::SmallButton("<")) {
				if (m_PrefabEditDirty) {
					m_ShowPrefabEditDiscardPrompt = true;
				}
				else {
					ClosePrefabEditing(false);
				}
			}

			// Center the prefab name on the same line as the back button.
			// SameLine + SetCursorPosX moves the cursor for the next item;
			// we measure GetContentRegionAvail before the SameLine so the
			// width reflects the entire row (including the button's slot)
			// rather than just what's left after it.
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

		// In prefab-edit mode, the "current scene" for entity creation,
		// right-click menus, and the hierarchy iteration is the detached
		// prefab scene — not whatever the user had loaded before. The
		// active scene is still alive in SceneManager but hidden from
		// this panel until the user leaves prefab mode.
		Scene* activeScene = GetContextScene();

		// Right-click on empty space: create entities in the active scene
		if (activeScene && ImGui::BeginPopupContextWindow("EntityCreateContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			Scene& scene = *activeScene;
			RenderCreateEntityMenu(scene, Entity::Null);
			ImGui::EndPopup();
		}

		// Build the list of scenes to display. In prefab-edit mode we
		// show only the detached prefab scene; otherwise we mirror the
		// SceneManager's loaded scenes (the existing multi-scene flow).
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
		// Deferred scene-reorder. The drop site can't call
		// SceneManager::MoveLoadedScene mid-iteration because the snapshot
		// vector (scenesToShow) and m_LoadedScenes are read by every
		// subsequent loop body. We record the gesture and apply it after
		// the loop completes — same pattern as sceneToRemove.
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
			ImGui::PushID(reinterpret_cast<const void*>(static_cast<uintptr_t>(sceneIdValue)));

			// Prefab-edit mode skips the scene-name tree node entirely. The
			// detached prefab scene has no user-facing identity (it's named
			// "##PrefabEdit"), the prefab toolbar above already shows the
			// prefab name, and the scene-header drop target (which unparents
			// onto root) would create a second root in a prefab that must
			// stay single-rooted. Entities render directly under the toolbar.
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
				// Capture the header rect for the SCENE drag-drop zone
				// indicator (top/bottom line drawn when reordering scenes).
				// Done immediately after TreeNodeEx so subsequent ImGui
				// calls don't shift the "last item" rect underneath us.
				const ImVec2 sceneItemRectMin = ImGui::GetItemRectMin();
				const ImVec2 sceneItemRectMax = ImGui::GetItemRectMax();
				// Any interaction with this scene's row marks it as the
				// "create entity" target. Activation covers left-click,
				// expand-toggle, and right-click — exactly the actions
				// where the user is signalling "I'm working on this scene".
				if (ImGui::IsItemActivated()) {
					m_LastInteractedSceneName = scene.GetName();
				}
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
					SelectSceneNode();
				}

				// Drag source — the scene header itself is draggable so the
				// user can reorder scenes within the panel. Only one scene
				// loaded ⇒ no reorder is possible; suppress the source so a
				// drag in that case doesn't spin up a useless tooltip.
				if (scenesToShow.size() > 1 && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
					HierarchySceneDragData sceneDrag{ sceneIdValue };
					ImGui::SetDragDropPayload("HIERARCHY_SCENE", &sceneDrag, sizeof(sceneDrag));
					ImGui::Text("Move scene: %s", scene.GetName().c_str());
					ImGui::EndDragDropSource();
				}

				// Drop target — three payload types overlap on the header:
				//   • HIERARCHY_SCENE: reorder relative to this scene.
				//     Top half ⇒ insert above; bottom half ⇒ insert below.
				//   • HIERARCHY_ENTITY (same scene): unparent the dragged
				//     entities (existing behavior).
				//   • HIERARCHY_ENTITY (different scene): migrate the
				//     entities into this scene at root level.
				if (ImGui::BeginDragDropTarget()) {
					// Visual indicator for an in-flight SCENE drag — thin
					// line at the top or bottom edge of the header showing
					// which slot will receive the move. Drawn before the
					// AcceptDragDropPayload call so the indicator renders
					// while the drag is still hovering (not just on drop).
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
							// Find the source scene's current index in
							// scenesToShow so we can compute its target
							// position in the post-erase list. scenesToShow
							// mirrors m_LoadedScenes in order, so the
							// index translates 1:1 to MoveLoadedScene.
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
								// Final position of the source scene in the
								// resulting list. When source < target,
								// removing it shifts the target up by one
								// — the formulas below collapse that
								// adjustment so the user-visible "drop
								// above B" lands the source immediately
								// before B regardless of direction.
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
									// Selection lives across scenes but is
									// keyed by raw entt handle — drop the
									// source-scene selection so the post-
									// migrate inspector reflects the new
									// entities, not the stale originals.
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

				// M30: skip the sync diff + DFS subtree-emit pass when nothing
				// has changed. We detect change by:
				//   • m_EntityOrderDirty (flipped at every callsite that used
				//     to clear() the order: entity create / delete / drop /
				//     scene swap / etc., and at every collapse/expand toggle
				//     so the pruned m_VisibleEntityOrder rebuilds).
				//   • registry size mismatch as a safety net for any caller
				//     that mutates the registry without flipping the flag.
				// view.size() is O(1) for the entity-storage view (swap_only
				// deletion policy: returns the free-list cut-off, which is
				// the alive-entity count). The previous std::distance walk
				// was O(N) and dominated the editor frame at 100k entities.
				// size_hint() is only available on in_place policies and
				// won't compile here.
				const auto registryCount = static_cast<std::size_t>(view.size());
				// Scene-swap detection: if the iterated scene differs from the
				// one m_EntityOrder was last built for, force a rebuild. Without
				// this, a script-driven LoadScene that happens to produce the
				// same entity count as the previous scene leaves
				// m_VisibleEntityOrder pointing at destroyed handles — the
				// clipper below then fires "Failed to calculate item height"
				// because every row is skipped by the IsValid filter.
				if (m_EntityOrderSceneId != sceneIdValue) {
					m_EntityOrderDirty = true;
					m_EntityOrderSceneId = sceneIdValue;
				}
				const bool needsRebuild = m_EntityOrderDirty
					|| m_EntityOrder.size() != registryCount;

				if (needsRebuild) {
					INDEX_PROFILE_SCOPE("Editor.HierarchyRebuild");
					// Sync m_EntityOrder with the registry without ever blanket-
					// resetting it: the user's drag-reorder lives inside this
					// vector, so dropping it on size change (e.g. one entity
					// deleted) would silently undo their work. Set-diff keeps
					// the existing order intact and only mutates the deltas.
					//
					// New entities go to the END so freshly-created entities land
					// at the bottom of the hierarchy. entt's `view<entt::entity>`
					// iterates the dense storage in reverse-insertion order
					// (newest first), so we walk it backwards — that way a scene
					// load that creates N entities in sequence yields the same
					// order on screen as the order they were created/serialized.
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

					// DFS reorder so each child appears immediately after its
					// parent in the flat list — that lets the existing single-
					// loop renderer show parent-child nesting just by tracking
					// depth and calling ImGui::Indent. Roots come first in
					// the user's chosen m_EntityOrder; their subtrees follow
					// in the order each parent's HierarchyComponent::Children
					// lists them.
					//
					// Same pass also builds m_VisibleEntityOrder / m_VisibleEntityDepths
					// — the pruned subset that excludes descendants of any
					// collapsed parent. The render loop iterates that pruned
					// list, so a fully-folded scene with 100k entities drops
					// from O(N) iteration to O(visible roots) per frame.
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

						// `insideCollapsed` is true when any ancestor on the
						// current DFS path is folded. The node itself still
						// goes into the full m_EntityOrder (drag-reorder and
						// scene ops read that), but the visible-list push is
						// suppressed for the duration of the subtree.
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

				// Tighten vertical spacing between rows so the hierarchy reads as
				// a dense list (Unity-style) rather than spaced-out items. Only
				// the y-component of ItemSpacing changes; horizontal spacing for
				// SameLine calls inside the row stays at the default.
				const ImVec2 defaultItemSpacing = ImGui::GetStyle().ItemSpacing;
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(defaultItemSpacing.x, 1.0f));

				// Iterate the pruned visible list (built by the DFS above;
				// descendants of any collapsed parent are absent, so we never
				// touch them). ImGuiListClipper virtualizes the visible window
				// so a fully-expanded 100k-flat scene only iterates the rows
				// currently inside the scroll viewport. Per-row Indent/Unindent
				// (vs. the prior currentIndentDepth tracker) keeps indent state
				// independent of which rows the clipper happens to skip.
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_VisibleEntityOrder.size()));
				while (clipper.Step()) {
				for (int entityIdx = clipper.DisplayStart; entityIdx < clipper.DisplayEnd; ++entityIdx) {
					const EntityHandle entityHandle = m_VisibleEntityOrder[static_cast<std::size_t>(entityIdx)];
					if (!scene.IsValid(entityHandle)) continue;
					Entity entity = scene.GetEntity(entityHandle);
					const bool selected = IsEntitySelected(entityHandle);

					const int targetDepth = entityIdx < static_cast<int>(m_VisibleEntityDepths.size())
						? m_VisibleEntityDepths[static_cast<std::size_t>(entityIdx)] : 0;

					// Selection-index callers used to take an index into a
					// separately-accumulated "rendered rows" list; with the
					// iterated list now being that list, entityIdx fills the
					// same role.
					const int visibleIndex = entityIdx;

					// Per-row indent: balanced inside the loop body (Unindent
					// at the bottom), so clipping never strands an unbalanced
					// indent on the layout stack.
					const float rowIndent = static_cast<float>(targetDepth) * 14.0f;
					if (rowIndent > 0.0f) ImGui::Indent(rowIndent);

					ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entityHandle)));

					// Fold-out toggle. We use an InvisibleButton sized to the
					// text line height (rather than ImGui::ArrowButton which is
					// FrameHeight-tall and inflates each row) and then manually
					// draw the triangle on top — that way every row stays at one
					// line of text, like a tight tree view. Entities without
					// children still consume an equal-width spacer so labels
					// stay vertically aligned across siblings.
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

					// Prefab-instance tint: amber if the source GUID can't be resolved
					// (orphaned), accent-blue otherwise. Engine is flat today so all
					// prefab-origin entities get the same color — no root vs child
					// distinction is possible until parent/child components land.
					bool entityIsPrefabTinted = false;
					if (!entityIsDisabled && !Application::GetIsPlaying() && scene.GetEntityOrigin(entityHandle) == EntityOrigin::Prefab) {
						const uint64_t prefabGuid = static_cast<uint64_t>(scene.GetPrefabGUID(entityHandle));
						const bool resolvable = prefabGuid != 0 && !AssetRegistry::ResolvePath(prefabGuid).empty();
						ImGui::PushStyleColor(ImGuiCol_Text,
							resolvable ? EditorTheme::Colors::PrefabInstance : EditorTheme::Colors::PrefabOrphan);
						entityIsPrefabTinted = true;
					}

					// Cut clipboard marker: dim over any other tint to signal
					// "pending move". Cleared by Paste (destroys originals)
					// or Esc / new Cut (clears the marker only). Pushed last
					// so it wins over disabled/prefab colors; popped first.
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
							// Inspector header reads the cached FirstName — without
							// bumping the version, the name field keeps showing the
							// pre-rename value until something else changes the
							// selection.
							++m_SelectionVersion;
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
						// Rounded selection / hover fill. ImGui::Selectable hardcodes
						// `RenderFrame(..., false, 0.0f)` (no rounding) for its highlight,
						// so we suppress its square fill (transparent Header colors) and
						// repaint a rounded one ourselves on a separate draw channel that
						// lands BEHIND the text. Without ChannelsSplit the rect would
						// cover the label since AddRectFilled draws on top of prior
						// geometry in the same channel.
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

					// Drag-drop source: drag entity to reorder or to asset browser for prefab.
					// The payload only carries the primary (clicked) entity; the
					// hierarchy drop sites resolve the full multi-selection at
					// drop time via ResolveDraggedHierarchyEntities so external
					// drop targets that expect a single entity (PropertyDrawer,
					// AssetBrowser→prefab) keep working unchanged.
					// SourceSceneId lets a multi-scene drop target tell whether
					// the gesture is a same-scene reorder or a cross-scene
					// migration (the latter has to serialize + deserialize the
					// entity into the target scene rather than reparent it).
					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
						HierarchyDragData dragData{
							entityIdx,
							static_cast<uint32_t>(entityHandle),
							static_cast<uint64_t>(scene.GetSceneId()),
						};
						ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragData, sizeof(dragData));
						const bool draggingMulti = IsEntitySelected(entityHandle) && m_SelectedEntities.size() > 1;
						if (draggingMulti) {
							ImGui::Text("Move: %zu entities", m_SelectedEntities.size());
						} else {
							ImGui::Text("Move: %s", entity.GetName().c_str());
						}
						ImGui::EndDragDropSource();
					}

					// Capture the row rect before opening the drag-drop target so
					// we can split it into three zones: top quarter inserts as
					// previous sibling, bottom quarter inserts as next sibling,
					// and the middle reparents (the original behavior). This
					// lets the user rearrange siblings without leaving the
					// drag gesture, similar to Unity / Godot.
					const ImVec2 itemRectMin = ImGui::GetItemRectMin();
					const ImVec2 itemRectMax = ImGui::GetItemRectMax();
					const float itemHeight = itemRectMax.y - itemRectMin.y;
					const float zoneHeight = itemHeight * 0.25f;

					// In prefab-edit mode, suppress the top/bottom sibling-
					// insert zones on the prefab root entity. A sibling-of-
					// root drop would create a second root (which the
					// SetEntityParentPreservingWorld gate refuses anyway),
					// but without disabling the zones the drop indicator
					// would still flash and mislead the user into thinking
					// the gesture worked. Center-zone reparent stays live.
					const bool suppressSiblingZones = IsInPrefabEditMode()
						&& entityHandle == m_PrefabEditRootEntity;

					if (ImGui::BeginDragDropTarget()) {
						const float mouseY = ImGui::GetMousePos().y;
						const bool inTopZone = !suppressSiblingZones && mouseY < itemRectMin.y + zoneHeight;
						const bool inBottomZone = !suppressSiblingZones && mouseY > itemRectMax.y - zoneHeight;

						// Insertion-point indicator. Drawn while a hierarchy drag
						// is hovering the row — a thin colored line at the top
						// or bottom edge tells the user which sibling slot they
						// will land in.
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
							// Prefab edit mode is a detached single-scene workspace.
							// Allowing a cross-scene migration into it would import
							// foreign entities (and their PrefabGUID/origin metadata)
							// into the asset, breaking the "this prefab owns exactly
							// these entities" invariant at save time. Reject the
							// gesture rather than silently corrupting the asset.
							if (!sameScene && IsInPrefabEditMode()) {
								// drop ignored
							}
							else if (!sameScene) {
								// Cross-scene drop on an entity row: migrate the
								// dragged entities into `scene`, then either reparent
								// under `entity` (middle zone) or sibling-insert next
								// to `entity` (top/bottom zone). Migration happens
								// first so the sibling-insert helpers can operate on
								// handles that already live in the target scene.
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
										// MoveSiblingNextTo's root-level repositionVec
										// only finds entities already tracked in
										// m_EntityOrder. Fresh-from-migrate handles
										// aren't in that list until the next-frame
										// rebuild, so we splice them in at the end
										// here. Without this, sibling-insert silently
										// no-ops and the entity ends up appended at
										// the bottom of the next-frame rebuild
										// instead of next to the anchor the user
										// dropped onto.
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
								// Sibling-insert: anchor advances after each move so a
								// multi-drop produces a contiguous block in selection
								// order. Drop-after walks forward (each next entity
								// becomes the next sibling of the previous one);
								// drop-before walks the selection in reverse so the
								// final order at the target's slot is selection order.
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
								// Reparent: drop each onto `entity`. Skip drops that
								// SetParent would refuse anyway (cycle: target is a
								// descendant of dragged) so we don't claim a move
								// or unfold the target on a no-op.
								for (EntityHandle h : draggedHandles) {
									if (h == entityHandle) continue;
									Entity draggedEntity = scene.GetEntity(h);
									if (draggedEntity.IsAncestorOf(entity)) continue;
									if (SetEntityParentPreservingWorld(scene, h, entity)) {
										didMove = true;
									}
								}
								if (didMove) {
									// Unfold the target so the dropped children are visible
									// (a no-op when the target was already expanded).
									m_CollapsedHierarchyEntities.erase(static_cast<uint32_t>(entityHandle));
									// Force a re-DFS next frame: reparenting doesn't
									// change the registry's entity count, so the size-
									// mismatch heuristic in the renderer won't trip.
									// Without this flag the dragged entity stays
									// visually in its old subtree until the user
									// reloads the scene, even though the parent ref
									// has actually moved.
									m_EntityOrderDirty = true;
								}
							}
							if (didMove) {
								scene.MarkDirty();
							}
							}
						}
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
							// 1-arg constructor reads up to the null terminator — the
							// asset-browser source intentionally appends `\0` and sends
							// `size() + 1` bytes so this works. The 2-arg
							// (data, DataSize) form silently embeds the trailing null
							// in the string and breaks every downstream `==` against
							// a literal extension like ".cs" / ".prefab".
							std::string droppedPath(static_cast<const char*>(payload->Data));
							std::string ext = std::filesystem::path(droppedPath).extension().string();
							std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
							if (ext == ".prefab") {
								// Nested-prefab drop into prefab edit mode would
								// create a second root inside a single-rooted
								// prefab asset, and would also let a prefab be
								// dragged into itself (infinite recursion at the
								// next instantiation pass). Reject the gesture
								// while editing a prefab; script (.cs/.cpp) drops
								// continue to work because they attach to the
								// existing entity rather than spawn a new one.
								if (!IsInPrefabEditMode()) {
									EntityHandle loaded = SceneSerializer::LoadEntityFromFile(scene, droppedPath);
									if (loaded != entt::null) {
										EnsureEditorUniqueEntityNames(scene, { loaded });
									}
									m_EntityOrder.clear(); m_EntityOrderDirty = true;
								}
							}
							else {
								// Mirror the Add Component button drop: a .cs (managed
								// component / EntityScript / native-component .cs) or
								// .cpp (NativeScript) dropped onto an entity row attaches
								// it to THAT entity only — not the full selection — so
								// the gesture lines up with the row the user dropped on.
								// CollectScriptFile no-ops for non-script extensions, so
								// the same code naturally ignores audio / fonts / etc.
								// `droppedPath` already constructed above with the
								// 1-arg form, so the null terminator is correctly
								// stripped before reaching CollectScriptFile.
								std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
								EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
								bool scriptAttached = false;
								for (const auto& scriptEntry : droppedScripts) {
									if (scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
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

					// Pop the row text-tint pushes BEFORE opening the context
					// menu — popups inherit the live style stack at open time,
					// so without this the menu items render in prefab-blue /
					// disabled-gray / cut-gray instead of the default text
					// color. Everything that needed the tint (selectable,
					// rename, drag source/target) has already drawn above.
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

						if (!IsEntitySelected(entityHandle)) {
							SetSingleEntitySelection(entityHandle, visibleIndex);
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

		// Deferred scene reorder (after iteration). Held until now because
		// MoveLoadedScene mutates m_LoadedScenes — applying it mid-loop
		// would invalidate the iteration state and could re-render a
		// scene at its new position before its old slot finishes drawing.
		if (pendingSceneReorder) {
			SceneManager::Get().MoveLoadedScene(pendingSceneReorder->SourceName, pendingSceneReorder->TargetIndex);
			// Force a hierarchy rebuild — m_EntityOrder is shared across
			// every scene rendered by this panel, so reorder might
			// surface a scene whose order vector is partially stale.
			m_EntityOrderDirty = true;
		}

		// Drag-drop target: only present during an active drag so it doesn't block right-click menus
		if (ImGui::GetDragDropPayload() != nullptr) {
			ImVec2 avail = ImGui::GetContentRegionAvail();
			if (avail.y > 0) {
				ImGui::InvisibleButton("##SceneDropTarget", ImVec2(-1, avail.y));
				if (ImGui::BeginDragDropTarget()) {
					// Dropping an entity into the empty space at the bottom of
					// the panel detaches it (becomes a root again) AND moves
					// it to the end of m_EntityOrder so it shows up at the
					// bottom of the root list — matching the user's gesture.
					// Disabled in prefab-edit mode: prefabs are single-rooted,
					// so unparenting here would create a second root that gets
					// silently dropped at save time.
					if (!IsInPrefabEditMode())
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
						auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
						EntityHandle primaryDraggedHandle = static_cast<EntityHandle>(dragData->EntityHandle);
						Scene* dropScene = GetContextScene();
						if (dropScene) {
							const uint64_t dropSceneIdValue = static_cast<uint64_t>(dropScene->GetSceneId());
							const bool sameScene = dragData->SourceSceneId == dropSceneIdValue;
							if (!sameScene) {
								// Cross-scene drop on empty space: locate the
								// source scene by payload SceneId, migrate the
								// dragged entities into the context (last-
								// interacted) scene as roots.
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
								// Push every dragged root to the bottom of m_EntityOrder
								// in selection order — matches the user's "I dropped
								// these at the bottom" gesture even when one was
								// already a root.
								auto orderIt = std::find(m_EntityOrder.begin(), m_EntityOrder.end(), h);
								if (orderIt != m_EntityOrder.end() && std::next(orderIt) != m_EntityOrder.end()) {
									m_EntityOrder.erase(orderIt);
									m_EntityOrder.push_back(h);
									m_EntityOrderDirty = true; // M30: re-DFS next render.
								}
							}
							if (didMove) {
								// Reparent to root happened — force a re-DFS in
								// case the in-vector reposition above didn't run
								// (entity was already at the bottom slot).
								m_EntityOrderDirty = true;
								dropScene->MarkDirty();
							}
							}
						}
					}
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						// See the comment on the entity-row drop above for why this
						// is the 1-arg constructor.
						std::string droppedPath(static_cast<const char*>(payload->Data));
						std::string ext = std::filesystem::path(droppedPath).extension().string();
						std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
						// All scene-level asset drops below create NEW root entities
						// in the context scene. In prefab edit mode that violates the
						// single-root invariant of a prefab asset (and a nested .prefab
						// drop could recurse if the dropped asset is the one being
						// edited). Reject the gesture wholesale; the user can still
						// drop scripts onto an existing entity row to attach them.
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

		auto acceptDroppedGameSystems = [&]() {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				// 1-arg form: see entity-row drop comment. The 2-arg form embeds
				// the trailing `\0` and CollectScriptFile silently emits nothing
				// because `IsCSharpScriptExtension(".cs\0")` is false.
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsGameSystem) {
						scene.AddGameSystem(scriptEntry.ClassName);
					}
				}
			}
		};

		const auto& systems = scene.GetGameSystemClassNames();
		for (size_t i = 0; i < systems.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));
			const std::string& className = systems[i];

			// Reorder/remove buttons share the row with the collapsing header so
			// the layout matches the prior text-only design — header on the left,
			// trailing actions on the right via SameLine. AllowOverlap lets the
			// SmallButton hit-test even though it's drawn over the header.
			bool open = ImGui::CollapsingHeader((className + "##system_header").c_str(),
				ImGuiTreeNodeFlags_AllowOverlap);

			// Trailing control strip — right-aligned. All four widgets are
			// FrameHeight-tall (Checkbox, two ArrowButtons, Button) so the
			// row reads as a single uniform strip of square icons.
			const ImGuiStyle& style = ImGui::GetStyle();
			const float btnW = ImGui::GetFrameHeight();
			const float spacing = style.ItemInnerSpacing.x;
			const float stripW = btnW * 4.0f + spacing * 3.0f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - stripW);

			bool enabled = scene.IsGameSystemEnabled(className);
			if (ImGui::Checkbox("##enabled", &enabled)) {
				scene.SetGameSystemEnabled(className, enabled);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", enabled ? "Disable GameSystem" : "Enable GameSystem");
			}

			ImGui::SameLine(0, spacing);
			if (ImGui::ArrowButton("##move_up", ImGuiDir_Up) && i > 0) {
				scene.MoveGameSystem(i, i - 1);
			}

			ImGui::SameLine(0, spacing);
			if (ImGui::ArrowButton("##move_down", ImGuiDir_Down) && i + 1 < systems.size()) {
				scene.MoveGameSystem(i, i + 1);
			}

			ImGui::SameLine(0, spacing);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.35f, 0.35f, 1.0f));
			const bool remove = ImGui::Button("X", ImVec2(btnW, btnW));
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove GameSystem");
			if (remove) {
				scene.RemoveGameSystem(i);
				ImGui::PopID();
				break;
			}

			if (open) {
				ImGui::Indent(8.0f);
				DrawGameSystemFields(scene, className);
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
			if (entry.IsGameSystem && !scene.HasGameSystem(entry.ClassName)) {
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
			acceptDroppedGameSystems();
			ImGui::EndDragDropTarget();
		}
		if (!hasAvailableSystem) {
			ImGui::EndDisabled();
			// AllowWhenDisabled so hover still fires after BeginDisabled.
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				ImGui::SetTooltip("No game systems available.\nCreate one via Asset Browser > Create > Scripting > GameSystem (C#).");
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
				if (!scriptEntry.IsGameSystem || scene.HasGameSystem(scriptEntry.ClassName)) {
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
					scene.AddGameSystem(scriptEntry.ClassName);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

	}

	void ImGuiEditorLayer::RenderInspectorPanel(Scene& scene) {
		ImGui::Begin("Inspector");

		// Cache-driven selection materialization. The cache also stores the
		// common-component intersection + name/Enabled/Static uniformity so
		// the body below doesn't have to re-derive them every frame. Without
		// this, the inspector ran several O(N) and O(N×M) loops per frame
		// over (selection × components), which dominated frame time at
		// 25k+ selected entities. Version bumps live with the selection
		// mutators and with the inspector's own add/remove paths below.
		if (m_InspectorCache.Version != m_SelectionVersion) {
			RecomputeInspectorSelectionCache(scene);
			m_InspectorCache.Version = m_SelectionVersion;
		}
		const std::vector<EntityHandle>& selectedHandles = m_InspectorCache.Handles;
		if (selectedHandles.empty()) {
			m_SelectedEntity = entt::null;
			if (m_IsSceneNodeSelected) {
				RenderSceneSystemsInspector(scene);
				// GameSystem fields can include reference pickers (Texture,
				// Audio, Entity). The picker opens here, so it has to render
				// here too — the entity-selection branch's RenderPopup call
				// further down isn't reachable from this early return.
				ReferencePicker::RenderPopup();
			}
			else {
				RenderAssetInspector();
			}
			m_IsInspectorPanelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			ImGui::End();
			return;
		}

		// Stable Entity wrappers for the selection — reused for the component
		// intersection and passed to every multi-edit inspector below.
		std::vector<Entity> selectedEntities;
		selectedEntities.reserve(selectedHandles.size());
		for (EntityHandle h : selectedHandles) {
			selectedEntities.push_back(scene.GetEntity(h));
		}
		std::span<const Entity> entitySpan(selectedEntities);
		Entity entity = selectedEntities[0]; // primary, used for the drag-source label and prefab buttons

		// Keep m_SelectedEntity pointed at a valid handle — some callers
		// (prefab Apply / Revert, focus / duplicate / rename helpers) still
		// index it directly.
		if (!scene.IsValid(m_SelectedEntity)) {
			m_SelectedEntity = entity.GetHandle();
		}

		// ── Entity Header: Name + Toggles ──────────────────

		// Show selection count when multiple entities are selected, so the
		// user immediately sees how many they're editing. Single-selection
		// is the common case and doesn't need the label.
		if (selectedEntities.size() > 1) {
			ImGui::TextDisabled("(%zu)", selectedEntities.size());
		}

		// Editable Name. The buffer is pre-filled with the actual name when
		// the selection is uniform AND named — otherwise the buffer stays
		// empty and a hint signals the state to the user without becoming
		// committable text:
		//   • uniform, no NameComponent → buffer empty, hint "Entity"
		//   • uniform, named            → buffer holds the name, no hint
		//   • mixed selection           → buffer empty, hint "-"
		// This avoids the old bug where pressing Enter on an unnamed
		// entity (where the buffer was pre-filled with the literal text
		// "Entity") created a real NameComponent("Entity").
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

		// Toggles row: Enabled + Static. Tri-state when the underlying tag
		// presence differs across the selection. Editing applies to all.
		{
			constexpr int kMixedValueFlag = 1 << 12;

			// Inspector toggle reflects the AUTHORED enabled state, not
			// the effective in-hierarchy state. A child whose parent is
			// disabled has both DisabledTag and InheritedDisabledTag —
			// inherited-disabled doesn't flip the user's intent, so we
			// keep the checkbox on. The parent re-enable cascade then
			// restores the child's runtime DisabledTag accordingly.
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

		// Prefab override state for the inspector header / per-component badges.
		// Computed once per frame for the primary entity when the selection is
		// a single prefab instance. Multi-select + prefab overrides is out of
		// scope for v1 — when more than one entity is selected the UI behaves
		// as if no overrides exist (consistent with mixed-value semantics).
		Json::Value prefabOverrides = Json::Value::MakeObject();
		const bool isSinglePrefabInstance = selectedEntities.size() == 1
			&& scene.GetEntityOrigin(entity.GetHandle()) == EntityOrigin::Prefab
			&& static_cast<uint64_t>(scene.GetPrefabGUID(entity.GetHandle())) != 0;
		bool prefabSourceResolvable = false;
		// `prefabInstanceRoot` is the entity Apply All / Revert All operate
		// on — always the actual prefab root, even when the user has a
		// child of the instance selected. Without this resolution Apply All
		// would write the selected child as if it were the prefab root,
		// silently overwriting the prefab file with that subtree.
		EntityHandle prefabInstanceRoot = entt::null;
		// True iff ANY entity in the prefab instance subtree differs from
		// its source — drives the Apply/Revert button enable state. The
		// per-entity `prefabOverrides` map only captures THIS entity's diff,
		// so a child-only edit would leave the root's button disabled
		// without this subtree-wide check.
		bool hasSubtreeOverrides = false;
		if (isSinglePrefabInstance) {
			prefabSourceResolvable = SceneSerializer::ComputeInstanceOverrides(
				scene, entity.GetHandle(), prefabOverrides);
			if (prefabSourceResolvable) {
				prefabInstanceRoot = SceneSerializer::GetPrefabInstanceRoot(scene, entity.GetHandle());
				hasSubtreeOverrides = SceneSerializer::HasPrefabInstanceOverrides(scene, entity.GetHandle());
			}
		}

		// Top-of-inspector prefab actions (only when the source resolves; orphans
		// can't apply or revert because we have no source to diff against).
		if (isSinglePrefabInstance && prefabSourceResolvable && prefabInstanceRoot != entt::null) {
			// No overrides anywhere in the subtree == nothing to apply or
			// revert. Without this gate the buttons happily fire on a clean
			// instance: Apply All rewrites the source prefab to a
			// structurally-equivalent file (still triggers a disk write,
			// asset re-bake, and live-instance refresh pass on every other
			// open scene), and Revert All destroys + rebuilds the entity
			// for no semantic change — both visible as "the editor did
			// something for no reason" to the user.
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.66f, 0.95f, 1.0f));
			ImGui::TextUnformatted(entity.GetHandle() == prefabInstanceRoot
				? "Prefab Instance"
				: "Prefab Instance (child)");
			ImGui::PopStyleColor();
			ImGui::BeginDisabled(!hasSubtreeOverrides);
			if (ImGui::SmallButton("Apply All")) {
				// Apply / Revert always act on the prefab ROOT, never the
				// currently-selected entity — applying a child as if it were
				// the root would overwrite the prefab file with that child's
				// serialized tree and silently delete the prefab's other
				// branches.
				//
				// Capture old source BEFORE the apply, then push instance
				// state to disk and propagate to other live instances
				// preserving their overrides.
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
						// Only the ROOT carries PrefabInstanceComponent — gating
						// the iteration on it skips child entities that also
						// have Origin=Prefab + matching PrefabGUID but would be
						// refreshed transitively when their root rebuilds.
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
				// RevertPrefabInstanceOverride destroys the current root and
				// returns a freshly-built replacement — every cached handle
				// derived from `entity` (selectedEntities, entitySpan, the
				// prefab-edit root) is stale after this point. Capture the
				// pre-revert root, dispatch the revert, then patch every
				// editor-side reference and bail out of the inspector body
				// this frame so we don't run the component loop against
				// destroyed memory (which manifested as the editor's UI
				// freezing to the clear colour after one click).
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

		// H24: bucket override paths by component-serialized-name once per
		// frame so the per-component check is O(1). Without this, the
		// `componentHasOverrides` lambda below scanned every override path
		// for every component on every frame the inspector was open
		// (O(components * paths) per render).
		//
		// Override paths follow the convention "<componentSerializedName>"
		// or "<componentSerializedName>.<field…>"; the leading token before
		// the first '.' is the component bucket.
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

		// Common-component intersection + partial list both come from the
		// inspector cache (recomputed once per selection mutation). The old
		// path ran an O(N) info.has() sweep per component per frame; with
		// 25k entities × ~50 component types that was 1.25M+ checks per
		// frame just to decide what to draw. ForEach below still iterates
		// the registry in its native (stable) order so the section order
		// the user sees doesn't depend on hash bucket layout.
		const std::vector<std::string>& hiddenPartialComponents = m_InspectorCache.PartialComponents;

		// A prefab "Revert" destroys + recreates the entity and refreshes the
		// selection (m_SelectedEntities), but the local `entity` / `entitySpan`
		// captured below still point at the OLD (now-destroyed) handle. Because
		// ForEachComponentInfo invokes this lambda once PER component, any
		// component drawn after the reverted one would re-enter with that stale
		// handle — a use-after-destroy against the EnTT registry. This latch
		// makes every remaining invocation this frame a no-op; the next frame
		// re-renders cleanly against the refreshed selection.
		bool selectionInvalidatedByRevert = false;
		registry.ForEachComponentInfo([&](const std::type_index& typeId, const ComponentInfo& info) {
			if (selectionInvalidatedByRevert) return;
			if (info.category != ComponentCategory::Component) return;
			if (info.displayName == "Name") return; // Shown in entity header

			// Cache gate: this set holds exactly the types present on every
			// selected entity. O(1) lookup replaces the per-frame O(N)
			// info.has() sweep. PartialComponents was populated alongside
			// CommonComponentTypes during the cache pass.
			if (m_InspectorCache.CommonComponentTypes.find(typeId)
				== m_InspectorCache.CommonComponentTypes.end()) {
				return;
			}

			// Scripts render their own per-script sections — skip the outer wrapper
			if (info.displayName == "Scripts") {
				DispatchComponentInspector(info, entitySpan);
				return;
			}

			// Dynamic components (DynamicComponentRegistrar / RegisterDynamic)
			// have no drawInspector and no PropertyDescriptors, so the outer
			// wrapper would render an empty section. Their fields surface via
			// the paired ScriptComponent.Scripts entry rendered above by the
			// "Scripts" case — that path is the single source of truth.
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

			// Override badge — colored dot drawn next to the section header to
			// surface the diff at a glance. Sourced from EditorTheme so future
			// theme changes don't require touching the inspector.
			if (thisComponentOverridden) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Colors::OverrideMarker);
				ImGui::TextUnformatted(" *");
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Component has per-field overrides relative to the source prefab.");
				}
			}

			// Component drag source on the header. References the primary
			// entity — useful when dragging onto a script field, since that
			// field would only be able to hold one component reference anyway.
			//
			// We embed the persistent UUID (not RuntimeID): if the user drops
			// onto a script field the value is saved to disk, and RuntimeIDs
			// are reallocated on every scene reload. The drop side parses
			// this string and writes it directly into the field, so the value
			// must survive save/load.
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
			// Copy serializes the primary entity. Paste / Reset apply to ALL
			// selected entities — that matches the rest of the multi-edit
			// behavior (each entity that lacks the component gets it added).
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

			// Prefab override operations — single-instance only (multi-select
			// composition is intentionally deferred). Revert paths destroy and
			// re-create the entity, so the selection handle is refreshed below.
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
				// Revert every override path under this component by walking the
				// captured overrides set. Each revert call destroys + recreates,
				// so we iterate paths first and only refresh the selection once.
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
				// "Apply Component" pushes the whole instance state to source
				// (no per-component partial save in v1) then propagates with
				// override-preservation, matching Apply All. The granularity
				// difference vs Apply All is purely UX framing.
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

		// Render the unified reference-picker popup once per inspector frame,
		// so any native component's PropertyDrawer-driven asset/entity picker
		// surfaces. The script inspector renders it inside its own draw too,
		// for the case where the panel is open without other components.
		ReferencePicker::RenderPopup();

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		float buttonWidth = ImGui::GetContentRegionAvail().x;
		if (Icons::ButtonWithIcon(Icons::Type::Plus, "Add Component", ImVec2(buttonWidth, 0), true)) {
			ImGui::OpenPopup("AddComponentPopup");
			m_ComponentSearchBuffer[0] = '\0';
		}

		// Drag-drop target on the Add Component button itself — accept .cs
		// script files dropped from the asset browser and attach them to
		// every selected entity. MUST sit immediately after the button so
		// ImGui's global LastItemData still points at the button; placing
		// it after RenderAddComponentPopup binds to whatever the popup
		// rendered last instead, and the drop silently no-ops.
		bool scriptDroppedSomething = false;
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				// 1-arg form: see entity-row drop comment above.
				std::string droppedPath(static_cast<const char*>(payload->Data));
				std::vector<EditorScriptDiscovery::ScriptEntry> droppedScripts;
				EditorScriptDiscovery::CollectScriptFile(std::filesystem::path(droppedPath), droppedScripts);
				for (const auto& scriptEntry : droppedScripts) {
					if (scriptEntry.IsGameSystem || scriptEntry.IsGlobalSystem) {
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

		// Categorized + searchable Add Component popup. Shared with the
		// asset-side prefab inspector so the UX matches whether the user
		// is editing a regular entity or a `.prefab` directly. The helper
		// hides components present on every selected entity, applies adds
		// to every entity missing the component, and runs conflict checks
		// against the selection so invariant-violating combinations are
		// disabled with a tooltip explaining why.
		bool addComponentChanged = false;
		RenderAddComponentPopup("AddComponentPopup", scene, entitySpan,
			m_ComponentSearchBuffer, sizeof(m_ComponentSearchBuffer),
			&addComponentChanged);
		if (addComponentChanged || scriptDroppedSomething) {
			// New component(s) were just added to one or more selected entities
			// — invalidate the inspector cache so the next frame rebuilds the
			// common-component intersection and surfaces the new section.
			++m_SelectionVersion;
		}

		// Mark the scene dirty only when ImGui reports a real value change on
		// the active widget this frame. IsAnyItemActive() would fire on mere
		// focus/click; ActiveIdHasBeenEditedThisFrame fires only on edits
		// (drag step, keystroke, etc.). Scope to this panel's focus stack.
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
