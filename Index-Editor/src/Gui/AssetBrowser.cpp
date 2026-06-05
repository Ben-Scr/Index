#include <pch.hpp>
#include "Assets/AssetRegistry.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Serialization/Json.hpp"
#include "Gui/AssetBrowser.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Serialization/Path.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"
#include "Components/General/NameComponent.hpp"
#include "Core/Log.hpp"
#include "Editor/EditorPreferences.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Gui/EditorTheme.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Gui/SpriteSliceDragPayload.hpp"
#include "Gui/ThumbnailCache.hpp"
#include "Graphics/Texture2D.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <algorithm>
#include "Gui/EditorIcons.hpp"
#include "Gui/Icons.hpp"
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <thread>

#ifdef IDX_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj_core.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ole32.lib")
#endif
#endif

namespace Index {
	namespace {
		bool IsLeftMouseDragPastClickThreshold()
		{
			const ImGuiIO& io = ImGui::GetIO();
			const ImVec2 delta(io.MousePos.x - io.MouseClickedPos[ImGuiMouseButton_Left].x,
				io.MousePos.y - io.MouseClickedPos[ImGuiMouseButton_Left].y);
			return (delta.x * delta.x + delta.y * delta.y) > (io.MouseDragThreshold * io.MouseDragThreshold);
		}

		std::string GetDuplicateBaseName(const std::string& stem)
		{
			if (stem.size() >= 4 && stem.back() == ')') {
				const std::size_t open = stem.rfind(" (");
				if (open != std::string::npos && open + 2 < stem.size() - 1) {
					bool allDigits = true;
					for (std::size_t i = open + 2; i < stem.size() - 1; ++i) {
						if (!std::isdigit(static_cast<unsigned char>(stem[i]))) {
							allDigits = false;
							break;
						}
					}
					if (allDigits) {
						return stem.substr(0, open);
					}
				}
			}

			const auto stripSeparatorNumber = [&](char separator) -> std::string {
				if (stem.size() < 3 || !std::isdigit(static_cast<unsigned char>(stem.back()))) {
					return {};
				}
				std::size_t firstDigit = stem.size() - 1;
				while (firstDigit > 0 && std::isdigit(static_cast<unsigned char>(stem[firstDigit - 1]))) {
					--firstDigit;
				}
				if (firstDigit > 0 && stem[firstDigit - 1] == separator) {
					return stem.substr(0, firstDigit - 1);
				}
				return {};
			};

			for (char separator : { ' ', '-', '_' }) {
				std::string stripped = stripSeparatorNumber(separator);
				if (!stripped.empty()) {
					return stripped;
				}
			}

			return stem;
		}

		IndexProject::EditorEntityNameSuffixStyle GetAssetDuplicateSuffixStyle()
		{
			if (IndexProject* project = ProjectManager::GetCurrentProject()) {
				return project->EditorAssetDuplicateSuffix;
			}
			return IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber;
		}

		std::string FormatDuplicateAssetName(
			const std::string& baseName,
			int index,
			IndexProject::EditorEntityNameSuffixStyle style)
		{
			switch (style) {
			case IndexProject::EditorEntityNameSuffixStyle::SpaceNumber:
				return baseName + " " + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::HyphenNumber:
				return baseName + "-" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::UnderscoreNumber:
				return baseName + "_" + std::to_string(index);
			case IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber:
			default:
				return baseName + " (" + std::to_string(index) + ")";
			}
		}

		std::filesystem::path MakeUniqueAssetPath(
			const std::filesystem::path& source,
			const std::filesystem::path& destinationDirectory,
			bool preserveOriginalNameWhenFree)
		{
			std::error_code ec;
			std::filesystem::path candidate = destinationDirectory / source.filename();
			if (preserveOriginalNameWhenFree && !std::filesystem::exists(candidate, ec)) {
				return candidate;
			}

			const std::string stem = GetDuplicateBaseName(source.stem().string());
			const std::string extension = source.extension().string();
			const auto suffixStyle = GetAssetDuplicateSuffixStyle();
			for (int counter = 1; counter < 10000; ++counter) {
				candidate = destinationDirectory / (FormatDuplicateAssetName(stem, counter, suffixStyle) + extension);
				ec.clear();
				if (!std::filesystem::exists(candidate, ec)) {
					return candidate;
				}
			}

			return destinationDirectory / (FormatDuplicateAssetName(stem, static_cast<int>(std::time(nullptr)), suffixStyle) + extension);
		}

		bool CopyEntryTo(const std::filesystem::path& source, const std::filesystem::path& destination)
		{
			std::error_code ec;
			if (!std::filesystem::exists(source, ec) || ec) {
				return false;
			}

			if (destination.has_parent_path()) {
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) {
					return false;
				}
			}

			if (std::filesystem::is_directory(source, ec) && !ec) {
				std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive, ec);
				return !ec;
			}

			ec.clear();
			std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
			return !ec;
		}

		bool MoveEntryTo(const std::filesystem::path& source, const std::filesystem::path& destination)
		{
			std::error_code ec;
			if (!std::filesystem::exists(source, ec) || ec) {
				return false;
			}

			if (destination.has_parent_path()) {
				std::filesystem::create_directories(destination.parent_path(), ec);
				if (ec) {
					return false;
				}
			}

			std::filesystem::rename(source, destination, ec);
			if (!ec) {
				if (std::filesystem::is_regular_file(destination, ec)) {
					AssetRegistry::MoveCompanionMetadata(source.string(), destination.string());
				}
				return true;
			}

			ec.clear();
			if (!CopyEntryTo(source, destination)) {
				return false;
			}

			Directory::Delete(source.string());
			return true;
		}

#ifdef IDX_PLATFORM_WINDOWS
		FORMATETC MakeHGlobalFormat(CLIPFORMAT format)
		{
			FORMATETC result{};
			result.cfFormat = format;
			result.dwAspect = DVASPECT_CONTENT;
			result.lindex = -1;
			result.tymed = TYMED_HGLOBAL;
			return result;
		}

		HGLOBAL CreateHDropMemory(const std::vector<std::wstring>& paths)
		{
			if (paths.empty()) {
				return nullptr;
			}

			std::size_t charCount = 1; // final double-null terminator
			for (const std::wstring& path : paths) {
				charCount += path.size() + 1;
			}

			const SIZE_T byteCount = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteCount);
			if (!memory) {
				return nullptr;
			}

			auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(memory));
			if (!dropFiles) {
				GlobalFree(memory);
				return nullptr;
			}

			dropFiles->pFiles = sizeof(DROPFILES);
			dropFiles->fWide = TRUE;

			wchar_t* write = reinterpret_cast<wchar_t*>(
				reinterpret_cast<std::uint8_t*>(dropFiles) + sizeof(DROPFILES));
			for (const std::wstring& path : paths) {
				std::memcpy(write, path.c_str(), path.size() * sizeof(wchar_t));
				write += path.size();
				*write++ = L'\0';
			}
			*write = L'\0';

			GlobalUnlock(memory);
			return memory;
		}

		HGLOBAL CreateDropEffectMemory(DWORD effect)
		{
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
			if (!memory) {
				return nullptr;
			}

			if (auto* value = static_cast<DWORD*>(GlobalLock(memory))) {
				*value = effect;
				GlobalUnlock(memory);
				return memory;
			}

			GlobalFree(memory);
			return nullptr;
		}

		class FormatEtcEnumerator final : public IEnumFORMATETC {
		public:
			explicit FormatEtcEnumerator(std::vector<FORMATETC> formats)
				: m_Formats(std::move(formats))
			{
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (!out) return E_POINTER;
				if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
					*out = static_cast<IEnumFORMATETC*>(this);
					AddRef();
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return ++m_RefCount; }

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG count = --m_RefCount;
				if (count == 0) {
					delete this;
				}
				return count;
			}

			HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* outFormats, ULONG* outFetched) override
			{
				if (!outFormats || (count > 1 && !outFetched)) {
					return E_POINTER;
				}

				ULONG fetched = 0;
				while (fetched < count && m_Index < m_Formats.size()) {
					outFormats[fetched++] = m_Formats[m_Index++];
				}

				if (outFetched) {
					*outFetched = fetched;
				}
				return fetched == count ? S_OK : S_FALSE;
			}

			HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
			{
				m_Index = std::min(m_Index + static_cast<std::size_t>(count), m_Formats.size());
				return m_Index < m_Formats.size() ? S_OK : S_FALSE;
			}

			HRESULT STDMETHODCALLTYPE Reset() override
			{
				m_Index = 0;
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override
			{
				if (!out) return E_POINTER;
				auto* clone = new FormatEtcEnumerator(m_Formats);
				clone->m_Index = m_Index;
				*out = clone;
				return S_OK;
			}

		private:
			ULONG m_RefCount = 1;
			std::vector<FORMATETC> m_Formats;
			std::size_t m_Index = 0;
		};

		class FileDropDataObject final : public IDataObject {
		public:
			explicit FileDropDataObject(std::vector<std::wstring> paths)
				: m_Paths(std::move(paths))
			{
				m_PreferredDropEffectFormat = static_cast<CLIPFORMAT>(
					RegisterClipboardFormatW(L"Preferred DropEffect"));
				m_Formats.push_back(MakeHGlobalFormat(static_cast<CLIPFORMAT>(CF_HDROP)));
				if (m_PreferredDropEffectFormat != 0) {
					m_Formats.push_back(MakeHGlobalFormat(m_PreferredDropEffectFormat));
				}
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (!out) return E_POINTER;
				if (riid == IID_IUnknown || riid == IID_IDataObject) {
					*out = static_cast<IDataObject*>(this);
					AddRef();
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return ++m_RefCount; }

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG count = --m_RefCount;
				if (count == 0) {
					delete this;
				}
				return count;
			}

			HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
			{
				if (!format || !medium) return E_POINTER;
				std::memset(medium, 0, sizeof(STGMEDIUM));

				if (Matches(format, static_cast<CLIPFORMAT>(CF_HDROP))) {
					HGLOBAL hdrop = CreateHDropMemory(m_Paths);
					if (!hdrop) return E_OUTOFMEMORY;
					medium->tymed = TYMED_HGLOBAL;
					medium->hGlobal = hdrop;
					return S_OK;
				}

				if (m_PreferredDropEffectFormat != 0
					&& Matches(format, m_PreferredDropEffectFormat)) {
					HGLOBAL effect = CreateDropEffectMemory(DROPEFFECT_COPY);
					if (!effect) return E_OUTOFMEMORY;
					medium->tymed = TYMED_HGLOBAL;
					medium->hGlobal = effect;
					return S_OK;
				}

				return DV_E_FORMATETC;
			}

			HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

			HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
			{
				if (!format) return E_POINTER;
				if (Matches(format, static_cast<CLIPFORMAT>(CF_HDROP))) return S_OK;
				if (m_PreferredDropEffectFormat != 0 && Matches(format, m_PreferredDropEffectFormat)) return S_OK;
				return DV_E_FORMATETC;
			}

			HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
			{
				if (out) out->ptd = nullptr;
				return E_NOTIMPL;
			}

			HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

			HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
			{
				if (!out) return E_POINTER;
				if (direction != DATADIR_GET) {
					*out = nullptr;
					return E_NOTIMPL;
				}
				*out = new FormatEtcEnumerator(m_Formats);
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
			{
				return OLE_E_ADVISENOTSUPPORTED;
			}

			HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }

			HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
			{
				return OLE_E_ADVISENOTSUPPORTED;
			}

		private:
			static bool Matches(const FORMATETC* format, CLIPFORMAT expected)
			{
				return format->cfFormat == expected
					&& format->dwAspect == DVASPECT_CONTENT
					&& (format->tymed & TYMED_HGLOBAL) != 0;
			}

			ULONG m_RefCount = 1;
			std::vector<std::wstring> m_Paths;
			CLIPFORMAT m_PreferredDropEffectFormat = 0;
			std::vector<FORMATETC> m_Formats;
		};

		class FileDropSource final : public IDropSource {
		public:
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (!out) return E_POINTER;
				if (riid == IID_IUnknown || riid == IID_IDropSource) {
					*out = static_cast<IDropSource*>(this);
					AddRef();
					return S_OK;
				}
				*out = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override { return ++m_RefCount; }

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG count = --m_RefCount;
				if (count == 0) {
					delete this;
				}
				return count;
			}

			HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
			{
				if (escapePressed) return DRAGDROP_S_CANCEL;
				if ((keyState & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
			{
				return DRAGDROP_S_USEDEFAULTCURSORS;
			}

		private:
			ULONG m_RefCount = 1;
		};

		bool IsCursorInsideAnyEditorWindow()
		{
			POINT point{};
			if (!GetCursorPos(&point)) {
				return true;
			}

			HWND hoveredWindow = WindowFromPoint(point);
			HWND hoveredRoot = hoveredWindow ? GetAncestor(hoveredWindow, GA_ROOT) : nullptr;
			bool foundEditorWindow = false;

			ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
			for (ImGuiViewport* viewport : platformIo.Viewports) {
				if (!viewport) continue;
				HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
				if (!hwnd || !IsWindow(hwnd)) continue;
				foundEditorWindow = true;

				HWND editorRoot = GetAncestor(hwnd, GA_ROOT);
				if (hoveredWindow == hwnd || hoveredRoot == hwnd || (editorRoot && hoveredRoot == editorRoot)) {
					return true;
				}
			}

			if (foundEditorWindow && hoveredWindow) {
				return false;
			}

			for (ImGuiViewport* viewport : platformIo.Viewports) {
				if (!viewport) continue;
				HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
				if (!hwnd || !IsWindow(hwnd)) continue;

				RECT rect{};
				if (GetWindowRect(hwnd, &rect) && PtInRect(&rect, point)) {
					return true;
				}
			}

			const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			if (mainViewport) {
				RECT rect{
					static_cast<LONG>(mainViewport->Pos.x),
					static_cast<LONG>(mainViewport->Pos.y),
					static_cast<LONG>(mainViewport->Pos.x + mainViewport->Size.x),
					static_cast<LONG>(mainViewport->Pos.y + mainViewport->Size.y)
				};
				if (PtInRect(&rect, point)) {
					return true;
				}
			}

			return false;
		}

		HWND GetCurrentEditorViewportHwnd()
		{
			if (ImGuiViewport* viewport = ImGui::GetWindowViewport()) {
				HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
				if (hwnd && IsWindow(hwnd)) {
					return hwnd;
				}
			}

			if (ImGuiViewport* viewport = ImGui::GetMainViewport()) {
				HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
				if (hwnd && IsWindow(hwnd)) {
					return hwnd;
				}
			}

			HWND hwnd = ::GetActiveWindow();
			return hwnd && IsWindow(hwnd) ? hwnd : nullptr;
		}

		bool IsScreenDragPastThreshold(int startX, int startY)
		{
			POINT point{};
			if (!GetCursorPos(&point)) {
				return IsLeftMouseDragPastClickThreshold();
			}

			const float threshold = ImGui::GetIO().MouseDragThreshold;
			const float dx = static_cast<float>(point.x - startX);
			const float dy = static_cast<float>(point.y - startY);
			return (dx * dx + dy * dy) > (threshold * threshold);
		}

		bool StartNativeFileDrag(const std::vector<std::string>& paths)
		{
			std::vector<std::wstring> widePaths;
			widePaths.reserve(paths.size());
			for (const std::string& pathString : paths) {
				std::filesystem::path path(pathString);
				std::error_code ec;
				if (!std::filesystem::exists(path, ec) || ec) {
					continue;
				}

				std::filesystem::path absolute = std::filesystem::weakly_canonical(path, ec);
				if (ec) {
					ec.clear();
					absolute = std::filesystem::absolute(path, ec);
				}
				if (ec) {
					absolute = path;
				}
				widePaths.push_back(absolute.wstring());
			}

			if (widePaths.empty()) {
				IDX_CORE_WARN_TAG("AssetBrowser", "Native file drag skipped because none of the selected paths exist.");
				return false;
			}

			const HRESULT oleResult = OleInitialize(nullptr);
			if (FAILED(oleResult)) {
				IDX_CORE_WARN_TAG("AssetBrowser", "OleInitialize failed for native file drag: HRESULT 0x{:08X}",
					static_cast<unsigned int>(oleResult));
				return false;
			}

			auto* dataObject = new FileDropDataObject(std::move(widePaths));
			auto* dropSource = new FileDropSource();
			DWORD effect = 0;
			const HRESULT dragResult = DoDragDrop(
				dataObject,
				dropSource,
				DROPEFFECT_COPY,
				&effect);
			dropSource->Release();
			dataObject->Release();
			OleUninitialize();

			const bool dropped = dragResult == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY) != 0;
			if (FAILED(dragResult)) {
				IDX_CORE_WARN_TAG("AssetBrowser", "DoDragDrop failed for native file drag: HRESULT 0x{:08X}",
					static_cast<unsigned int>(dragResult));
			}
			return dropped;
		}
#endif
	}

	static const char* GetFileTypeIconName(const std::string& extension) {
		std::string ext = extension;
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext == ".cs")                                                    return "file_cs";
		if (ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp")    return "file_fallback";
		if (ext == ".scene" || ext == ".index")                               return "file_scene";
		if (ext == ".prefab")                                                return "file_prefab";
		if (ext == ".dataasset")                                             return "file_dataasset";
		if (ext == ".anim")                                                  return "file_anim";
		if (ext == ".shader")                                                return "file_shader";
		if (ext == ".json")                                                  return "file_json";
		if (ext == ".xml")                                                   return "file_xml";
		if (ext == ".bin")                                                   return "file_bin";
		if (ext == ".zip")                                                   return "folder_zip";
		if (ext == ".txt" || ext == ".cfg" || ext == ".ini" ||
			ext == ".yaml" || ext == ".toml" || ext == ".lua")               return "file_txt";
		if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") return "file_audio";
		if (ext == ".ttf" || ext == ".otf")                                  return "file_font";

		return nullptr;
	}

	void AssetBrowser::Initialize(const std::string& rootDirectory) {
		m_RootDirectory = rootDirectory;
		m_CurrentDirectory = rootDirectory;
		m_Thumbnails.Initialize();
		m_NeedsRefresh = true;
	}

#ifdef IDX_PLATFORM_WINDOWS
	// M29: defined in AssetBrowserActions.cpp. Joins any in-flight
	// ShellExecuteW worker threads so they don't outlive editor shutdown.
	extern void JoinAllShellLaunchThreads();
#endif

	void AssetBrowser::Shutdown() {
#ifdef IDX_PLATFORM_WINDOWS
		JoinAllShellLaunchThreads();
#endif
		m_Thumbnails.Shutdown();
	}

	void AssetBrowser::NavigateTo(const std::string& directory) {
		m_CurrentDirectory = directory;
		ClearAssetSelection();
		CancelRename();
		m_NeedsRefresh = true;
	}

	void AssetBrowser::NavigateUp() {
		std::filesystem::path current(m_CurrentDirectory);
		std::filesystem::path root(m_RootDirectory);

		if (current != root && current.has_parent_path()) {
			std::filesystem::path parent = current.parent_path();
			if (parent.string().size() >= root.string().size()) {
				NavigateTo(parent.string());
			}
		}
	}

	void AssetBrowser::Refresh() {
		AssetRegistry::MarkDirty();
		AssetRegistry::Sync();
		m_Entries = Directory::GetEntries(m_CurrentDirectory);
		RebuildSliceCache();
		m_NeedsRefresh = false;
	}

	void AssetBrowser::RebuildSliceCache() {
		m_SliceCache.clear();
		for (const DirectoryEntry& entry : m_Entries) {
			if (entry.IsDirectory) continue;
			const AssetType type = ThumbnailCache::GetAssetType(
				std::filesystem::path(entry.Path).extension().string());
			if (type != AssetType::Image) continue;

			TextureMeta meta = AssetRegistry::ReadTextureMeta(entry.Path);
			if (meta.Sprites.empty()) continue;
			m_SliceCache.emplace(entry.Path, std::move(meta.Sprites));
		}
	}

	void AssetBrowser::ClearAssetSelection() {
		m_SelectedPath.clear();
		m_SelectedPaths.clear();
		m_LastSelectionIndex = -1;
		m_PressedPath.clear();
#ifdef IDX_PLATFORM_WINDOWS
		ClearExternalFileDrag();
#endif
		m_SelectionActivated = false;
		CancelRename();
	}

	bool AssetBrowser::IsPathSelected(const std::string& path) const {
		if (m_SelectedPaths.empty()) {
			return m_SelectedPath == path;
		}
		return std::find(m_SelectedPaths.begin(), m_SelectedPaths.end(), path) != m_SelectedPaths.end();
	}

	bool AssetBrowser::IsPathInCutClipboard(const std::string& path) const {
		if (!m_AssetClipboardCut) return false;
		return std::find(m_AssetClipboardPaths.begin(), m_AssetClipboardPaths.end(), path) != m_AssetClipboardPaths.end();
	}

	bool AssetBrowser::TryCreatePrefabFromHierarchyDrop(const ImGuiPayload* payload, const std::string& targetDirectory) {
		if (!payload || payload->DataSize != sizeof(HierarchyDragData)) {
			return false;
		}

		// Hierarchy panel drag payload — see Gui/HierarchyDragData.hpp.
		const auto* dragData = static_cast<const HierarchyDragData*>(payload->Data);
		const entt::entity entityHandle = static_cast<entt::entity>(dragData->EntityHandle);

		// Resolve via SourceSceneId so cross-scene drags find the right registry; falls back to the active scene for older producers.
		Scene* scene = nullptr;
		if (dragData->SourceSceneId != 0) {
			SceneManager::Get().ForeachLoadedScene([&](Scene& s) {
				if (!scene && static_cast<uint64_t>(s.GetSceneId()) == dragData->SourceSceneId) {
					scene = &s;
				}
			});
		}
		if (!scene) scene = SceneManager::Get().GetActiveScene();
		if (!scene || !scene->IsValid(entityHandle)) {
			return false;
		}

		std::string entityName = "Entity";
		if (scene->HasComponent<NameComponent>(entityHandle)) {
			const std::string& sourceName = scene->GetComponent<NameComponent>(entityHandle).Name;
			if (!sourceName.empty()) entityName = sourceName;
		}

		// Append " (N)" until the path is free — never silently overwrite an
		// existing prefab. Cap at 10,000 to keep us out of an infinite loop
		// if the directory state is unreadable.
		std::filesystem::path prefabPath = std::filesystem::path(targetDirectory) / (entityName + ".prefab");
		std::error_code existsEc;
		for (int n = 1; std::filesystem::exists(prefabPath, existsEc) && n < 10000; ++n) {
			prefabPath = std::filesystem::path(targetDirectory) / (entityName + " (" + std::to_string(n) + ").prefab");
			existsEc.clear();
		}

		// Convert root AND children — marking only the root left children as plain scene entities (half-converted prefab bug).
		if (SceneSerializer::SaveEntityAsPrefabInstance(*scene, entityHandle, prefabPath.string()) == 0) {
			return false;
		}
		return true;
	}

	std::vector<std::string> AssetBrowser::GetSelectedPaths() const {
		if (!m_SelectedPaths.empty()) {
			return m_SelectedPaths;
		}

		if (!m_SelectedPath.empty()) {
			return { m_SelectedPath };
		}

		return {};
	}

#ifdef IDX_PLATFORM_WINDOWS
	void AssetBrowser::PrepareExternalFileDrag(const DirectoryEntry& entry) {
		m_ExternalDragPaths = IsPathSelected(entry.Path)
			? GetSelectedPaths()
			: std::vector<std::string>{ entry.Path };

		POINT point{};
		if (GetCursorPos(&point)) {
			m_ExternalDragStartScreenX = point.x;
			m_ExternalDragStartScreenY = point.y;
		}
		else {
			const ImGuiIO& io = ImGui::GetIO();
			m_ExternalDragStartScreenX = static_cast<int>(io.MousePos.x);
			m_ExternalDragStartScreenY = static_cast<int>(io.MousePos.y);
		}

		HWND hwnd = GetCurrentEditorViewportHwnd();
		if (hwnd) {
			SetCapture(hwnd);
			m_ExternalDragCaptureWindow = hwnd;
		}
	}

	void AssetBrowser::ClearExternalFileDrag() {
		if (m_ExternalDragCaptureWindow) {
			HWND captured = static_cast<HWND>(m_ExternalDragCaptureWindow);
			if (GetCapture() == captured) {
				ReleaseCapture();
			}
			m_ExternalDragCaptureWindow = nullptr;
		}
		m_ExternalDragPaths.clear();
	}

	void AssetBrowser::MaybeStartExternalFileDrag() {
		if (m_ExternalDragPaths.empty()) {
			return;
		}

		// TEMP EXT-DRAG DIAG — remove after debugging
		{
			const bool diagLbtn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			const bool diagPast = IsScreenDragPastThreshold(m_ExternalDragStartScreenX, m_ExternalDragStartScreenY);
			const bool diagInside = IsCursorInsideAnyEditorWindow();
			IDX_CORE_INFO_TAG("ExtDragDiag",
				"tracking paths={} lbtnDown={} pastThreshold={} insideEditor={}",
				m_ExternalDragPaths.size(), diagLbtn, diagPast, diagInside);
		}

		if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
			ClearExternalFileDrag();
			return;
		}

		if (!IsScreenDragPastThreshold(m_ExternalDragStartScreenX, m_ExternalDragStartScreenY)
			|| IsCursorInsideAnyEditorWindow()) {
			return;
		}

		std::vector<std::string> paths = std::move(m_ExternalDragPaths);
		m_ExternalDragPaths.clear();
		if (m_ExternalDragCaptureWindow) {
			HWND captured = static_cast<HWND>(m_ExternalDragCaptureWindow);
			if (GetCapture() == captured) {
				ReleaseCapture();
			}
			m_ExternalDragCaptureWindow = nullptr;
		}
		IDX_CORE_INFO_TAG("ExtDragDiag", "FIRING StartNativeFileDrag for {} paths", paths.size());
		const bool diagDragOk = StartNativeFileDrag(paths);
		IDX_CORE_INFO_TAG("ExtDragDiag", "StartNativeFileDrag returned {}", diagDragOk);
	}
#endif

	void AssetBrowser::SetSingleSelection(const std::string& path, int index) {
		m_SelectedPath = path;
		m_SelectedPaths = { path };
		m_LastSelectionIndex = index;
	}

	void AssetBrowser::ToggleSelection(const std::string& path, int index) {
		auto it = std::find(m_SelectedPaths.begin(), m_SelectedPaths.end(), path);
		if (it != m_SelectedPaths.end()) {
			m_SelectedPaths.erase(it);
			if (m_SelectedPath == path) {
				m_SelectedPath = m_SelectedPaths.empty() ? std::string() : m_SelectedPaths.back();
			}
		}
		else {
			m_SelectedPaths.push_back(path);
			m_SelectedPath = path;
		}

		m_LastSelectionIndex = index;
	}

	void AssetBrowser::SelectRange(int index) {
		if (m_VisibleEntryPaths.empty()) {
			return;
		}

		if (m_LastSelectionIndex < 0 || m_LastSelectionIndex >= static_cast<int>(m_VisibleEntryPaths.size())) {
			SetSingleSelection(m_VisibleEntryPaths[static_cast<std::size_t>(index)], index);
			return;
		}

		const int first = std::min(m_LastSelectionIndex, index);
		const int last = std::max(m_LastSelectionIndex, index);
		m_SelectedPaths.clear();
		for (int i = first; i <= last; ++i) {
			m_SelectedPaths.push_back(m_VisibleEntryPaths[static_cast<std::size_t>(i)]);
		}
		m_SelectedPath = m_VisibleEntryPaths[static_cast<std::size_t>(index)];
	}

	void AssetBrowser::HandleAssetShortcuts() {
		if (m_IsRenaming || ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper
			&& ImGui::IsKeyPressed(ImGuiKey_F, false)) {
			CreateFolder(m_CurrentDirectory);
			return;
		}
		if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
			&& (ImGui::IsKeyPressed(ImGuiKey_Delete, false)
				|| ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false))) {
			RequestDeleteSelectedAssets();
			return;
		}

		// Backspace: navigate up one directory. NavigateUp already guards
		// against escaping the project's Assets root, so pressing Backspace
		// while there is a no-op (no need for a separate root check here).
		if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
			&& ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
			NavigateUp();
			return;
		}

		// Enter: open the currently selected asset (same path as double-click).
		// Folders navigate into; files route through OpenAssetExternal which
		// scenes/prefabs/scripts all hook for their specialised open flow.
		if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
			&& (ImGui::IsKeyPressed(ImGuiKey_Enter, false)
				|| ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
			if (!m_SelectedPath.empty()) {
				OpenAssetPath(m_SelectedPath);
			}
			return;
		}

		// Arrow keys: Left = previous tile (one step lower in the row-major
		// visible list), Right = next tile. Matches the visual layout — the
		// arrow direction equals the direction the selection moves.
		if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
			&& !m_VisibleEntryPaths.empty()) {
			const int count = static_cast<int>(m_VisibleEntryPaths.size());
			int currentIndex = -1;
			if (!m_SelectedPath.empty()) {
				for (int i = 0; i < count; ++i) {
					if (m_VisibleEntryPaths[static_cast<std::size_t>(i)] == m_SelectedPath) {
						currentIndex = i;
						break;
					}
				}
			}

			auto select = [&](int index) {
				if (index < 0) index = 0;
				if (index >= count) index = count - 1;
				SetSingleSelection(m_VisibleEntryPaths[static_cast<std::size_t>(index)], index);
				m_SelectionActivated = true;
			};

			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
				select(currentIndex < 0 ? 0 : currentIndex - 1);
				return;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
				select(currentIndex < 0 ? 0 : currentIndex + 1);
				return;
			}
		}

		if (!io.KeyCtrl) {
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_X)) {
			CopySelectedAssets(true);
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_C)) {
			CopySelectedAssets(false);
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_V)) {
			PasteAssets();
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_D)) {
			DuplicateSelectedAssets();
		}
	}

	void AssetBrowser::CopySelectedAssets(bool cut) {
		m_AssetClipboardPaths = GetSelectedPaths();
		m_AssetClipboardCut = cut && !m_AssetClipboardPaths.empty();
	}

	void AssetBrowser::PasteAssets() {
		if (m_AssetClipboardPaths.empty()) {
			return;
		}

		std::vector<std::string> pastedPaths;
		const std::filesystem::path targetDirectory(m_CurrentDirectory);

		for (const std::string& sourcePathString : m_AssetClipboardPaths) {
			const std::filesystem::path sourcePath(sourcePathString);
			std::error_code ec;
			if (!std::filesystem::exists(sourcePath, ec) || ec) {
				continue;
			}

			const bool sameDirectory = std::filesystem::equivalent(sourcePath.parent_path(), targetDirectory, ec) && !ec;
			const std::filesystem::path targetPath = MakeUniqueAssetPath(sourcePath, targetDirectory, !sameDirectory);

			bool succeeded = false;
			if (m_AssetClipboardCut) {
				if (sameDirectory) {
					continue;
				}
				succeeded = MoveEntryTo(sourcePath, targetPath);
			}
			else {
				succeeded = CopyEntryTo(sourcePath, targetPath);
				if (succeeded) {
					SyncSceneEmbeddedNameIfNeeded(targetPath);
				}
			}

			if (succeeded) {
				pastedPaths.push_back(targetPath.string());
				m_Thumbnails.Invalidate(sourcePath.string());
				m_Thumbnails.Invalidate(targetPath.string());
			}
		}

		if (m_AssetClipboardCut) {
			m_AssetClipboardPaths.clear();
			m_AssetClipboardCut = false;
		}

		if (!pastedPaths.empty()) {
			m_SelectedPaths = pastedPaths;
			m_SelectedPath = pastedPaths.back();
			m_LastSelectionIndex = -1;
			m_NeedsRefresh = true;
		}
	}

	void AssetBrowser::DuplicateSelectedAssets() {
		const std::vector<std::string> selectedPaths = GetSelectedPaths();
		if (selectedPaths.empty()) {
			return;
		}

		std::vector<std::string> duplicatedPaths;
		for (const std::string& sourcePathString : selectedPaths) {
			const std::filesystem::path sourcePath(sourcePathString);
			std::error_code ec;
			if (!std::filesystem::exists(sourcePath, ec) || ec) {
				continue;
			}

			const std::filesystem::path targetPath = MakeUniqueAssetPath(sourcePath, sourcePath.parent_path(), false);
			if (CopyEntryTo(sourcePath, targetPath)) {
				SyncSceneEmbeddedNameIfNeeded(targetPath);
				duplicatedPaths.push_back(targetPath.string());
				m_Thumbnails.Invalidate(targetPath.string());
			}
		}

		if (!duplicatedPaths.empty()) {
			m_SelectedPaths = duplicatedPaths;
			m_SelectedPath = duplicatedPaths.back();
			m_LastSelectionIndex = -1;
			m_NeedsRefresh = true;
		}
	}

	void AssetBrowser::DeleteSelectedAssets() {
		const std::vector<std::string> paths = GetSelectedPaths();
		for (const std::string& path : paths) {
			DeleteEntry(path);
		}
	}

	void AssetBrowser::Render() {
		m_SelectionActivated = false;

#ifdef IDX_PLATFORM_WINDOWS
		MaybeStartExternalFileDrag();
#endif

		// Previous frame's deleted assets land here. Destroying their
		// Texture2D now is safe — prior frame's ImGui draws have been
		// dispatched and the wgpu command buffer is no longer reachable.
		// Doing it any earlier would dangle the WGPUTextureView mid-frame
		// (see m_PendingThumbnailInvalidates header comment).
		if (!m_PendingThumbnailInvalidates.empty()) {
			for (const std::string& p : m_PendingThumbnailInvalidates) {
				m_Thumbnails.Invalidate(p);
			}
			m_PendingThumbnailInvalidates.clear();
		}

		// Load pending scene on main thread (before ImGui frame)
		if (!m_PendingSceneLoad.empty())
		{
			std::string scenePath = m_PendingSceneLoad;
			m_PendingSceneLoad.clear();

			Scene* active = SceneManager::Get().GetActiveScene();
			if (active)
			{
				SceneSerializer::LoadFromFile(*active, scenePath);

				IndexProject* project = ProjectManager::GetCurrentProject();
				if (project)
				{
					std::string sceneName = std::filesystem::path(scenePath).stem().string();
					project->LastOpenedScene = sceneName;
					project->Save();
				}
			}
		}

		ImGui::Begin("Project");

		if (m_NeedsRefresh) {
			Refresh();
		}

		RenderBreadcrumb();
		ImGui::Separator();
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& ImGui::IsKeyPressed(ImGuiKey_F2)
			&& !ImGui::GetIO().WantTextInput
			&& !ImGui::IsAnyItemActive()) {
			BeginRenameSelected();
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
			HandleAssetShortcuts();
		}
		RenderGrid();

		RenderCreationCollisionPrompt();
		RenderDeleteConfirmationPrompt();

		ImGui::End();
	}

	void AssetBrowser::RenderBreadcrumb() {
		// Build the full segment list: [Assets, intermediate..., current].
		// Each entry holds the visible label and the absolute path the entry
		// navigates to.
		struct BreadcrumbSegment {
			std::string Label;
			std::string FullPath;
		};

		std::filesystem::path root(m_RootDirectory);
		std::filesystem::path current(m_CurrentDirectory);
		std::filesystem::path relative = std::filesystem::relative(current, root);

		std::vector<BreadcrumbSegment> segments;
		segments.push_back({ "Assets", m_RootDirectory });
		if (relative != "." && !relative.empty()) {
			std::filesystem::path accumulated = root;
			for (const auto& seg : relative) {
				accumulated /= seg;
				segments.push_back({ seg.string(), accumulated.string() });
			}
		}

		// Back button: push FramePadding.y=0 so it advertises baseline_y=0, matching SmallButton's AlignTextBaseLine flag; otherwise the arrow renders ~3px above the breadcrumb text.
		if (m_CurrentDirectory != m_RootDirectory) {
			const ImGuiStyle& s = ImGui::GetStyle();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s.FramePadding.x, 0.0f));
			const float h = ImGui::GetFontSize();
			const bool clicked = Icons::IconButton(
				"##AssetBrowserBack", Icons::Type::ArrowLeft, ImVec2(h, h));
			ImGui::PopStyleVar();
			if (clicked) {
				NavigateUp();
				return;
			}
			ImGui::SameLine();
		}

		// Width budget. Measured fresh every frame so resizing the panel
		// re-computes which segments fit.
		const ImGuiStyle& style = ImGui::GetStyle();
		const float refreshButtonReserved = 30.0f + style.ItemSpacing.x;
		const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - refreshButtonReserved);

		auto buttonWidth = [&style](const std::string& label) -> float {
			return ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2.0f;
		};
		const float separatorWidth = style.ItemSpacing.x + ImGui::CalcTextSize("/").x + style.ItemSpacing.x;
		const float truncationButtonWidth = buttonWidth("...");

		// Decide which segments are visible. Always show first (root) and
		// last (current); fill intermediates from the deepest end backward
		// while they still fit alongside the truncation marker.
		std::vector<bool> visible(segments.size(), false);
		bool showTruncation = false;

		if (segments.size() == 1) {
			visible[0] = true;
		}
		else {
			// First try: do all segments fit without any truncation marker?
			float total = buttonWidth(segments[0].Label);
			for (std::size_t i = 1; i < segments.size(); ++i) {
				total += separatorWidth + buttonWidth(segments[i].Label);
			}

			if (total <= available) {
				std::fill(visible.begin(), visible.end(), true);
			}
			else {
				visible.front() = true;
				visible.back() = true;

				// Fixed budget cost: Assets / ... / Current.
				const float fixedCost = buttonWidth(segments.front().Label)
					+ separatorWidth + truncationButtonWidth
					+ separatorWidth + buttonWidth(segments.back().Label);

				float remaining = available - fixedCost;

				if (remaining > 0.0f) {
					for (std::size_t k = segments.size() - 1; k-- > 1; ) {
						const float extra = separatorWidth + buttonWidth(segments[k].Label);
						if (extra <= remaining) {
							visible[k] = true;
							remaining -= extra;
						}
						else {
							break;
						}
					}
				}

				// At least one intermediate had to be hidden — show the marker.
				for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
					if (!visible[i]) { showTruncation = true; break; }
				}
			}
		}

		// ── Render ───────────────────────────────────────────────────

		auto isCurrent = [this](const BreadcrumbSegment& s) {
			return s.FullPath == m_CurrentDirectory;
		};

		// Root.
		if (isCurrent(segments[0])) {
			ImGui::TextUnformatted(segments[0].Label.c_str());
		}
		else if (ImGui::SmallButton(segments[0].Label.c_str())) {
			NavigateTo(segments[0].FullPath);
			return;
		}

		if (showTruncation) {
			ImGui::SameLine();
			ImGui::TextUnformatted("/");
			ImGui::SameLine();
			if (ImGui::SmallButton("...##BreadcrumbTrunc")) {
				ImGui::OpenPopup("BreadcrumbHidden");
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Hidden directories");
			}
			if (ImGui::BeginPopup("BreadcrumbHidden")) {
				for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
					if (visible[i]) continue;
					// Append the full path as the ID suffix so duplicate folder
					// names (e.g. multiple "New Folder" ancestors) don't collide.
					const std::string itemId = segments[i].Label + "##" + segments[i].FullPath;
					if (ImGui::MenuItem(itemId.c_str())) {
						NavigateTo(segments[i].FullPath);
						ImGui::EndPopup();
						return;
					}
				}
				ImGui::EndPopup();
			}
		}

		// Visible intermediates and current — same separator pattern as
		// before so the layout is unchanged when nothing's hidden.
		for (std::size_t i = 1; i < segments.size(); ++i) {
			if (!visible[i]) continue;
			ImGui::SameLine();
			ImGui::TextUnformatted("/");
			ImGui::SameLine();
			if (isCurrent(segments[i])) {
				ImGui::TextUnformatted(segments[i].Label.c_str());
			}
			else {
				const std::string id = segments[i].Label + "##" + segments[i].FullPath;
				if (ImGui::SmallButton(id.c_str())) {
					NavigateTo(segments[i].FullPath);
					return;
				}
			}
		}

		// Refresh button (right-aligned, unchanged).
		ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 30.0f);
		{
			uint64_t refreshIcon = EditorIcons::Get("reset", 16);
			bool clicked = false;
			if (refreshIcon) {
				clicked = ImGui::ImageButton("##Refresh",
					static_cast<ImTextureID>(static_cast<intptr_t>(refreshIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0));
			} else {
				clicked = ImGui::SmallButton("R");
			}
			if (clicked) m_NeedsRefresh = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh");
		}
	}

	void AssetBrowser::RenderGrid() {
		ImGui::BeginChild("AssetGrid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);

		// Snapshot full child rect for the panel-wide HIERARCHY_ENTITY drop target — covers tile gaps and empty rows that BeginDragDropTarget() would miss.
		ImGuiWindow* const gridWindow = ImGui::GetCurrentWindow();
		const ImRect gridChildRect = gridWindow->Rect();

		const float cellSize = m_TileSize + m_TilePadding;
		const float panelWidth = ImGui::GetContentRegionAvail().x;
		int columns = static_cast<int>(panelWidth / cellSize);
		if (columns < 1) columns = 1;

		m_ItemRightClicked = false;

		if (ImGui::IsWindowHovered()
			&& ImGui::IsMouseReleased(ImGuiMouseButton_Left)
			&& !IsLeftMouseDragPastClickThreshold()
			&& !ImGui::IsAnyItemHovered()) {
			ClearAssetSelection();
		}

		// Avoid copying m_Entries (can be thousands of entries) every frame; only the
		// rare pending-rename case needs a mutable copy with one synthetic entry. m_Entries
		// is mutated only by Refresh() (not mid-render), so referencing it is safe here.
		std::vector<DirectoryEntry> entriesWithPending;
		const std::vector<DirectoryEntry>* visibleEntriesPtr = &m_Entries;
		if (m_PendingScriptType != PendingScriptType::None
			&& m_PendingScriptDir == m_CurrentDirectory
			&& !m_RenamePath.empty()) {
			DirectoryEntry pendingEntry;
			pendingEntry.Path = m_RenamePath;
			pendingEntry.Name = std::filesystem::path(m_RenamePath).filename().string();
			pendingEntry.IsDirectory = false;
			entriesWithPending = m_Entries;
			entriesWithPending.push_back(std::move(pendingEntry));
			visibleEntriesPtr = &entriesWithPending;
		}

		const std::vector<DirectoryEntry>& visibleEntries = *visibleEntriesPtr;
		m_VisibleEntryPaths.clear();
		m_VisibleEntryPaths.reserve(visibleEntries.size());
		for (const DirectoryEntry& entry : visibleEntries) {
			m_VisibleEntryPaths.push_back(entry.Path);
		}

		// Track tiles placed (not source-entry index) — spliced slice tiles shift the wrap point so `i % columns` would place siblings in the wrong row.
		int tilesPlaced = 0;
		for (int i = 0; i < static_cast<int>(visibleEntries.size()); i++) {
			if (tilesPlaced > 0 && tilesPlaced % columns != 0) {
				ImGui::SameLine();
			}
			RenderAssetTile(visibleEntries[i], i);
			++tilesPlaced;

			auto sliceIt = m_SliceCache.find(visibleEntries[i].Path);
			if (sliceIt == m_SliceCache.end()) continue;
			if (m_ExpandedTextures.find(visibleEntries[i].Path) == m_ExpandedTextures.end()) continue;

			const std::vector<SpriteSlice>& slices = sliceIt->second;
			for (int s = 0; s < static_cast<int>(slices.size()); ++s) {
				if (tilesPlaced % columns != 0) {
					ImGui::SameLine();
				}
				RenderSliceTile(visibleEntries[i], slices[s], s, /*tileIndex*/ -1 - s);
				++tilesPlaced;
			}
		}

		if (visibleEntries.empty()) {
			ImGui::TextDisabled("Empty folder");
		}

		RenderGridContextMenu();

		// Panel-wide drop target via custom rect: BeginDragDropTarget() would only cover the last tile; IsAnyItemHovered() prevents overriding a tile-level target.
		if (!ImGui::IsAnyItemHovered()) {
			const ImGuiID dropZoneId = ImGui::GetID("##AssetGridEmptySpaceDrop");
			if (ImGui::BeginDragDropTargetCustom(gridChildRect, dropZoneId)) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
					if (TryCreatePrefabFromHierarchyDrop(payload, m_CurrentDirectory)) {
						m_NeedsRefresh = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		ImGui::EndChild();
	}

	ImVec2 PixelSnap(ImVec2 p)
	{
		return ImVec2(floorf(p.x) + 0.5f, floorf(p.y) + 0.5f);
	}

	void AssetBrowser::RenderAssetTile(const DirectoryEntry& entry, int index) {
		ImGui::PushID(index);

		const bool isSelected = IsPathSelected(entry.Path);
		const bool isCut = IsPathInCutClipboard(entry.Path);

		ImGui::BeginGroup();

		ImVec2 cursorPos = ImGui::GetCursorScreenPos();
		const ImVec2 selectionMin(cursorPos.x - 2.0f, cursorPos.y - 2.0f);
		const ImVec2 selectionMax(
			cursorPos.x + m_TileSize + 2.0f,
			cursorPos.y + m_TileSize + ImGui::GetTextLineHeightWithSpacing() + 2.0f);



		if (isSelected)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();

			ImVec2 min = selectionMin;
			ImVec2 max = selectionMax;

			// One simple inset so the selection does not touch/clamp at item edges
			const float inset = 1.0f;
			min.x += inset;
			min.y += inset;
			max.x -= inset;
			max.y -= inset;

			// Important for crisp 1px AddRect border
			min = PixelSnap(min);
			max = PixelSnap(max);

			const float rounding = 2.0f;
			const float thickness = 1.0f;

			ImU32 fillColor = ImGui::GetColorU32(EditorTheme::Colors::AssetTileSelection);
			ImU32 borderColor = ImGui::GetColorU32(EditorTheme::Colors::AssetTileSelectionBorder);

			drawList->AddRectFilled(
				min,
				max,
				fillColor,
				rounding
			);

			drawList->AddRect(
				min,
				max,
				borderColor,
				rounding,
				ImDrawFlags_RoundCornersAll,
				thickness
			);
		}

		// Pushed AFTER the selection rect so the highlight renders at full color and only the icon+label dims.
		if (isCut) {
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
		}

		ImVec2 iconPos = cursorPos;
		uint64_t thumbnail = 0;

		if (!entry.IsDirectory) {
			thumbnail = m_Thumbnails.GetThumbnail(entry.Path);
		}

		if (thumbnail != 0) {
			auto it = m_Thumbnails.GetCacheEntry(entry.Path);
			float texW = m_TileSize, texH = m_TileSize;
			if (it) {
				texW = it->GetWidth();
				texH = it->GetHeight();
			}

			float drawW = m_TileSize;
			float drawH = m_TileSize;

			if (texW > 0.0f && texH > 0.0f) {
				float aspect = texW / texH;
				if (aspect > 1.0f) {
					drawH = m_TileSize / aspect;
				}
				else {
					drawW = m_TileSize * aspect;
				}
			}

			float offsetX = (m_TileSize - drawW) * 0.5f;
			float offsetY = (m_TileSize - drawH) * 0.5f;

			ImGui::SetCursorScreenPos(ImVec2(iconPos.x + offsetX, iconPos.y + offsetY));
			ImGui::Image(
				static_cast<ImTextureID>(static_cast<intptr_t>(thumbnail)),
				ImVec2(drawW, drawH),
				ImVec2(0, 1), ImVec2(1, 0)
			);

			ImGui::SetCursorScreenPos(ImVec2(iconPos.x, iconPos.y + m_TileSize));
		}
		else {
			AssetType type = entry.IsDirectory
				? AssetType::Folder
				: ThumbnailCache::GetAssetType(std::filesystem::path(entry.Path).extension().string());

			bool drewTexture = false;
			const char* iconName = nullptr;

			if (type == AssetType::Folder) {
				iconName = "open_folder";
			}
			else if (!entry.IsDirectory) {
				iconName = GetFileTypeIconName(std::filesystem::path(entry.Path).extension().string());
				if (!iconName) iconName = "file_fallback";
			}

			if (iconName) {
				uint64_t icon = EditorIcons::Get(iconName, (int)m_TileSize);
				if (icon) {
					const float pad = m_TileSize * 0.1f;
					const float drawSize = m_TileSize - pad * 2.0f;
					ImGui::SetCursorScreenPos(ImVec2(iconPos.x + pad, iconPos.y + pad));
					ImGui::Image(
						static_cast<ImTextureID>(static_cast<intptr_t>(icon)),
						ImVec2(drawSize, drawSize),
						ImVec2(0, 1), ImVec2(1, 0)
					);
					ImGui::SetCursorScreenPos(ImVec2(iconPos.x, iconPos.y + m_TileSize));
					drewTexture = true;
				}
			}

			if (!drewTexture) {
				ThumbnailCache::DrawAssetIcon(type, iconPos, m_TileSize);
				ImGui::Dummy(ImVec2(m_TileSize, m_TileSize));
			}
		}

		if (IsRenamingEntry(entry.Path)) {
			m_RenameFrameCounter++;

			ImGui::PushItemWidth(m_TileSize);

			if (m_RenameFrameCounter == 1) {
				ImGui::SetKeyboardFocusHere();
			}

			bool committed = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

			if (committed) {
				CommitRename();
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				CancelRename();
			}
			else if (m_RenameFrameCounter > 2 && !ImGui::IsItemActive()) {
				CommitRename();
			}

			ImGui::PopItemWidth();
		}
		else {
			const float maxWidth = m_TileSize;
						const bool showExt = EditorPreferences::GetShowFileExtensions();
			const std::string& fullName = entry.Name;
			std::string label = fullName;
			if (!showExt && !entry.IsDirectory) {
				const std::string stem = std::filesystem::path(fullName).stem().string();
				if (!stem.empty()) label = stem;
			}
			bool truncated = false;
			const std::string display = ImGuiUtils::Ellipsize(label, maxWidth, &truncated);
			const float textWidth = ImGui::CalcTextSize(display.c_str()).x;
			const float offsetX = (maxWidth - textWidth) * 0.5f;
			if (offsetX > 0.0f) {
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
			}
			ImGui::TextUnformatted(display.c_str());
			if (ImGui::IsItemHovered()) {
				if (truncated || label != fullName) {
					ImGui::SetTooltip("%s", fullName.c_str());
				}
			}
		}

		ImGui::EndGroup();

		if (isCut) {
			ImGui::PopStyleVar();
		}

		// MUST NOT submit any ImGui items here: an InvisibleButton would become LastItem (breaking IsItemHovered) and shift the SameLine cursor, causing tile overlap.
		// Use ImDrawList + IsMouseHoveringRect instead. expanderHovered gates all downstream click paths so double-click doesn't expand AND open the asset.
		bool expanderHovered = false;
		if (!entry.IsDirectory) {
			auto sliceIt = m_SliceCache.find(entry.Path);
			if (sliceIt != m_SliceCache.end() && !sliceIt->second.empty()) {
								const float triHit = 16.0f;
				const ImVec2 triMin(
					cursorPos.x + m_TileSize - 2.0f - triHit,
					cursorPos.y + (m_TileSize - triHit) * 0.5f);
				const ImVec2 triMax(triMin.x + triHit, triMin.y + triHit);
				expanderHovered =
					ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup
						| ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
					&& ImGui::IsMouseHoveringRect(triMin, triMax, /*clip*/ true);

				ImDrawList* dl = ImGui::GetWindowDrawList();
				const bool isExpanded = m_ExpandedTextures.find(entry.Path) != m_ExpandedTextures.end();
				// Background pill so the glyph stays readable on light textures.
				dl->AddRectFilled(triMin, triMax,
					IM_COL32(0, 0, 0, expanderHovered ? 160 : 110), 4.0f);
								const uint64_t arrowIcon = EditorIcons::Get(
					isExpanded ? "arrow_left" : "arrow_right", 16);
				if (arrowIcon != 0) {
					const ImU32 tint = expanderHovered
						? IM_COL32(255, 255, 255, 255)
						: IM_COL32(220, 220, 220, 200);
					dl->AddImage(
						static_cast<ImTextureID>(static_cast<intptr_t>(arrowIcon)),
						triMin, triMax,
						ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
						tint);
				}

				if (expanderHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					if (isExpanded) m_ExpandedTextures.erase(entry.Path);
					else            m_ExpandedTextures.insert(entry.Path);
				}
			}
		}

		if (!expanderHovered
			&& ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			m_PressedPath = entry.Path;
#ifdef IDX_PLATFORM_WINDOWS
			PrepareExternalFileDrag(entry);
#endif
		}

		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			if (!expanderHovered
				&& ImGui::IsItemHovered() && m_PressedPath == entry.Path
				&& !IsLeftMouseDragPastClickThreshold()) {
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyShift) {
					SelectRange(index);
				}
				else if (io.KeyCtrl) {
					ToggleSelection(entry.Path, index);
				}
				else {
					SetSingleSelection(entry.Path, index);
				}
				m_SelectionActivated = true;
				if (!IsRenamingEntry(entry.Path)) {
					CancelRename();
				}
			}
			if (m_PressedPath == entry.Path) {
				m_PressedPath.clear();
#ifdef IDX_PLATFORM_WINDOWS
				ClearExternalFileDrag();
#endif
			}
		}

		if (!expanderHovered
			&& ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (entry.IsDirectory) {
				NavigateTo(entry.Path);  // deferred via m_NeedsRefresh
			} else {
				OpenAssetExternal(entry);  // deferred via m_DeferredOpenPath
			}
		}

		RenderItemContextMenu(entry, index);

		HandleDragSource(entry);
		if (entry.IsDirectory) {
			HandleDropTarget(entry);
		}
		else {
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
					if (TryCreatePrefabFromHierarchyDrop(payload, m_CurrentDirectory)) {
						m_NeedsRefresh = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		ImGui::PopID();

		ImGui::SameLine(0, 0);
		ImGui::Dummy(ImVec2(m_TilePadding, 0));
	}

	void AssetBrowser::OpenAssetPath(const std::string& path) {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec) || ec) {
			return;
		}

		if (std::filesystem::is_directory(path, ec) && !ec) {
			NavigateTo(path);
			return;
		}

		DirectoryEntry entry;
		entry.Path = path;
		entry.Name = std::filesystem::path(path).filename().string();
		entry.IsDirectory = false;
		OpenAssetExternal(entry);
	}

	void AssetBrowser::RevealAssetInExplorer(const std::string& path) {
#ifdef IDX_PLATFORM_WINDOWS
		std::string args = "/select,\"" + path + "\"";
		ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#else
		std::filesystem::path target(path);
		DirectoryEntry entry;
		entry.Path = target.has_parent_path() ? target.parent_path().string() : path;
		entry.Name = std::filesystem::path(entry.Path).filename().string();
		entry.IsDirectory = true;
		OpenAssetExternal(entry);
#endif
	}


	void AssetBrowser::RenderGridContextMenu() {
		if (!m_ItemRightClicked &&
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
		{
			ImGui::OpenPopup("##AssetGridCtx");
		}

		if (ImGui::BeginPopup("##AssetGridCtx")) {
			if (!m_AssetClipboardPaths.empty()) {
				if (ImGui::MenuItem("Paste Asset", "Ctrl+V")) {
					PasteAssets();
				}
				ImGui::Separator();
			}

			if (ImGui::MenuItem("Entity Prefab")) {
				CreateEntityPrefab(m_CurrentDirectory);
			}

			if (ImGui::BeginMenu("Texture")) {
				if (ImGui::MenuItem("Square")) {
					CreateDefaultTexture(m_CurrentDirectory, "Square.png", "Square");
				}
				if (ImGui::MenuItem("Circle")) {
					CreateDefaultTexture(m_CurrentDirectory, "circle.png", "Circle");
				}
				if (ImGui::MenuItem("Capsule")) {
					CreateDefaultTexture(m_CurrentDirectory, "Capsule.png", "Capsule");
				}
				if (ImGui::MenuItem("9-Sliced")) {
					CreateDefaultTexture(m_CurrentDirectory, "9Sliced.png", "9Sliced");
				}
				if (ImGui::MenuItem("Hexagon (Flat-Top)")) {
					CreateDefaultTexture(m_CurrentDirectory, "HexagonFlatTop.png", "HexagonFlatTop");
				}
				if (ImGui::MenuItem("Hexagon (Pointed-Top)")) {
					CreateDefaultTexture(m_CurrentDirectory, "HexagonPointedTop.png", "HexagonPointedTop");
				}
				if (ImGui::MenuItem("Isometric Diamond")) {
					CreateDefaultTexture(m_CurrentDirectory, "IsometricDiamond.png", "IsometricDiamond");
				}
				if (ImGui::MenuItem("Pixel")) {
					CreateDefaultTexture(m_CurrentDirectory, "Pixel.png", "Pixel");
				}
				if (ImGui::MenuItem("Invisible")) {
					CreateDefaultTexture(m_CurrentDirectory, "Invisible.png", "Invisible");
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Scripting")) {
				if (ImGui::MenuItem("EntityScript")) {
					CreateScript(m_CurrentDirectory);
				}
				if (ImGui::MenuItem("Component")) {
					CreateManagedCSharpComponent(m_CurrentDirectory);
				}
				if (ImGui::MenuItem("Native Component")) {
					CreateNativeCSharpComponent(m_CurrentDirectory);
				}
				if (ImGui::MenuItem("SceneSystem")) {
					CreateSceneSystem(m_CurrentDirectory);
				}
				if (ImGui::MenuItem("GlobalSystem")) {
					CreateGlobalSystem(m_CurrentDirectory);
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Data Asset")) {
				std::vector<char> typesBuf(8192);
				int required = ScriptEngine::GetDataAssetTypes(typesBuf.data(), static_cast<int>(typesBuf.size()));
				if (required > static_cast<int>(typesBuf.size())) {
					typesBuf.resize(static_cast<std::size_t>(required));
					ScriptEngine::GetDataAssetTypes(typesBuf.data(), static_cast<int>(typesBuf.size()));
				}

				// Build a submenu tree from each type's '/'-separated menu path
				// ("Gameplay/Item Data" -> Gameplay > Item Data). The last segment
				// is the creatable leaf; the earlier ones are nested submenus.
				struct MenuNode {
					std::string Label;
					std::string TypeName; // non-empty => creatable leaf
					std::vector<MenuNode> Children;
					MenuNode* FindGroup(const std::string& label) {
						for (auto& c : Children)
							if (c.TypeName.empty() && c.Label == label) return &c;
						return nullptr;
					}
				};

				MenuNode root;
				bool anyType = false;
				Json::Value parsed;
				if (typesBuf[0] != '\0' && Json::TryParse(typesBuf.data(), parsed) && parsed.IsArray()) {
					for (const Json::Value& item : parsed.GetArray()) {
						if (!item.IsObject()) continue;
						const Json::Value* typeMember = item.FindMember("type");
						if (!typeMember) continue;
						const std::string typeName = typeMember->AsStringOr();
						if (typeName.empty()) continue;
						const Json::Value* menuMember = item.FindMember("menu");
						std::string menuPath = menuMember ? menuMember->AsStringOr(typeName) : typeName;
						if (menuPath.empty()) menuPath = typeName;
						anyType = true;

						MenuNode* cur = &root;
						std::size_t start = 0;
						while (true) {
							const std::size_t slash = menuPath.find('/', start);
							std::string seg = (slash == std::string::npos)
								? menuPath.substr(start)
								: menuPath.substr(start, slash - start);
							if (slash == std::string::npos) {
								if (seg.empty()) seg = typeName;
								cur->Children.push_back(MenuNode{ seg, typeName, {} });
								break;
							}
							if (!seg.empty()) {
								MenuNode* group = cur->FindGroup(seg);
								if (!group) {
									cur->Children.push_back(MenuNode{ seg, std::string(), {} });
									group = &cur->Children.back();
								}
								cur = group;
							}
							start = slash + 1;
						}
					}
				}

				if (!anyType) {
					ImGui::TextDisabled("No DataAsset types");
				}
				else {
					std::function<void(const MenuNode&)> renderNode = [&](const MenuNode& node) {
						for (const MenuNode& child : node.Children) {
							if (!child.TypeName.empty()) {
								if (ImGui::MenuItem(child.Label.c_str()))
									CreateDataAsset(m_CurrentDirectory, child.TypeName);
							}
							else if (ImGui::BeginMenu(child.Label.c_str())) {
								renderNode(child);
								ImGui::EndMenu();
							}
						}
					};
					renderNode(root);
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("Empty File")) {
					CreateFile(m_CurrentDirectory, "NewFile", "", "");
				}
				if (ImGui::MenuItem("Text File")) {
					CreateFile(m_CurrentDirectory, "NewFile", ".txt", "");
				}
				if (ImGui::MenuItem("Binary File")) {
					CreateFile(m_CurrentDirectory, "NewFile", ".bin", "");
				}
				if (ImGui::MenuItem("JSON File")) {
					CreateFile(m_CurrentDirectory, "NewFile", ".json", "{\n}\n");
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("New Scene")) {
				CreateScene(m_CurrentDirectory);
			}
			if (ImGui::MenuItem("New Folder", "Shift + F")) {
				CreateFolder(m_CurrentDirectory);
			}
			ImGui::EndPopup();
		}
	}

	void AssetBrowser::RenderItemContextMenu(const DirectoryEntry& entry, int index) {
		if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
			ImGui::OpenPopup("##ItemCtx");
			m_ItemRightClicked = true;
			if (!IsPathSelected(entry.Path)) {
				SetSingleSelection(entry.Path, index);
			}
		}

		if (ImGui::BeginPopup("##ItemCtx")) {
			if (!IsPathSelected(entry.Path)) {
				SetSingleSelection(entry.Path, index);
			}

			if (ImGui::MenuItem("Copy", "Ctrl+C")) {
				CopySelectedAssets(false);
			}
						if (ImGui::MenuItem("Copy Path")) {
				CopyPathToClipboard(entry.Path);
			}

			if (ImGui::MenuItem("Open")) {
				OpenAssetPath(entry.Path);
				ImGui::EndPopup();
				return;
			}
			if (ImGui::MenuItem("Open in Explorer")) {
				RevealAssetInExplorer(entry.Path);
			}
			ImGui::Separator();

			if (ImGui::MenuItem("Delete", "Del")) {
				RequestDeleteSelectedAssets();
				ImGui::EndPopup();
				return;
			}
			if (ImGui::MenuItem("Rename", "F2", false, GetSelectedPaths().size() == 1)) {
				BeginRename(entry.Path, entry.Name);
			}
			if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
				DuplicateSelectedAssets();
			}

			ImGui::EndPopup();
		}
	}

	void AssetBrowser::RenderSliceTile(const DirectoryEntry& parentEntry,
		const SpriteSlice& slice, int sliceIndex, int tileIndex)
	{
		ImGui::PushID(tileIndex);
		ImGui::BeginGroup();

		const ImVec2 cursorPos = ImGui::GetCursorScreenPos();

		// Dummy reserves the full tile bbox — ImDrawList doesn't grow the bbox, so without it SameLine() would tuck the next tile against the label.
		ImGui::Dummy(ImVec2(m_TileSize, m_TileSize));

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 bgMin = cursorPos;
		const ImVec2 bgMax(cursorPos.x + m_TileSize, cursorPos.y + m_TileSize);
		dl->AddRectFilled(bgMin, bgMax, IM_COL32(35, 38, 46, 200), 3.0f);
		dl->AddRect(bgMin, bgMax, IM_COL32(70, 80, 100, 180), 3.0f);

		// Y-flipped UVs: thumbnails are uploaded bottom-up (same convention as the regular tile path).
		const uint64_t parentThumb = m_Thumbnails.GetThumbnail(parentEntry.Path);
		Texture2D* parentTex = m_Thumbnails.GetCacheEntry(parentEntry.Path);
		if (parentThumb != 0 && parentTex && parentTex->IsValid()
			&& parentTex->GetWidth() > 0 && parentTex->GetHeight() > 0
			&& slice.W > 0 && slice.H > 0)
		{
			const float texW = static_cast<float>(parentTex->GetWidth());
			const float texH = static_cast<float>(parentTex->GetHeight());
			const float u0 = static_cast<float>(slice.X) / texW;
			const float u1 = static_cast<float>(slice.X + slice.W) / texW;
			// Y-flip: the textured quad is drawn with (V=1) at the top, so the
			// slice's `y` (measured from the texture's top in pixel space) maps
			// to V = 1 - (y / texH).
			const float v0 = 1.0f - static_cast<float>(slice.Y) / texH;
			const float v1 = 1.0f - static_cast<float>(slice.Y + slice.H) / texH;

			// Aspect-correct fit inside the tile so a tall sub-rect doesn't
			// stretch horizontally. 8px margin keeps the slice visually
			// distinct from the tile border.
			const float margin = 8.0f;
			const float maxDim = m_TileSize - margin * 2.0f;
			const float sliceAspect = static_cast<float>(slice.W) / static_cast<float>(slice.H);
			float drawW = maxDim;
			float drawH = maxDim;
			if (sliceAspect > 1.0f) drawH = maxDim / sliceAspect;
			else                    drawW = maxDim * sliceAspect;
			const ImVec2 imgMin(
				cursorPos.x + (m_TileSize - drawW) * 0.5f,
				cursorPos.y + (m_TileSize - drawH) * 0.5f);
			const ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

			dl->AddImage(
				static_cast<ImTextureID>(static_cast<intptr_t>(parentThumb)),
				imgMin, imgMax,
				ImVec2(u0, v0), ImVec2(u1, v1));
		}
		else {
			// Parent thumbnail not loaded yet — touch the path so the
			// thumbnail cache starts loading it. Next frame the image will
			// show. Meanwhile draw a placeholder so the tile isn't blank.
			m_Thumbnails.GetThumbnail(parentEntry.Path);
			const ImVec2 phMin(cursorPos.x + 12.0f, cursorPos.y + 12.0f);
			const ImVec2 phMax(cursorPos.x + m_TileSize - 12.0f, cursorPos.y + m_TileSize - 12.0f);
			dl->AddRectFilled(phMin, phMax, IM_COL32(60, 60, 60, 200), 4.0f);
		}

		ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + m_TileSize));

		const float maxWidth = m_TileSize;
		bool truncated = false;
		const std::string display = ImGuiUtils::Ellipsize(slice.Name, maxWidth, &truncated);
		const float textWidth = ImGui::CalcTextSize(display.c_str()).x;
		const float offsetX = (maxWidth - textWidth) * 0.5f;
		if (offsetX > 0.0f) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
		}
		ImGui::TextUnformatted(display.c_str());
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", slice.Name.c_str());
		}

		ImGui::EndGroup();

		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			SpriteSliceDragPayload payload;
			payload.Reset();
			payload.SetTexturePath(parentEntry.Path);
			payload.SetSliceName(slice.Name);
			ImGui::SetDragDropPayload(k_SpriteSliceDragPayloadType,
				&payload, sizeof(payload));
			ImGui::Text("Sprite: %s > %s",
				std::filesystem::path(parentEntry.Path).filename().string().c_str(),
				slice.Name.c_str());
			ImGui::EndDragDropSource();
		}

		ImGui::PopID();

		// Match the regular tile's trailing horizontal padding so columns
		// stay aligned when the next tile lands via SameLine().
		ImGui::SameLine(0, 0);
		ImGui::Dummy(ImVec2(m_TilePadding, 0));

		(void)sliceIndex;
	}

	void AssetBrowser::HandleDragSource(const DirectoryEntry& entry) {
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			const char* pathStr = entry.Path.c_str();
			ImGui::SetDragDropPayload("ASSET_BROWSER_ITEM", pathStr, entry.Path.size() + 1);
			std::vector<std::string> dragPaths = IsPathSelected(entry.Path)
				? GetSelectedPaths()
				: std::vector<std::string>{ entry.Path };
			if (dragPaths.size() > 1) {
				ImGui::Text("Drag %zu assets", dragPaths.size());
			}
			else {
				ImGui::Text("Drag: %s", entry.Name.c_str());
			}
			ImGui::EndDragDropSource();
		}
	}

	void AssetBrowser::HandleDropTarget(const DirectoryEntry& entry) {
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				std::string sourcePath(static_cast<const char*>(payload->Data));

				if (sourcePath != entry.Path) {
					if (Directory::Move(sourcePath, entry.Path)) {
						m_Thumbnails.Invalidate(sourcePath);
						if (m_SelectedPath == sourcePath) {
							m_SelectedPath.clear();
						}
						m_SelectedPaths.erase(
							std::remove(m_SelectedPaths.begin(), m_SelectedPaths.end(), sourcePath),
							m_SelectedPaths.end());
						if (m_PressedPath == sourcePath) {
							m_PressedPath.clear();
						}
						m_NeedsRefresh = true;
					}
				}
			}
			// Hierarchy entity dropped directly onto a folder tile — save the
			// prefab inside THAT folder, not the current directory.
			else if (const ImGuiPayload* entityPayload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
				if (TryCreatePrefabFromHierarchyDrop(entityPayload, entry.Path)) {
					m_NeedsRefresh = true;
				}
			}
			ImGui::EndDragDropTarget();
		}
	}


}
