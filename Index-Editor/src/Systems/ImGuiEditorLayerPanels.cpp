#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include "Assets/AssetRegistry.hpp"
#include "Collections/AspectRatio.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Graphics/ImageData.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/ImGuiImplWebGPU.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Packages/GitHubSource.hpp"
#include "Packages/NuGetSource.hpp"
#include "Project/BuildPlatformSupport.hpp"
#include "Project/IndexBuildProfile.hpp"
#include "Project/IndexProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/SplashAssetResolve.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptComponentInspector.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Utils/Process.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <unordered_set>

#ifdef IDX_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Index {
	namespace {
		bool BrowseAcceptImageDrop(std::span<const std::string_view> extWhitelist,
			const std::function<void(const std::string&)>& onDrop)
		{
			if (!ImGui::BeginDragDropTarget()) return false;
			bool applied = false;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				if (payload->Data && payload->DataSize > 0) {
					std::string dropped(static_cast<const char*>(payload->Data),
						static_cast<std::size_t>(payload->DataSize));
					if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
					if (!dropped.empty()) {
						bool extOk = extWhitelist.empty();
						if (!extOk) {
							std::string ext = std::filesystem::path(dropped).extension().string();
							std::transform(ext.begin(), ext.end(), ext.begin(),
								[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							for (std::string_view allowed : extWhitelist) {
								if (ext == allowed) { extOk = true; break; }
							}
						}
						if (extOk) {
							onDrop(dropped);
							applied = true;
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
			return applied;
		}

		std::string ToProjectRelativeAssetPath(const std::filesystem::path& absPath,
			const std::string& assetsDirectory)
		{
			std::filesystem::path assetsDir(assetsDirectory);
			if (absPath.string().find(assetsDir.string()) == 0) {
				return std::filesystem::relative(absPath, assetsDir.parent_path()).string();
			}
			return absPath.filename().string();
		}

		constexpr std::string_view k_ImageExts[] = {
			".png", ".jpg", ".jpeg", ".bmp"
		};

		struct RenderBackendOption {
			IndexProject::RenderBackend Backend;
			const char* Label;
		};

		constexpr RenderBackendOption k_RenderBackendOptions[] = {
			{ IndexProject::RenderBackend::Auto,       "Auto (platform best)" },
			{ IndexProject::RenderBackend::Vulkan,     "Vulkan" },
			{ IndexProject::RenderBackend::Direct3D11, "Direct3D 11" },
			{ IndexProject::RenderBackend::Direct3D12, "Direct3D 12" },
			{ IndexProject::RenderBackend::Metal,      "Metal" },
			{ IndexProject::RenderBackend::OpenGL,     "OpenGL" },
			{ IndexProject::RenderBackend::OpenGLES,   "OpenGL ES" },
		};

		const char* RenderBackendLabel(IndexProject::RenderBackend backend) {
			for (const RenderBackendOption& option : k_RenderBackendOptions) {
				if (option.Backend == backend) return option.Label;
			}
			return "Auto (platform best)";
		}

		bool IsRenderBackendSupportedOnHost(IndexProject::RenderBackend backend) {
			if (backend == IndexProject::RenderBackend::Auto) return true;
#if defined(IDX_PLATFORM_WINDOWS)
			return backend == IndexProject::RenderBackend::Vulkan
				|| backend == IndexProject::RenderBackend::Direct3D11
				|| backend == IndexProject::RenderBackend::Direct3D12
				|| backend == IndexProject::RenderBackend::OpenGL;
#elif defined(IDX_PLATFORM_MACOS)
			return backend == IndexProject::RenderBackend::Metal;
#elif defined(IDX_PLATFORM_LINUX)
			return backend == IndexProject::RenderBackend::Vulkan
				|| backend == IndexProject::RenderBackend::OpenGL
				|| backend == IndexProject::RenderBackend::OpenGLES;
#else
			return false;
#endif
		}

		const char* RenderBackendUnsupportedReason(IndexProject::RenderBackend backend) {
			if (IsRenderBackendSupportedOnHost(backend)) return nullptr;
			switch (backend) {
				case IndexProject::RenderBackend::Direct3D11:
				case IndexProject::RenderBackend::Direct3D12:
					return "Direct3D backends are only available on Windows.";
				case IndexProject::RenderBackend::Metal:
					return "Metal is only available on macOS.";
				case IndexProject::RenderBackend::Vulkan:
					return "Vulkan is not available for this host platform.";
				case IndexProject::RenderBackend::OpenGL:
					return "OpenGL is not available for this host platform.";
				case IndexProject::RenderBackend::OpenGLES:
					return "OpenGL ES is not available for this host platform.";
				case IndexProject::RenderBackend::Auto:
					break;
			}
			return "This backend is not available for this host platform.";
		}

		uint64_t ParsePickerAssetId(const std::string& value) {
			if (value.empty()) return 0;
			try {
				size_t parsed = 0;
				uint64_t id = std::stoull(value, &parsed, 10);
				return parsed == value.size() ? id : 0;
			}
			catch (...) {
				return 0;
			}
		}

		bool NeedsCopy(const std::filesystem::path& src, const std::filesystem::path& dest) {
			if (!std::filesystem::exists(dest)) return true;
			try {
				return std::filesystem::last_write_time(src) > std::filesystem::last_write_time(dest);
			}
			catch (...) {
				return true;
			}
		}

		int CopyDirIncremental(const std::filesystem::path& srcDir, const std::filesystem::path& destDir) {
			int copied = 0;
			std::filesystem::create_directories(destDir);
			for (auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
				auto rel = std::filesystem::relative(entry.path(), srcDir);
				auto dest = destDir / rel;
				try {
					if (entry.is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(entry.path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		bool IsSerializedAssetPath(const std::filesystem::path& path) {
			std::string ext = path.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return ext == ".scene" || ext == ".prefab";
		}

		int ConvertSerializedAssetsInDirectory(const std::filesystem::path& rootDir,
			SceneSerializationFormat format)
		{
			if (!std::filesystem::exists(rootDir)) {
				return 0;
			}

			int converted = 0;
			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(rootDir,
					std::filesystem::directory_options::skip_permission_denied, ec), end;
				it != end;
				it.increment(ec))
			{
				if (ec) {
					ec.clear();
					continue;
				}
				if (!it->is_regular_file(ec) || ec || !IsSerializedAssetPath(it->path())) {
					ec.clear();
					continue;
				}
				if (SceneSerializer::ConvertFileFormat(it->path().string(), format)) {
					++converted;
				}
			}
			return converted;
		}

		SceneSerializationFormat ToSceneSerializationFormat(
			IndexProject::ProjectAssetSerializationFormat format)
		{
			return format == IndexProject::ProjectAssetSerializationFormat::Binary
				? SceneSerializationFormat::Binary
				: SceneSerializationFormat::Json;
		}

		// Match is case-insensitive on Windows so "Editor" excludes regardless of the user's disk casing.
		int CopyDirIncrementalExcluding(const std::filesystem::path& srcDir,
			const std::filesystem::path& destDir,
			const std::vector<std::filesystem::path>& excludedRelativePaths)
		{
			int copied = 0;
			std::filesystem::create_directories(destDir);

			auto isExcluded = [&](const std::filesystem::path& rel) {
				const std::string relStr = rel.generic_string();
				for (const auto& excluded : excludedRelativePaths) {
					const std::string excStr = excluded.generic_string();
					// Match the excluded path itself or anything underneath it.
#ifdef IDX_PLATFORM_WINDOWS
					if (relStr.size() >= excStr.size()
						&& _strnicmp(relStr.c_str(), excStr.c_str(), excStr.size()) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#else
					if (relStr.size() >= excStr.size()
						&& relStr.compare(0, excStr.size(), excStr) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#endif
				}
				return false;
			};

			for (auto it = std::filesystem::recursive_directory_iterator(srcDir);
				it != std::filesystem::recursive_directory_iterator(); ++it)
			{
				auto rel = std::filesystem::relative(it->path(), srcDir);
				if (isExcluded(rel)) {
					if (it->is_directory()) {
						it.disable_recursion_pending();
					}
					continue;
				}
				auto dest = destDir / rel;
				try {
					if (it->is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(it->path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		std::string GetRuntimeExecutableFilename() {
#if defined(IDX_PLATFORM_WINDOWS)
			return "Index-Runtime.exe";
#else
			return "Index-Runtime";
#endif
		}

#ifdef IDX_PLATFORM_WINDOWS
		// Layout: [IconDir 6B][IcoEntry 16B][BITMAPINFOHEADER 40B, biHeight=2*H][BGRA pixels bottom-up][AND mask 1bpp 4B-aligned, all zero].
		std::string BuildIcoBytesFromImage(const std::filesystem::path& imagePath,
			std::vector<std::uint8_t>& outIcoBytes)
		{
#pragma pack(push, 1)
			struct IconDirHdr {
				std::uint16_t Reserved;
				std::uint16_t Type;
				std::uint16_t Count;
			};
			struct IcoEntryHdr {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint32_t ImageOffset;
			};
			struct BitmapInfoHdr {
				std::uint32_t biSize;
				std::int32_t  biWidth;
				std::int32_t  biHeight;
				std::uint16_t biPlanes;
				std::uint16_t biBitCount;
				std::uint32_t biCompression;
				std::uint32_t biSizeImage;
				std::int32_t  biXPelsPerMeter;
				std::int32_t  biYPelsPerMeter;
				std::uint32_t biClrUsed;
				std::uint32_t biClrImportant;
			};
#pragma pack(pop)

			// flipVertical=false: pixels come top-down; we flip + BGRA-swizzle into the DIB below.
			std::unique_ptr<ImageData> img = Texture2D::DecodeFileToCpu(
				imagePath.string().c_str(), /*flipVertical*/ false);
			if (!img) {
				return "failed to decode image";
			}

			const int width = img->Width;
			const int height = img->Height;
			if (width <= 0 || height <= 0) {
				return "image has zero size";
			}
			// Win32 ICO entries store width/height in 8-bit fields with 0
			// meaning "256 or larger". We refuse > 256 — Explorer scales the
			// largest entry it finds anyway, and accidentally writing a 257
			// would silently truncate to 1 in the on-disk entry.
			if (width > 256 || height > 256) {
				return "image exceeds 256x256 (use a smaller icon source)";
			}

			const std::uint32_t pixelBytes =
				static_cast<std::uint32_t>(width) *
				static_cast<std::uint32_t>(height) * 4u;
			// AND mask: 1 bit per pixel, each row padded to a 4-byte boundary.
			const std::uint32_t maskRowBytes =
				((static_cast<std::uint32_t>(width) + 31u) / 32u) * 4u;
			const std::uint32_t maskBytes = maskRowBytes * static_cast<std::uint32_t>(height);
			const std::uint32_t dibBytes = sizeof(BitmapInfoHdr) + pixelBytes + maskBytes;

			outIcoBytes.assign(sizeof(IconDirHdr) + sizeof(IcoEntryHdr) + dibBytes, 0);

			IconDirHdr dir{};
			dir.Reserved = 0;
			dir.Type = 1;
			dir.Count = 1;
			std::memcpy(outIcoBytes.data(), &dir, sizeof(dir));

			IcoEntryHdr entry{};
			entry.Width = static_cast<std::uint8_t>(width == 256 ? 0 : width);
			entry.Height = static_cast<std::uint8_t>(height == 256 ? 0 : height);
			entry.ColorCount = 0;
			entry.Reserved = 0;
			entry.Planes = 1;
			entry.BitCount = 32;
			entry.BytesInRes = dibBytes;
			entry.ImageOffset = sizeof(IconDirHdr) + sizeof(IcoEntryHdr);
			std::memcpy(outIcoBytes.data() + sizeof(IconDirHdr), &entry, sizeof(entry));

			BitmapInfoHdr bih{};
			bih.biSize = sizeof(BitmapInfoHdr);
			bih.biWidth = width;
			// biHeight = 2*H per ICO convention: XOR bitmap + AND mask stacked; positive = bottom-up DIB.
			bih.biHeight = height * 2;
			bih.biPlanes = 1;
			bih.biBitCount = 32;
			bih.biCompression = 0; // BI_RGB
			bih.biSizeImage = pixelBytes + maskBytes;
			std::uint8_t* dibStart = outIcoBytes.data() + entry.ImageOffset;
			std::memcpy(dibStart, &bih, sizeof(bih));

			// Swizzle RGBA top-down -> BGRA bottom-up. ImageData's pixels are
			// laid out row by row with row 0 at the top; the DIB needs row 0
			// at the bottom.
			const std::uint8_t* srcPixels = img->Pixels;
			std::uint8_t* dstPixels = dibStart + sizeof(BitmapInfoHdr);
			for (int y = 0; y < height; ++y) {
				const std::uint8_t* srcRow = srcPixels + (height - 1 - y) * width * 4;
				std::uint8_t* dstRow = dstPixels + y * width * 4;
				for (int x = 0; x < width; ++x) {
					dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
					dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
					dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
					dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
				}
			}
			// AND mask is all-zero (assign() above): signals "use alpha channel" for 32-bit icons.

			return std::string{};
		}

		// Read an existing .ico file straight into a byte vector. Used when
		// the user supplied a real .ico (no decode/re-encode needed).
		std::string ReadIcoFileBytes(const std::filesystem::path& icoPath,
			std::vector<std::uint8_t>& outIcoBytes)
		{
			std::ifstream in(icoPath, std::ios::binary);
			if (!in) return "open .ico failed";
			outIcoBytes.assign((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			return std::string{};
		}

		struct ExistingResName {
			bool IsNumeric = false;
			WORD NumericId = 0;
			std::string StringName;
		};

		std::vector<ExistingResName> EnumerateExistingResources(
			const std::string& exePath, WORD resType, std::string& outError)
		{
			std::vector<ExistingResName> out;
			HMODULE hMod = ::LoadLibraryExA(exePath.c_str(), nullptr,
				LOAD_LIBRARY_AS_DATAFILE);
			if (!hMod) {
				const DWORD err = ::GetLastError();
				outError = "LoadLibraryEx failed (error " +
					std::to_string(err) + ")";
				return out;
			}
			struct Ctx { std::vector<ExistingResName>* List; };
			Ctx ctx{ &out };
			auto cb = [](HMODULE, LPCSTR, LPSTR lpName, LONG_PTR lParam) -> BOOL {
				auto* c = reinterpret_cast<Ctx*>(lParam);
				ExistingResName r{};
				if ((reinterpret_cast<ULONG_PTR>(lpName) >> 16) == 0) {
					r.IsNumeric = true;
					r.NumericId = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(lpName) & 0xFFFF);
				}
				else {
					r.IsNumeric = false;
					r.StringName = lpName;
				}
				c->List->push_back(std::move(r));
				return TRUE;
			};
			if (!::EnumResourceNamesA(hMod, MAKEINTRESOURCEA(resType),
				cb, reinterpret_cast<LONG_PTR>(&ctx)))
			{
				const DWORD err = ::GetLastError();
				// ERROR_RESOURCE_TYPE_NOT_FOUND (1813) just means the .exe
				// has no resources of this type — return an empty list.
				if (err != ERROR_RESOURCE_TYPE_NOT_FOUND
					&& err != ERROR_RESOURCE_DATA_NOT_FOUND)
				{
					outError = "EnumResourceNames failed (error " +
						std::to_string(err) + ")";
				}
			}
			::FreeLibrary(hMod);
			return out;
		}

		// Retries up to 6 times on error 1359/32/33 and BeginUpdateResource failures — AV/Explorer hold the
		// freshly copied exe open transiently; polling fixes the race without permanently failing the Build.
		std::string EmbedIconIntoExecutableOnce(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes);

		std::string EmbedIconIntoExecutable(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes)
		{
			constexpr int k_MaxAttempts = 6;
			constexpr int k_BaseDelayMs = 150;
			std::string lastErr;
			for (int attempt = 0; attempt < k_MaxAttempts; ++attempt) {
				lastErr = EmbedIconIntoExecutableOnce(exePath, icoBytes);
				if (lastErr.empty()) return {};
				// Only retry on the well-known transient failure modes;
				// other errors (malformed .ico, ico-out-of-bounds, etc.)
				// won't fix themselves with a delay.
				const bool transient = lastErr.find("error 1359") != std::string::npos
					|| lastErr.find("error 32") != std::string::npos       // ERROR_SHARING_VIOLATION
					|| lastErr.find("error 33") != std::string::npos       // ERROR_LOCK_VIOLATION
					|| lastErr.find("BeginUpdateResource") != std::string::npos;
				if (!transient) return lastErr;
				::Sleep(static_cast<DWORD>(k_BaseDelayMs * (attempt + 1)));
			}
			return lastErr.empty() ? std::string("icon embed retry exhausted") : lastErr;
		}

		std::string EmbedIconIntoExecutableOnce(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes)
		{
#pragma pack(push, 1)
			struct IconDir {
				std::uint16_t Reserved;
				std::uint16_t Type;
				std::uint16_t Count;
			};
			struct IcoEntry {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint32_t ImageOffset;
			};
			// RT_GROUP_ICON wire format: trailing field is a 16-bit resource ID (not a 32-bit offset), so 14 bytes/entry; pack to avoid loader-rejected padding.
			struct GroupEntry {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint16_t Id;
			};
#pragma pack(pop)

			if (icoBytes.size() < sizeof(IconDir)) return "ico too small";

			IconDir dir{};
			std::memcpy(&dir, icoBytes.data(), sizeof(dir));
			if (dir.Type != 1 || dir.Count == 0) return "not a .ico";

			const std::size_t entriesEnd = sizeof(IconDir)
				+ static_cast<std::size_t>(dir.Count) * sizeof(IcoEntry);
			if (icoBytes.size() < entriesEnd) return "ico header truncated";

			std::vector<IcoEntry> entries(dir.Count);
			std::memcpy(entries.data(), icoBytes.data() + sizeof(IconDir),
				entries.size() * sizeof(IcoEntry));

			// Snapshot pre-existing icon ids BEFORE opening the update handle
			// (LoadLibraryEx + UpdateResource on the same file have to be
			// sequential). We only need these to choose fresh RT_ICON ids.
			std::string enumErr;
			std::vector<ExistingResName> existingIcons =
				EnumerateExistingResources(exePath, 3 /*RT_ICON*/, enumErr);
			if (!enumErr.empty()) return enumErr;

			HANDLE hUpdate = ::BeginUpdateResourceA(exePath.c_str(), FALSE);
			if (!hUpdate) {
				return "BeginUpdateResource failed (error " +
					std::to_string(::GetLastError()) + ")";
			}

			// Avoid the delete-then-add pattern: updating without deleting is more reliable on freshly copied exes.
			std::unordered_set<WORD> usedIconIds;
			for (const ExistingResName& r : existingIcons) {
				if (r.IsNumeric) usedIconIds.insert(r.NumericId);
			}
			std::uint32_t nextIconId = 256;
			std::vector<GroupEntry> groupEntries(dir.Count);
			for (std::size_t i = 0; i < entries.size(); ++i) {
				const IcoEntry& e = entries[i];
				if (e.ImageOffset + e.BytesInRes > icoBytes.size()) {
					::EndUpdateResourceA(hUpdate, TRUE);
					return "ico entry out of bounds";
				}
				while (nextIconId <= 0xFFFFu
					&& usedIconIds.find(static_cast<WORD>(nextIconId)) != usedIconIds.end())
				{
					++nextIconId;
				}
				if (nextIconId > 0xFFFFu) {
					::EndUpdateResourceA(hUpdate, TRUE);
					return "too many icon resources";
				}
				const WORD iconId = static_cast<WORD>(nextIconId++);
				usedIconIds.insert(iconId);
				// Use MAKEINTRESOURCEA (not RT_ICON macro): RT_ICON expands to WIDE under UNICODE, mismatching UpdateResourceA's LPCSTR.
				if (!::UpdateResourceA(hUpdate,
					MAKEINTRESOURCEA(3),
					MAKEINTRESOURCEA(iconId),
					MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
					const_cast<std::uint8_t*>(icoBytes.data()) + e.ImageOffset,
					e.BytesInRes))
				{
					const DWORD err = ::GetLastError();
					::EndUpdateResourceA(hUpdate, TRUE);
					return "UpdateResource(RT_ICON) failed (error " +
						std::to_string(err) + ")";
				}
				groupEntries[i] = GroupEntry{
					e.Width, e.Height, e.ColorCount, e.Reserved,
					e.Planes, e.BitCount, e.BytesInRes, iconId,
				};
			}

			std::vector<std::uint8_t> groupBlob(sizeof(IconDir)
				+ groupEntries.size() * sizeof(GroupEntry));
			std::memcpy(groupBlob.data(), &dir, sizeof(IconDir));
			std::memcpy(groupBlob.data() + sizeof(IconDir),
				groupEntries.data(),
				groupEntries.size() * sizeof(GroupEntry));

			// Group ID 1 matches Index-Runtime/icon.rc. Overwriting that group
			// makes the runtime point at the fresh icon resources above.
			if (!::UpdateResourceA(hUpdate,
				MAKEINTRESOURCEA(14),
				MAKEINTRESOURCEA(1),
				MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
				groupBlob.data(),
				static_cast<DWORD>(groupBlob.size())))
			{
				const DWORD err = ::GetLastError();
				::EndUpdateResourceA(hUpdate, TRUE);
				return "UpdateResource(RT_GROUP_ICON) failed (error " +
					std::to_string(err) + ")";
			}

			if (!::EndUpdateResourceA(hUpdate, FALSE)) {
				return "EndUpdateResource failed (error " +
					std::to_string(::GetLastError()) + ")";
			}
			return std::string{};
		}
#endif

		std::string GetEngineRuntimeFilename() {
#if defined(IDX_PLATFORM_WINDOWS)
			return "Index-Engine.dll";
#elif defined(__APPLE__)
			return "libIndex-Engine.dylib";
#else
			return "libIndex-Engine.so";
#endif
		}

		std::string GetNetHostRuntimeFilename() {
#if defined(IDX_PLATFORM_WINDOWS)
			return "nethost.dll";
#else
			return "libnethost.so";
#endif
		}

		// Filename of a premake `kind "SharedLib"` target on the current platform.
		// Windows: <Name>.dll, Linux: lib<Name>.so, macOS: lib<Name>.dylib.
		std::string SharedLibraryFilename(std::string_view projectName) {
#if defined(IDX_PLATFORM_WINDOWS)
			return std::string(projectName) + ".dll";
#elif defined(__APPLE__)
			return "lib" + std::string(projectName) + ".dylib";
#else
			return "lib" + std::string(projectName) + ".so";
#endif
		}

		std::string NormalizePreviewTexturePath(const std::filesystem::path& path) {
			std::error_code ec;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
			if (ec) {
				ec.clear();
				normalized = std::filesystem::absolute(path, ec);
				if (ec) {
					normalized = path.lexically_normal();
				}
			}

			return normalized.lexically_normal().make_preferred().string();
		}

		bool IsLogEntryVisible(Log::Level level, bool showInfo, bool showWarn, bool showError) {
			if (level <= Log::Level::Info) return showInfo;
			if (level == Log::Level::Warn) return showWarn;
			return showError;
		}

		const char* GetLogLevelPrefix(Log::Level level) {
			if (level <= Log::Level::Info) return "[Info] ";
			if (level == Log::Level::Warn) return "[Warn] ";
			return "[Error] ";
		}

		ImVec4 GetLogLevelColor(Log::Level level) {
			if (level <= Log::Level::Info) {
				const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
				const float luminance = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
				return luminance > 0.5f
					? ImVec4(0.20f, 0.25f, 0.32f, 1.0f)
					: ImVec4(0.86f, 0.88f, 0.92f, 1.0f);
			}
			if (level == Log::Level::Warn) return ImVec4(1.0f, 0.78f, 0.22f, 1.0f);
			return ImVec4(1.0f, 0.32f, 0.32f, 1.0f);
		}

		// EVERY ImGui::CollapsingHeader inside a RenderSettings_* helper MUST have a matching k_SettingsIndex entry — SectionVisible() fails closed for unlisted (category, section) pairs.
		ImVec4 GetLogLinkColor() {
			const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
			const float luminance = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
			return luminance > 0.5f
				? ImVec4(0.05f, 0.32f, 0.68f, 1.0f)
				: ImVec4(0.45f, 0.68f, 1.00f, 1.0f);
		}

		// SameLine(0,0) joining is required for inline links — ImGui wrapping breaks with multi-segment lines, so link lines sacrifice wrap (parent BeginChild has HScrollbar).
		void RenderLogMessageWithInlineLink(
			std::string_view fullText,
			size_t linkStart,
			size_t linkEnd,
			const std::string& filePath,
			int linkLine,
			const ImVec4& textColor)
		{
			const ImVec4 linkColor = GetLogLinkColor();
			const bool hasLink = !filePath.empty()
				&& linkEnd > linkStart
				&& linkEnd <= fullText.size();

			auto renderSegment = [&](std::string_view segment, bool isLink, bool firstOnLine) {
				if (segment.empty()) return;
				if (!firstOnLine) ImGui::SameLine(0.0f, 0.0f);

				const ImVec4& color = isLink ? linkColor : textColor;
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::TextUnformatted(segment.data(), segment.data() + segment.size());
				ImGui::PopStyleColor();

				if (!isLink) return;

				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddLine(
					ImVec2(min.x, max.y - 1.0f),
					ImVec2(max.x, max.y - 1.0f),
					ImGui::GetColorU32(linkColor),
					1.0f);

				if (ImGui::IsItemHovered()) {
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					ImGui::SetTooltip("Double-click to open");
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						ExternalEditor::OpenFile(filePath, linkLine);
					}
				}
			};

			size_t pos = 0;
			while (pos <= fullText.size()) {
				const size_t newlinePos = fullText.find('\n', pos);
				const size_t lineEnd = (newlinePos == std::string_view::npos)
					? fullText.size()
					: newlinePos;

				if (lineEnd == pos) {
					// Empty line — emit a blank line of standard text height
					// so consecutive '\n's preserve their vertical gap.
					ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
				}
				else if (hasLink && linkStart < lineEnd && linkEnd > pos) {
					const size_t segLinkStart = std::max(linkStart, pos);
					const size_t segLinkEnd = std::min(linkEnd, lineEnd);

					bool firstOnLine = true;
					if (segLinkStart > pos) {
						renderSegment(fullText.substr(pos, segLinkStart - pos), false, firstOnLine);
						firstOnLine = false;
					}
					renderSegment(
						fullText.substr(segLinkStart, segLinkEnd - segLinkStart),
						true, firstOnLine);
					firstOnLine = false;
					if (segLinkEnd < lineEnd) {
						renderSegment(fullText.substr(segLinkEnd, lineEnd - segLinkEnd), false, firstOnLine);
					}
				}
				else {
					renderSegment(fullText.substr(pos, lineEnd - pos), false, true);
				}

				if (newlinePos == std::string_view::npos) break;
				pos = newlinePos + 1;
			}
		}

		struct SettingsIndexEntry {
			ImGuiEditorLayer::SettingsCategory Category;
			const char* Section;   // EXACTLY as passed to ImGui::CollapsingHeader.
			const char* Keywords;  // Space-separated extra terms (lowercase).
		};

		constexpr SettingsIndexEntry k_SettingsIndex[] = {
			// Display (RenderSettings_Display)
			{ ImGuiEditorLayer::SettingsCategory::Display,  "Window",          "width height fullscreen resizable resolution window size run background" },
			{ ImGuiEditorLayer::SettingsCategory::Display,  "UI Scaling",      "ui scaling reference resolution canvas scaler match width height layout" },

			// Graphics (RenderSettings_Graphics)
			{ ImGuiEditorLayer::SettingsCategory::Graphics, "Render Backend",  "render backend rendering api dawn webgpu vulkan d3d11 d3d12 opengl metal auto" },
			{ ImGuiEditorLayer::SettingsCategory::Graphics, "Post-Processing", "post processing post-processing pp bloom postfx effects pipeline kill switch" },
			{ ImGuiEditorLayer::SettingsCategory::Graphics, "Default Font",    "default font typeface text font asset" },

			// Branding (RenderSettings_Branding)
			{ ImGuiEditorLayer::SettingsCategory::Branding, "Splash Screen",   "splash screen logo intro startup fade duration enable image background color text preview" },
			{ ImGuiEditorLayer::SettingsCategory::Branding, "App Icon",        "app icon application exe executable window taskbar shipped game" },
			{ ImGuiEditorLayer::SettingsCategory::Branding, "Cursors",         "cursor mouse pointer ui hover default" },

			// Build (RenderSettings_Build)
			{ ImGuiEditorLayer::SettingsCategory::Build,    "Executable",      "executable exe name output binary" },
			{ ImGuiEditorLayer::SettingsCategory::Build,    "Diagnostics",     "diagnostics runtime stats logs overlay fps f6 f7 debug" },
			{ ImGuiEditorLayer::SettingsCategory::Build,    "ECS Entity Bits", "ecs entity bits entt entities versions rebuild engine 16 20 22 24 28" },

			// Editor (RenderSettings_Editor)
			{ ImGuiEditorLayer::SettingsCategory::Editor,   "Asset Browser",   "asset browser duplicate suffix asset naming" },
			{ ImGuiEditorLayer::SettingsCategory::Editor,   "Entities",        "entities entity naming suffix unique duplicate" },
			{ ImGuiEditorLayer::SettingsCategory::Editor,   "Serialization",   "serialization serialize format json binary scene prefab assets" },
			{ ImGuiEditorLayer::SettingsCategory::Editor,   "Scripting",       "scripting scripts recompile auto build play mode dotnet csharp c#" },

			// Systems (RenderSettings_Systems)
			{ ImGuiEditorLayer::SettingsCategory::Systems,  "Global Systems",  "global systems globalsystem scene system registration project scope" },
		};

		constexpr const char* k_SettingsCategoryNamesLower[] = {
			"display", "graphics", "branding", "build", "editor", "systems",
		};
		static_assert(IM_ARRAYSIZE(k_SettingsCategoryNamesLower)
			== static_cast<int>(ImGuiEditorLayer::SettingsCategory::Systems) + 1,
			"k_SettingsCategoryNamesLower out of sync with SettingsCategory enum");

		bool SettingsContains(const char* haystackLower, const std::string& needleLower) {
			if (needleLower.empty()) return true;
			if (!haystackLower) return false;
			return std::string_view(haystackLower).find(needleLower) != std::string_view::npos;
		}

		bool SettingsEntryMatches(const std::string& filterLower,
			const SettingsIndexEntry& entry, const char* categoryNameLower)
		{
			if (filterLower.empty()) return true;
			if (SettingsContains(categoryNameLower, filterLower)) return true;

			std::string sectionLower(entry.Section ? entry.Section : "");
			std::transform(sectionLower.begin(), sectionLower.end(), sectionLower.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (sectionLower.find(filterLower) != std::string::npos) return true;

			return SettingsContains(entry.Keywords, filterLower);
		}

		bool CategoryHasMatches(ImGuiEditorLayer::SettingsCategory cat,
			const std::string& filterLower)
		{
			if (filterLower.empty()) return true;
			const int idx = static_cast<int>(cat);
			const char* categoryNameLower =
				(idx >= 0 && idx < IM_ARRAYSIZE(k_SettingsCategoryNamesLower))
					? k_SettingsCategoryNamesLower[idx]
					: "";
			for (const SettingsIndexEntry& entry : k_SettingsIndex) {
				if (entry.Category != cat) continue;
				if (SettingsEntryMatches(filterLower, entry, categoryNameLower)) return true;
			}
			return false;
		}

		bool SectionVisible(ImGuiEditorLayer::SettingsCategory cat,
			const char* section, const std::string& filterLower)
		{
			if (filterLower.empty()) return true;
			const int idx = static_cast<int>(cat);
			const char* categoryNameLower =
				(idx >= 0 && idx < IM_ARRAYSIZE(k_SettingsCategoryNamesLower))
					? k_SettingsCategoryNamesLower[idx]
					: "";
			for (const SettingsIndexEntry& entry : k_SettingsIndex) {
				if (entry.Category != cat) continue;
				if (std::strcmp(entry.Section, section) != 0) continue;
				return SettingsEntryMatches(filterLower, entry, categoryNameLower);
			}
			// Section not in k_SettingsIndex: fail closed so unlisted sections disappear under any filter (loudly visible).
			return false;
		}
	}

	void ImGuiEditorLayer::RenderLogPanel() {
		DrainPendingLogEntries();
		ImGui::Begin("Log");

		if (ImGui::Button("Clear")) {
			ClearLogEntries();
		}

		int infoCount = 0, warnCount = 0, errorCount = 0;
		for (const auto& entry : m_LogEntries) {
			if (entry.Level <= Log::Level::Info) infoCount++;
			else if (entry.Level == Log::Level::Warn) warnCount++;
			else errorCount++;
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		{
			uint64_t infoIcon = EditorIcons::Get("info", 16);
			if (infoIcon) {
				ImVec4 tint = m_ShowLogInfo ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
				if (ImGui::ImageButton("##FilterInfo",
					static_cast<ImTextureID>(static_cast<intptr_t>(infoIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogInfo = !m_ShowLogInfo;
				}
			}
			else if (ImGui::SmallButton(m_ShowLogInfo ? "[I]" : "( )")) {
				m_ShowLogInfo = !m_ShowLogInfo;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Info (%d)", infoCount);
		}

		ImGui::SameLine();

		{
			uint64_t warnIcon = EditorIcons::Get("warning", 16);
			ImVec4 tint = m_ShowLogWarn ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (warnIcon) {
				if (ImGui::ImageButton("##FilterWarn",
					static_cast<ImTextureID>(static_cast<intptr_t>(warnIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogWarn ? "W" : "(W)")) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warnings (%d)", warnCount);
		}

		ImGui::SameLine();

		{
			uint64_t errIcon = EditorIcons::Get("error", 16);
			ImVec4 tint = m_ShowLogError ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (errIcon) {
				if (ImGui::ImageButton("##FilterError",
					static_cast<ImTextureID>(static_cast<intptr_t>(errIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogError = !m_ShowLogError;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogError ? "E" : "(E)")) {
					m_ShowLogError = !m_ShowLogError;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Errors (%d)", errorCount);
		}

		ImGui::Separator();

		ImGui::BeginChild("##LogEntries", ImVec2(-1.0f, -1.0f), true,
			ImGuiWindowFlags_HorizontalScrollbar);

		const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
		for (const auto& entry : m_LogEntries) {
			if (!IsLogEntryVisible(entry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
				continue;
			}

			const std::string prefix = std::string(GetLogLevelPrefix(entry.Level));
			const ImVec4 levelColor = GetLogLevelColor(entry.Level);

			const bool hasInlineLink = !entry.SourceFilePath.empty()
				&& entry.SourceLinkEnd > entry.SourceLinkStart
				&& entry.SourceLinkEnd <= entry.Message.size();

			if (hasInlineLink) {
				const std::string fullText = prefix + entry.Message;
				RenderLogMessageWithInlineLink(
					fullText,
					entry.SourceLinkStart + prefix.size(),
					entry.SourceLinkEnd + prefix.size(),
					entry.SourceFilePath,
					entry.SourceLine,
					levelColor);
			}
			else {
				const std::string line = prefix + entry.Message;
				ImGui::PushStyleColor(ImGuiCol_Text, levelColor);
				ImGui::TextWrapped("%s", line.c_str());
				ImGui::PopStyleColor();
			}
		}

		if (wasAtBottom) {
			ImGui::SetScrollHereY(1.0f);
		}

		if (ImGui::BeginPopupContextWindow("##LogTextCtx")) {
			if (ImGui::MenuItem("Copy All Visible")) {
				std::string all;
				for (const auto& visibleEntry : m_LogEntries) {
					if (!IsLogEntryVisible(visibleEntry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
						continue;
					}
					all += std::string(GetLogLevelPrefix(visibleEntry.Level)) + visibleEntry.Message + "\n";
				}
				ImGui::SetClipboardText(all.c_str());
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderProjectPanel() {
		if (!m_AssetBrowserInitialized) {
			std::string assetsRoot;
			IndexProject* project = ProjectManager::GetCurrentProject();
			if (project) assetsRoot = project->AssetsDirectory;
			else assetsRoot = Path::Combine(Path::ExecutableDir(), "Assets");

			if (!Directory::Exists(assetsRoot)) {
				Directory::Create(assetsRoot);
			}

			m_AssetBrowser.Initialize(assetsRoot);
			m_AssetBrowserInitialized = true;
		}

		m_AssetBrowser.Render();
		if (m_AssetBrowser.TakeSelectionActivated() && !m_AssetBrowser.GetSelectedPath().empty()) {
			ClearEntitySelection();
		}
	}

	void ImGuiEditorLayer::ReportBuildProgress(float progress, std::string_view stage) {
		// Update progress and stage as a pair under the same lock so the UI
		// thread can never observe a stage that doesn't match the bar fraction.
		std::lock_guard<std::mutex> lk(m_BuildProgressMutex);
		m_BuildProgress = std::clamp(progress, 0.0f, 1.0f);
		m_BuildStage.assign(stage);
	}

	void ImGuiEditorLayer::ExecuteBuildAsync() {
		{
			std::lock_guard<std::mutex> lk(m_BuildProgressMutex);
			m_BuildProgress = 0.0f;
		}
		m_BuildSucceeded.store(true, std::memory_order_relaxed);
		ReportBuildProgress(0.0f, "Starting build");

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			IDX_ERROR_TAG("Build", "No project loaded.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "No project loaded");
			return;
		}

		// Save here on the UI thread — SceneSerializer/project::Save aren't thread-safe to call from the build worker.
		Scene* active = SceneManager::Get().GetActiveScene();
		if (active && active->IsDirty()) {
			std::string scenePath = project->GetSceneFilePath(active->GetName());
			SceneSerializer::SaveToFile(*active, scenePath);
			project->LastOpenedScene = active->GetName();
			project->Save();
			IDX_INFO_TAG("Build", "Saved current scene.");
		}

		m_BuildFuture = std::async(std::launch::async, [this]() {
			ExecuteBuild();
		});
	}

	void ImGuiEditorLayer::ExecuteBuild() {
		m_BuildStartTime = std::chrono::steady_clock::now();

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			IDX_ERROR_TAG("Build", "No project loaded.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			return;
		}

		m_BuildOutputDir = std::string(m_BuildOutputDirBuffer);
		if (m_BuildOutputDir.empty()) {
			IDX_ERROR_TAG("Build", "No output directory specified.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "No output directory");
			return;
		}

		IDX_INFO_TAG("Build", "Starting build for '{}'...", project->Name);
		ReportBuildProgress(0.05f, "Preparing build");

		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		auto outDir = std::filesystem::path(m_BuildOutputDir);
		const std::string buildConfiguration = IndexProject::GetActiveBuildConfiguration();

		if (std::filesystem::exists(project->CsprojPath)) {
			ReportBuildProgress(0.10f, "Compiling C# scripts");
			IDX_INFO_TAG("Build", "Compiling C# scripts...");
			Process::Result buildResult = Process::Run({
				"dotnet",
				"build",
				project->CsprojPath,
				"-c", buildConfiguration,
				"--nologo",
				"-v", "q",
				"-p:DefineConstants=" + IndexProject::BuildManagedDefineConstants("INDEX_BUILD")
			});
			if (!buildResult.Succeeded()) {
				IDX_ERROR_TAG("Build", "C# script compilation failed (exit code {}).", buildResult.ExitCode);
				if (!buildResult.Output.empty()) {
					IDX_ERROR_TAG("Build", "{}", buildResult.Output);
				}
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
				ReportBuildProgress(1.0f, "C# compilation failed");
				return;
			}
			IDX_INFO_TAG("Build", "C# scripts compiled.");
			ReportBuildProgress(0.30f, "C# scripts compiled");
		}

		const std::filesystem::path nativeProjectDir(project->NativeScriptsDir);
		const std::filesystem::path nativeCMakeLists = nativeProjectDir / "CMakeLists.txt";
		if (std::filesystem::exists(nativeCMakeLists)) {
			ReportBuildProgress(0.35f, "Compiling native scripts");
			IDX_INFO_TAG("Build", "Compiling native scripts ({} profile)...",
				IndexProject::BuildProfileToString(project->ActiveBuildProfile));
			const std::string profileCmakeArg = std::string("-DINDEX_BUILD_PROFILE=")
				+ ((project->ActiveBuildProfile == IndexProject::BuildProfile::Release) ? "RELEASE" : "DEVELOPMENT");
			const std::filesystem::path nativeBuildDir = nativeProjectDir / "build";
			// Configure (overrides editor's cached INDEX_BUILD_PROFILE so the
			// next-frame editor hot-reload also picks up the explicit value).
			Process::Result cfg = Process::Run({
				"cmake",
				"-B", nativeBuildDir.string(),
				"-S", nativeProjectDir.string(),
				"-DCMAKE_BUILD_TYPE=" + buildConfiguration,
				profileCmakeArg
			}, nativeProjectDir.string());
			if (!cfg.Succeeded()) {
				IDX_WARN_TAG("Build",
					"Native script CMake configure failed (exit code {}); shipping the editor's last-built native DLL.",
					cfg.ExitCode);
			} else {
				Process::Result bld = Process::Run({
					"cmake", "--build", nativeBuildDir.string(),
					"--config", buildConfiguration
				}, nativeProjectDir.string());
				if (!bld.Succeeded()) {
					IDX_WARN_TAG("Build",
						"Native script build failed (exit code {}); shipping the editor's last-built native DLL.",
						bld.ExitCode);
					if (!bld.Output.empty()) IDX_WARN_TAG("Build", "{}", bld.Output);
				} else {
					IDX_INFO_TAG("Build", "Native scripts compiled.");
				}
			}
		}

		try {
			std::filesystem::create_directories(outDir);
		}
		catch (const std::exception& e) {
			IDX_ERROR_TAG("Build", "Failed to create output directory: {}", e.what());
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "Output directory creation failed");
			return;
		}
		// Release ships an editor-free engine. The editor's own dev build of
		// Index-Engine.dll is compiled with INDEX_WITH_EDITOR=1 (it has to host
		// the editor); copying it verbatim would ship editor-only systems (the
		// PrefabTemplateCache .prefab watcher, ProjectManager hooks, ...). So for
		// Release we rebuild Index-Runtime in the Dist configuration — which sets
		// INDEX_WITH_EDITOR=0 via ApplyIndexEditorModuleDefine() in premake5.lua —
		// and stage the runtime exe + engine DLL from that output instead.
		// Development keeps copying the adjacent dev build for fast iteration.
		std::filesystem::path runtimeOutputDirectory = exeDir / ".." / "Index-Runtime";
		if (project->ActiveBuildProfile == IndexProject::BuildProfile::Release) {
			const std::filesystem::path engineRoot =
				exeDir.parent_path().parent_path().parent_path();
			const std::filesystem::path runtimeProject =
				engineRoot / "Index-Runtime" / "Index-Runtime.vcxproj";
			const std::string msbuildPath = IndexProject::GetMSBuildPath();
			if (msbuildPath.empty() || !std::filesystem::exists(runtimeProject)) {
				IDX_ERROR_TAG("Build",
					"Release profile needs a Dist (editor-free) runtime build, but the toolchain "
					"is unavailable (msbuild='{}', project='{}'). Aborting rather than shipping an "
					"editor-enabled engine.",
					msbuildPath, runtimeProject.string());
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
				ReportBuildProgress(1.0f, "Dist toolchain unavailable");
				return;
			}
			ReportBuildProgress(0.45f, "Building editor-free runtime (Dist)");
			IDX_INFO_TAG("Build", "Building Index-Runtime (Dist) so the shipped engine carries no editor systems...");
			const Process::Result distBuild = Process::Run({
				msbuildPath, runtimeProject.string(),
				"/p:Configuration=Dist", "/p:Platform=x64",
				"/m", "/v:minimal", "/nologo"
			}, engineRoot.string());
			if (!distBuild.Succeeded()) {
				IDX_ERROR_TAG("Build", "Dist runtime build failed (exit code {}).", distBuild.ExitCode);
				if (!distBuild.Output.empty()) IDX_ERROR_TAG("Build", "{}", distBuild.Output);
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
				ReportBuildProgress(1.0f, "Dist runtime build failed");
				return;
			}
			// Re-point staging at bin/Dist-<system>-<arch>/Index-Runtime, preserving
			// whatever system/arch suffix the editor's own bin dir uses.
			const std::string curCfgDir = exeDir.parent_path().filename().string();
			const std::string::size_type dash = curCfgDir.find('-');
			const std::string distCfgDir = "Dist" +
				(dash != std::string::npos ? curCfgDir.substr(dash) : std::string("-windows-x86_64"));
			runtimeOutputDirectory = engineRoot / "bin" / distCfgDir / "Index-Runtime";
			IDX_INFO_TAG("Build", "Editor-free runtime ready; staging from {}", runtimeOutputDirectory.string());
		}

		ReportBuildProgress(0.55f, "Copying runtime binaries");

		auto copyFile = [&](const std::filesystem::path& src, const std::filesystem::path& dest, const std::string& name) {
			try {
				if (!std::filesystem::exists(src)) {
					IDX_WARN_TAG("Build", "{} not found at {}", name, src.string());
					return;
				}
				auto canonical = std::filesystem::canonical(src);
				if (NeedsCopy(canonical, dest)) {
					std::filesystem::create_directories(dest.parent_path());
					std::filesystem::copy_file(canonical, dest, std::filesystem::copy_options::overwrite_existing);
					IDX_INFO_TAG("Build", "Copied {}", name);
				}
			}
			catch (const std::exception& e) {
				IDX_WARN_TAG("Build", "Failed to copy {}: {}", name, e.what());
			}
		};

		const std::string runtimeExecutableFilename = GetRuntimeExecutableFilename();
		const std::string outputExecutableStem = project->ExecutableName.empty()
			? project->Name : project->ExecutableName;
		copyFile(runtimeOutputDirectory / runtimeExecutableFilename,
			outDir / (outputExecutableStem + std::filesystem::path(runtimeExecutableFilename).extension().string()),
			"runtime executable");
		copyFile(runtimeOutputDirectory / GetEngineRuntimeFilename(),
			outDir / GetEngineRuntimeFilename(),
			GetEngineRuntimeFilename());

		for (std::string_view depName : { std::string_view{"GLFW"}, std::string_view{"Glad"} }) {
			const std::string filename = SharedLibraryFilename(depName);
			copyFile(runtimeOutputDirectory / filename, outDir / filename, filename);
		}
		{
			const std::string tracyFilename = SharedLibraryFilename("Tracy");
			const std::filesystem::path tracySource = runtimeOutputDirectory / tracyFilename;
			if (std::filesystem::exists(tracySource)) {
				copyFile(tracySource, outDir / tracyFilename, tracyFilename);
			}
		}

		auto scriptCoreDir = exeDir / ".." / "Index-ScriptCore";
		copyFile(scriptCoreDir / "Index-ScriptCore.dll", outDir / "Index-ScriptCore.dll", "Index-ScriptCore.dll");
		copyFile(scriptCoreDir / "Index-ScriptCore.runtimeconfig.json", outDir / "Index-ScriptCore.runtimeconfig.json", "ScriptCore config");
		copyFile(scriptCoreDir / "Index-ScriptCore.deps.json", outDir / "Index-ScriptCore.deps.json", "ScriptCore deps");
		{
			const std::string netHostFilename = GetNetHostRuntimeFilename();
			const std::filesystem::path netHostSource = exeDir / netHostFilename;
			if (std::filesystem::exists(netHostSource)) {
				copyFile(netHostSource, outDir / netHostFilename, netHostFilename);
			}
		}

		try {
			std::filesystem::copy_file(project->ProjectFilePath, outDir / "project.json",
				std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::exception& e) {
			IDX_WARN_TAG("Build", "Failed to copy project.json: {}", e.what());
		}

		ReportBuildProgress(0.75f, "Copying project assets");
		if (std::filesystem::exists(project->AssetsDirectory)) {
			int updatedFiles = CopyDirIncremental(project->AssetsDirectory, outDir / "Assets");
			IDX_INFO_TAG("Build", "Assets: {} file(s) updated", updatedFiles);
			ReportBuildProgress(0.78f, "Optimizing scene assets");
			int convertedFiles = ConvertSerializedAssetsInDirectory(outDir / "Assets", SceneSerializationFormat::Binary);
			IDX_INFO_TAG("Build", "Serialized assets optimized to binary: {} file(s)", convertedFiles);
		}

		{
			std::string indexAssetsSrc;
			if (std::filesystem::exists(project->IndexAssetsDirectory)) {
				indexAssetsSrc = project->IndexAssetsDirectory;
			}
			else {
				indexAssetsSrc = Path::ResolveIndexAssets("");
			}

			if (!indexAssetsSrc.empty() && std::filesystem::exists(indexAssetsSrc)) {
				const std::vector<std::filesystem::path> excluded{
					std::filesystem::path("Textures") / "Editor",
				};
				int updatedFiles = CopyDirIncrementalExcluding(
					indexAssetsSrc, outDir / "IndexAssets", excluded);
				IDX_INFO_TAG("Build", "IndexAssets: {} file(s) updated", updatedFiles);
			}
			else {
				IDX_WARN_TAG("Build", "IndexAssets not found - build may be incomplete");
			}
		}

		{
			auto userBinDir = std::filesystem::path(project->GetUserAssemblyOutputPath()).parent_path();
			if (std::filesystem::exists(userBinDir)) {
				auto destBinDir = outDir / "bin" / buildConfiguration;
				int copied = 0;
				for (const auto& entry : std::filesystem::directory_iterator(userBinDir)) {
					if (!entry.is_regular_file()) continue;
					auto ext = entry.path().extension().string();
					if (ext == ".dll" || ext == ".json") {
						copyFile(entry.path(), destBinDir / entry.path().filename(), entry.path().filename().string());
						copied++;
					}
				}
				IDX_INFO_TAG("Build", "User assemblies: {} file(s) copied", copied);
			}
		}

		{
			std::string nativeDll = project->GetNativeDllPath();
			if (std::filesystem::exists(nativeDll)) {
				const std::filesystem::path nativeLibraryPath(nativeDll);
				copyFile(nativeDll,
					outDir / "NativeScripts" / "build" / buildConfiguration / nativeLibraryPath.filename(),
					"native script DLL");
			}
		}

		// Native package DLLs: MSBuild copies the C# Pkg.*.dll but not the native sibling; without this
		// block DllImport resolves into a process with no native module and components never register.
		{
			const std::filesystem::path packageSearchRoot = exeDir / "..";
			int copiedPackages = 0;
			for (const std::string& packageName : project->Packages) {
				if (packageName.empty()) continue;
				const std::string nativeFolderName = "Pkg." + packageName + ".Native";
				const std::string nativeFileName = nativeFolderName + ".dll";
				const std::filesystem::path src = packageSearchRoot / nativeFolderName / nativeFileName;
				const std::filesystem::path dst = outDir / "Packages" / nativeFolderName / nativeFileName;
				if (!std::filesystem::exists(src)) {
					// Package may be managed-only (csharp layer with no
					// engine_core/standalone_cpp). That's a valid manifest, so
					// surface as INFO rather than WARN.
					IDX_INFO_TAG("Build",
						"Package '{}' has no native DLL at '{}' — skipping (managed-only?).",
						packageName, src.string());
					continue;
				}
				copyFile(src, dst, nativeFileName);
				copiedPackages++;

				// Best-effort PDB copy so a native crash inside a shipped
				// package surfaces with line numbers when the user keeps
				// debug symbols around.
				const std::filesystem::path pdbSrc = packageSearchRoot / nativeFolderName /
					(nativeFolderName + ".pdb");
				if (std::filesystem::exists(pdbSrc)) {
					copyFile(pdbSrc, dst.parent_path() / pdbSrc.filename(), pdbSrc.filename().string());
				}
			}
			if (copiedPackages > 0) {
				IDX_INFO_TAG("Build", "Native packages: {} DLL(s) staged under Packages/.", copiedPackages);
			}
		}

#ifdef IDX_PLATFORM_WINDOWS
		if (!project->AppIconPath.empty()) {
			std::filesystem::path iconPath(project->AppIconPath);
			if (!iconPath.is_absolute()) {
				iconPath = std::filesystem::path(project->RootDirectory) / project->AppIconPath;
			}
			const std::string iconExt = iconPath.has_extension()
				? iconPath.extension().string() : std::string{};
			std::string iconExtLower = iconExt;
			std::transform(iconExtLower.begin(), iconExtLower.end(),
				iconExtLower.begin(), [](unsigned char c) { return std::tolower(c); });
			const std::filesystem::path embedTargetExe = outDir
				/ (outputExecutableStem + std::filesystem::path(runtimeExecutableFilename).extension().string());
			if (!std::filesystem::exists(iconPath)) {
				IDX_WARN_TAG("Build", "AppIcon not found at {}", iconPath.string());
			}
			else if (!std::filesystem::exists(embedTargetExe)) {
				IDX_WARN_TAG("Build", "Cannot embed icon — built executable not found at {}",
					embedTargetExe.string());
			}
			else {
				std::vector<std::uint8_t> icoBytes;
				std::string buildErr;
				if (iconExtLower == ".ico") {
					buildErr = ReadIcoFileBytes(iconPath, icoBytes);
				}
				else {
					buildErr = BuildIcoBytesFromImage(iconPath, icoBytes);
				}
				if (!buildErr.empty()) {
					IDX_WARN_TAG("Build", "Failed to prepare icon ({}): {}",
						iconPath.filename().string(), buildErr);
				}
				else {
					const std::string err = EmbedIconIntoExecutable(
						embedTargetExe.string(), icoBytes);
					if (err.empty()) {
						IDX_INFO_TAG("Build", "Embedded icon: {}", iconPath.filename().string());
					}
					else {
						IDX_WARN_TAG("Build", "Failed to embed icon: {}", err);
					}
				}
			}
		}
#endif

		ReportBuildProgress(0.95f, "Finalizing");
		float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_BuildStartTime).count();
		IDX_INFO_TAG("Build", "Build completed in {:.2f}s -> {}", elapsed, m_BuildOutputDir);
		ReportBuildProgress(1.0f, "Build complete");

#ifdef IDX_PLATFORM_WINDOWS
		// Explorer popup runs on the UI thread once the future resolves, not on the build worker (ShellExecute touches OS UI state).
#endif
	}

	void ImGuiEditorLayer::RenderBuildPanel() {
		if (!m_ShowBuildPanel) return;

		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		ImGui::Begin("Build", &m_ShowBuildPanel);

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			AssetRegistry::Sync();
			std::vector<std::string> diskScenes;
			diskScenes.reserve(m_BuildSceneList.size() + 4);
			for (const AssetRegistry::Record& record : AssetRegistry::FindAll(AssetKind::Scene)) {
				diskScenes.push_back(std::filesystem::path(record.Path).stem().string());
			}

			// Drop entries whose .scene file no longer exists.
			m_BuildSceneList.erase(
				std::remove_if(m_BuildSceneList.begin(), m_BuildSceneList.end(),
					[&](const std::string& s) {
						return std::find(diskScenes.begin(), diskScenes.end(), s) == diskScenes.end();
					}),
				m_BuildSceneList.end());

			// Append entries the user has not seen yet (new on disk).
			for (const std::string& stem : diskScenes) {
				if (std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), stem) == m_BuildSceneList.end()) {
					m_BuildSceneList.push_back(stem);
				}
			}

			// Pin StartupScene to position [0] so the [Startup] tag and
			// the runtime's first-loaded scene stay in sync with the
			// project file's StartupScene field.
			if (!project->StartupScene.empty()) {
				auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), project->StartupScene);
				if (it != m_BuildSceneList.end()) {
					if (it != m_BuildSceneList.begin()) {
						std::string startupScene = *it;
						m_BuildSceneList.erase(it);
						m_BuildSceneList.insert(m_BuildSceneList.begin(), startupScene);
					}
				}
				else if (!m_BuildSceneList.empty()) {
					// StartupScene's .scene no longer exists on disk (renamed or
					// deleted) — repair it to the first available scene so the build
					// doesn't ship a dangling startup reference that loads blank.
					// Self-limiting: once repaired, StartupScene is in the list and
					// this branch won't run again.
					project->StartupScene = m_BuildSceneList.front();
					project->Save();
				}
			}

			if (ImGui::CollapsingHeader("Target Platform", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);

				std::vector<IndexBuildProfile> diskProfiles;
				if (!project->GetBuildProfilesDirectory().empty()) {
					std::error_code ec;
					const std::string dir = project->GetBuildProfilesDirectory();
					if (std::filesystem::exists(dir)) {
						const std::string ext(IndexBuildProfile::FileExtension);
						for (const auto& entry : std::filesystem::directory_iterator(
								 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
							if (ec) { ec.clear(); continue; }
							if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
							if (entry.path().extension().string() != ext) continue;
							diskProfiles.push_back(IndexBuildProfile::Load(entry.path().string()));
						}
						std::sort(diskProfiles.begin(), diskProfiles.end(),
							[](const IndexBuildProfile& a, const IndexBuildProfile& b) {
								return a.Name < b.Name;
							});
					}
				}

				// Clear stale ActiveBuildProfileName if the file was deleted
				// externally — keeps the UI honest after a manual delete.
				if (!project->ActiveBuildProfileName.empty()) {
					bool stillExists = false;
					for (const auto& p : diskProfiles) {
						if (p.Name == project->ActiveBuildProfileName) { stillExists = true; break; }
					}
					if (!stillExists) {
						project->ActiveBuildProfileName.clear();
						project->Save();
					}
				}

				// Default to first Windows profile; leave empty if none exists (don't silently target Linux).
				if (project->ActiveBuildProfileName.empty()) {
					for (const auto& p : diskProfiles) {
						if (p.Platform == BuildPlatform::Windows) {
							project->ActiveBuildProfileName = p.Name;
							project->Save();
							break;
						}
					}
				}

				ImGui::TextUnformatted("Active Profile:");
				ImGui::SetNextItemWidth(-1);
				const char* previewLabel = project->ActiveBuildProfileName.empty()
					? "<none>" : project->ActiveBuildProfileName.c_str();
				if (ImGui::BeginCombo("##ActiveBuildProfile", previewLabel)) {
					if (ImGui::Selectable("<none>", project->ActiveBuildProfileName.empty())) {
						if (!project->ActiveBuildProfileName.empty()) {
							project->ActiveBuildProfileName.clear();
							project->Save();
						}
					}
					for (const auto& p : diskProfiles) {
						const bool selected = (project->ActiveBuildProfileName == p.Name);
						if (ImGui::Selectable(p.Name.c_str(), selected)) {
							if (project->ActiveBuildProfileName != p.Name) {
								project->ActiveBuildProfileName = p.Name;
								project->Save();
							}
						}
					}
					ImGui::EndCombo();
				}

				ImGui::SameLine();
				if (ImGui::SmallButton("Manage Profiles…")) {
					m_ShowBuildProfilesPanel = true;
				}

				// Echo the active profile's platform + render backend below
				// the combo so the user sees what Build will actually target
				// without flipping over to the Build Profiles window.
				if (!project->ActiveBuildProfileName.empty()) {
					const IndexBuildProfile* active = nullptr;
					for (const auto& p : diskProfiles) {
						if (p.Name == project->ActiveBuildProfileName) { active = &p; break; }
					}
					if (active) {
						ImGui::Text("Platform: %s",
							IndexBuildProfile::PlatformDisplayName(active->Platform));
						ImGui::Text("Rendering API: %s",
							IndexProject::RenderBackendToString(active->RenderBackend));
						if (!BuildPlatformSupport::IsAvailable(active->Platform)) {
							ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
							ImGui::TextWrapped("[!] %s",
								BuildPlatformSupport::UnavailableReason(active->Platform).c_str());
							ImGui::PopStyleColor();
						}
					}
				}
				else {
					ImGui::TextDisabled("No active profile — pick one above or click 'Manage Profiles…'.");
				}

				ImGui::Unindent(8);
			}

			ImGui::Spacing();

			if (ImGui::CollapsingHeader("Scene List", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (m_BuildSceneList.empty()) {
					ImGui::TextDisabled("No scenes found in Assets/");
				}
				else {
					for (int i = 0; i < static_cast<int>(m_BuildSceneList.size()); i++) {
						ImGui::PushID(i);
						bool isStartup = (i == 0);

						if (isStartup) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "[Startup]");
						else ImGui::TextDisabled("[%d]", i);

						ImGui::SameLine();
						const std::string sceneItemId = std::to_string(i);
						ImGuiUtils::SelectableEllipsis(m_BuildSceneList[i], sceneItemId.c_str());

						if (ImGui::BeginDragDropSource()) {
							ImGui::SetDragDropPayload("SCENE_LIST_ITEM", &i, sizeof(int));
							ImGui::Text("Move: %s", m_BuildSceneList[i].c_str());
							ImGui::EndDragDropSource();
						}
						if (ImGui::BeginDragDropTarget()) {
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_LIST_ITEM")) {
								int srcIndex = *static_cast<const int*>(payload->Data);
								if (srcIndex != i) {
									std::string moved = m_BuildSceneList[srcIndex];
									m_BuildSceneList.erase(m_BuildSceneList.begin() + srcIndex);
									int insertAt = (srcIndex < i) ? i - 1 : i;
									m_BuildSceneList.insert(m_BuildSceneList.begin() + insertAt, moved);
									if (!m_BuildSceneList.empty()) {
										project->StartupScene = m_BuildSceneList[0];
										project->Save();
									}
								}
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::PopID();
					}
				}

				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						// Use 1-arg ctor: payload includes trailing '\0' in DataSize, so (data,DataSize) embeds it and breaks extension comparison.
						std::string droppedPath(static_cast<const char*>(payload->Data));
						if (std::filesystem::path(droppedPath).extension() == ".scene") {
							std::string sceneName = std::filesystem::path(droppedPath).stem().string();
							auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), sceneName);
							if (it == m_BuildSceneList.end()) {
								m_BuildSceneList.push_back(sceneName);
							}
						}
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::Unindent(8);
			}

			ImGui::Spacing();

			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (ImGui::Button("Open Project Settings")) {
					m_ShowProjectSettings = true;
				}

				ImGui::Spacing();

				ImGui::TextUnformatted("Build Profile:");
				ImGui::SetNextItemWidth(-1);
				const char* profileItems[] = { "Development", "Release" };
				int profileIdx = (project->ActiveBuildProfile == IndexProject::BuildProfile::Release) ? 1 : 0;
				if (ImGui::Combo("##BuildProfile", &profileIdx, profileItems, IM_ARRAYSIZE(profileItems))) {
					const auto newProfile = (profileIdx == 1)
						? IndexProject::BuildProfile::Release
						: IndexProject::BuildProfile::Development;
					if (newProfile != project->ActiveBuildProfile) {
						project->ActiveBuildProfile = newProfile;
						// Auto-flip overlay defaults to match the new profile.
						// Release = both off (ship-clean). Development = both on.
						const bool isDev = (newProfile == IndexProject::BuildProfile::Development);
						project->ShowRuntimeStats = isDev;
						project->ShowRuntimeLogs  = isDev;
						project->Save();
					}
				}

				ImGui::Spacing();

				ImGui::TextUnformatted("Custom Defines:");
				const float addBtnWidth = ImGui::CalcTextSize("Add").x
					+ ImGui::GetStyle().FramePadding.x * 2.0f;
				const float availForInput = ImGui::GetContentRegionAvail().x
					- addBtnWidth - ImGui::GetStyle().ItemInnerSpacing.x;
				ImGui::SetNextItemWidth(availForInput);
				const bool entryEnter = ImGui::InputTextWithHint("##NewCustomDefine",
					"Add a symbol (e.g. STEAM_BUILD)…",
					m_CustomDefineEntryBuffer, sizeof(m_CustomDefineEntryBuffer),
					ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				bool addClicked = ImGui::Button("Add##CustomDefine") || entryEnter;
				const bool entryReady = m_CustomDefineEntryBuffer[0] != '\0';
				if (addClicked && entryReady) {
					std::string newDef(m_CustomDefineEntryBuffer);
					// Trim whitespace + reject duplicates / empty strings.
					while (!newDef.empty() && std::isspace(static_cast<unsigned char>(newDef.back()))) newDef.pop_back();
					std::size_t firstNonSpace = 0;
					while (firstNonSpace < newDef.size()
						&& std::isspace(static_cast<unsigned char>(newDef[firstNonSpace]))) ++firstNonSpace;
					newDef.erase(0, firstNonSpace);
					if (!newDef.empty()
						&& std::find(project->CustomDefines.begin(), project->CustomDefines.end(), newDef) == project->CustomDefines.end()) {
						project->CustomDefines.push_back(std::move(newDef));
						project->Save();
					}
					m_CustomDefineEntryBuffer[0] = '\0';
				}

				if (project->CustomDefines.empty()) {
					ImGui::TextDisabled("(no custom defines)");
				} else {
					int removeIdx = -1;
					for (int i = 0; i < static_cast<int>(project->CustomDefines.size()); ++i) {
						ImGui::PushID(i);
						ImGui::Bullet();
						ImGui::TextUnformatted(project->CustomDefines[i].c_str());
						ImGui::SameLine();
						if (ImGui::SmallButton("x")) removeIdx = i;
						ImGui::PopID();
					}
					if (removeIdx >= 0) {
						project->CustomDefines.erase(project->CustomDefines.begin() + removeIdx);
						project->Save();
					}
				}

				ImGui::Spacing();
				ImGui::Text("Output Directory:");
				ImGui::SetNextItemWidth(-1);
				ImGui::InputText("##BuildOutputDir", m_BuildOutputDirBuffer, sizeof(m_BuildOutputDirBuffer));
				ImGui::Unindent(8);
			}
		}

		ImGui::Spacing();

		IndexBuildProfile activeProfile;
		bool activeProfileResolved = false;
		if (project && !project->ActiveBuildProfileName.empty()) {
			const std::string profilePath =
				(std::filesystem::path(project->GetBuildProfilesDirectory()) /
				 (project->ActiveBuildProfileName + std::string(IndexBuildProfile::FileExtension))).string();
			if (std::filesystem::exists(profilePath)) {
				activeProfile = IndexBuildProfile::Load(profilePath);
				activeProfileResolved = true;
			}
		}

		const bool platformAvailable = activeProfileResolved
			&& BuildPlatformSupport::IsAvailable(activeProfile.Platform);

		bool canBuild = project && !Application::GetIsPlaying() && m_BuildState == 0
			&& activeProfileResolved && platformAvailable;
		if (!canBuild) ImGui::BeginDisabled();

		float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (ImGui::Button("Build", ImVec2(halfWidth, 0))) {
			if (activeProfileResolved && project) {
				project->ActiveRenderBackend = activeProfile.RenderBackend;
				project->Save();
			}
			m_BuildState = 1;
			m_BuildAndPlay = false;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}
		ImGui::SameLine();
		if (ImGui::Button("Build and Play", ImVec2(-1, 0))) {
			if (activeProfileResolved && project) {
				project->ActiveRenderBackend = activeProfile.RenderBackend;
				project->Save();
			}
			m_BuildState = 1;
			m_BuildAndPlay = true;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}

		if (!canBuild) ImGui::EndDisabled();

		// Explain why the button is disabled — the editor's existing
		// disabled-button convention has no tooltip, so a one-liner below
		// keeps users from guessing.
		if (project && !Application::GetIsPlaying() && m_BuildState == 0) {
			if (!activeProfileResolved) {
				ImGui::TextDisabled("Select an active build profile to enable Build.");
			}
			else if (!platformAvailable) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
				ImGui::TextWrapped("[!] Cannot build: %s",
					BuildPlatformSupport::UnavailableReason(activeProfile.Platform).c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderBuildProfilesPanel() {
		if (!m_ShowBuildProfilesPanel) return;
		if (!m_BuildProfilesPanelInitialized) {
			m_BuildProfilesPanel.Initialize();
			m_BuildProfilesPanelInitialized = true;
		}
		m_BuildProfilesPanel.Render(&m_ShowBuildProfilesPanel);
	}

	void ImGuiEditorLayer::RenderProjectSettingsPanel() {
		if (!m_ShowProjectSettings) return;

		ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (!ImGui::Begin("Project Settings", &m_ShowProjectSettings)) {
			ImGui::End();
			return;
		}

		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("No project loaded");
			ImGui::End();
			return;
		}

		constexpr const char* k_CategoryLabels[] = {
			"Display",
			"Graphics",
			"Branding",
			"Build",
			"Editor",
			"Systems",
		};
		static_assert(IM_ARRAYSIZE(k_CategoryLabels)
			== static_cast<int>(SettingsCategory::Systems) + 1,
			"k_CategoryLabels out of sync with SettingsCategory enum");

		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##SettingsSearch", "Search settings...",
			m_ProjectSettingsSearchBuffer, sizeof(m_ProjectSettingsSearchBuffer));

		std::string filterLower(m_ProjectSettingsSearchBuffer);
		std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const bool filterActive = !filterLower.empty();

		if (filterActive && !CategoryHasMatches(m_SelectedSettingsCategory, filterLower)) {
			for (int i = 0; i < IM_ARRAYSIZE(k_CategoryLabels); ++i) {
				auto cat = static_cast<SettingsCategory>(i);
				if (CategoryHasMatches(cat, filterLower)) {
					m_SelectedSettingsCategory = cat;
					break;
				}
			}
		}

		ImGui::Separator();

		const float listColumnWidth = ImGui::GetContentRegionAvail().x * 0.22f;
		const float listW = std::max(160.0f, listColumnWidth);

		bool anyVisible = false;
		ImGui::BeginChild("##SettingsNav", ImVec2(listW, 0), /*border*/ true);
		for (int i = 0; i < IM_ARRAYSIZE(k_CategoryLabels); ++i) {
			auto cat = static_cast<SettingsCategory>(i);
			if (filterActive && !CategoryHasMatches(cat, filterLower)) continue;
			anyVisible = true;
			const bool selected = static_cast<int>(m_SelectedSettingsCategory) == i;
			if (ImGui::Selectable(k_CategoryLabels[i], selected)) {
				m_SelectedSettingsCategory = cat;
			}
		}
		if (filterActive && !anyVisible) {
			ImGui::TextDisabled("No matches");
		}
		ImGui::EndChild();

		ImGui::SameLine();

		bool changed = false;
		bool globalSystemsChanged = false;

		ImGui::BeginChild("##SettingsContent", ImVec2(0, 0));
		if (filterActive && !anyVisible) {
			ImGui::TextDisabled("No settings match \"%s\"", m_ProjectSettingsSearchBuffer);
		}
		else {
			switch (m_SelectedSettingsCategory) {
				case SettingsCategory::Display:  RenderSettings_Display(*project, changed, filterLower);  break;
				case SettingsCategory::Graphics: RenderSettings_Graphics(*project, changed, filterLower); break;
				case SettingsCategory::Branding: RenderSettings_Branding(*project, changed, filterLower); break;
				case SettingsCategory::Build:    RenderSettings_Build(*project, changed, filterLower);    break;
				case SettingsCategory::Editor:   RenderSettings_Editor(*project, changed, filterLower);   break;
				case SettingsCategory::Systems:
					RenderSettings_Systems(*project, changed, globalSystemsChanged, filterLower);
					break;
			}
		}
		ImGui::EndChild();

		if (changed) {
			project->Save();
			if (globalSystemsChanged) {
				std::vector<std::string> activeGlobalSystems;
				for (const auto& registration : project->GlobalSystems) {
					if (registration.Active && !registration.ClassName.empty()) {
						activeGlobalSystems.push_back(registration.ClassName);
					}
				}
				ScriptEngine::ShutdownGlobalSystems();
				ScriptEngine::InitializeGlobalSystems(activeGlobalSystems);
			}
		}

		ReferencePicker::RenderPopup();

		ImGui::End();
	}

	void ImGuiEditorLayer::RenderSettings_Display(IndexProject& project, bool& changed,
		const std::string& filterLower)
	{
		if (SectionVisible(SettingsCategory::Display, "Window", filterLower)) {
			if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
			if (ImGui::CollapsingHeader("Window", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);

				ImGui::TextDisabled("Startup");
				ImGui::Separator();

				changed |= ImGui::InputInt("Width", &project.BuildWidth);
				changed |= ImGui::InputInt("Height", &project.BuildHeight);
				if (project.BuildWidth < 320) project.BuildWidth = 320;
				if (project.BuildHeight < 240) project.BuildHeight = 240;

				changed |= ImGui::Checkbox("Fullscreen", &project.BuildFullscreen);

				struct FullscreenModeOption {
					const char* Label;
					FullscreenMode Mode;
				};
				static constexpr std::array<FullscreenModeOption, 3> k_FullscreenModeOptions = {{
					{ "Exclusive",          FullscreenMode::Exclusive },
					{ "Borderless Windowed", FullscreenMode::BorderlessWindowed },
					{ "Maximized",          FullscreenMode::Maximized },
				}};
				const bool fullscreenEnabled = project.BuildFullscreen;
				if (!fullscreenEnabled) ImGui::BeginDisabled();
				const char* currentLabel = "Exclusive";
				for (const auto& opt : k_FullscreenModeOptions) {
					if (opt.Mode == project.BuildFullscreenMode) {
						currentLabel = opt.Label;
						break;
					}
				}
				ImGui::SetNextItemWidth(220.0f);
				if (ImGui::BeginCombo("Fullscreen Mode", currentLabel)) {
					for (const auto& opt : k_FullscreenModeOptions) {
						const bool selected = (opt.Mode == project.BuildFullscreenMode);
						if (ImGui::Selectable(opt.Label, selected)) {
							project.BuildFullscreenMode = opt.Mode;
							changed = true;
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (!fullscreenEnabled) ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip(
						"How the runtime realises fullscreen at launch:\n"
						"  Exclusive — true fullscreen, takes over the display.\n"
						"  Borderless Windowed — undecorated window sized to the\n"
						"      monitor; Alt-Tab is instant.\n"
						"  Maximized — decorated, maximised window.\n"
						"\n"
						"Disabled when 'Fullscreen' is off — the value is\n"
						"ignored in windowed launches.");
				}

				ImGui::Spacing();
				ImGui::TextDisabled("Runtime");
				ImGui::Separator();

				changed |= ImGui::Checkbox("Resizable", &project.BuildResizable);
				changed |= ImGui::Checkbox("Run in background", &project.BuildRunInBackground);

				ImGui::Spacing();
				ImGui::TextDisabled("Resize Limits (0 = unbounded)");
				changed |= ImGui::InputInt("Min Width",  &project.BuildMinWidth);
				changed |= ImGui::InputInt("Min Height", &project.BuildMinHeight);
				changed |= ImGui::InputInt("Max Width",  &project.BuildMaxWidth);
				changed |= ImGui::InputInt("Max Height", &project.BuildMaxHeight);
				if (project.BuildMinWidth  < 0) project.BuildMinWidth  = 0;
				if (project.BuildMinHeight < 0) project.BuildMinHeight = 0;
				if (project.BuildMaxWidth  < 0) project.BuildMaxWidth  = 0;
				if (project.BuildMaxHeight < 0) project.BuildMaxHeight = 0;
				if (project.BuildMaxWidth  > 0 && project.BuildMinWidth  > project.BuildMaxWidth)
					ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Min Width > Max Width");
				if (project.BuildMaxHeight > 0 && project.BuildMinHeight > project.BuildMaxHeight)
					ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Min Height > Max Height");

				ImGui::Spacing();
				int aspectIndex = AspectRatioPresetIndexFromLabel(project.BuildAspect);
				ImGui::SetNextItemWidth(220.0f);
				if (ImGui::BeginCombo("Aspect Ratio", k_AspectRatioPresets[aspectIndex].Label)) {
					for (int i = 0; i < static_cast<int>(k_AspectRatioPresets.size()); ++i) {
						const bool selected = (i == aspectIndex);
						if (ImGui::Selectable(k_AspectRatioPresets[i].Label, selected)) {
							project.BuildAspect = k_AspectRatioPresets[i].Label;
							changed = true;
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Locks the shipped game to this aspect ratio.\n"
						"When the window's aspect doesn't match, the renderer\n"
						"adds black bars (letterbox/pillarbox) so the game\n"
						"always appears at the chosen aspect.\n"
						"\n"
						"Independent of the editor's Game View aspect.\n"
						"Select 'Free Aspect' to disable and fill the full window.");
				}
				ImGui::Unindent(8);
			}
		}

		if (SectionVisible(SettingsCategory::Display, "UI Scaling", filterLower)) {
			if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
			if (ImGui::CollapsingHeader("UI Scaling", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				changed |= ImGui::InputInt("Reference Width##UIRef", &project.UIReferenceWidth);
				changed |= ImGui::InputInt("Reference Height##UIRef", &project.UIReferenceHeight);
				if (project.UIReferenceWidth  < 1) project.UIReferenceWidth  = 1;
				if (project.UIReferenceHeight < 1) project.UIReferenceHeight = 1;
				changed |= ImGui::SliderFloat("Match Width / Height", &project.UIScaleMatch, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"0 = scale UI by window WIDTH only.\n"
						"1 = scale UI by window HEIGHT only.\n"
						"0.5 = balanced (geometric mean).\n"
						"\n"
						"Move toward 1 if your UI hugs the top/bottom edges,\n"
						"toward 0 if it hugs the left/right edges.");
				}
				ImGui::Unindent(8);
			}
		}
	}

	void ImGuiEditorLayer::RenderSettings_Graphics(IndexProject& project, bool& changed,
		const std::string& filterLower)
	{
		// Static state for the render-backend restart modal. Function-static
		// is safe — only one Graphics tab can be visible at once.
		static bool s_RenderBackendChangePopup = false;
		static IndexProject::RenderBackend s_PendingRenderBackend = IndexProject::RenderBackend::Auto;

		if (SectionVisible(SettingsCategory::Graphics, "Render Backend", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Render Backend", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo("Rendering API", RenderBackendLabel(project.ActiveRenderBackend))) {
				for (const RenderBackendOption& option : k_RenderBackendOptions) {
					const bool selected = option.Backend == project.ActiveRenderBackend;
					const bool supported = IsRenderBackendSupportedOnHost(option.Backend);
					if (!supported) ImGui::BeginDisabled();
					if (ImGui::Selectable(option.Label, selected)) {
						if (supported && option.Backend != project.ActiveRenderBackend) {
							s_PendingRenderBackend = option.Backend;
							s_RenderBackendChangePopup = true;
						}
					}
					if (!supported) ImGui::EndDisabled();
					if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
						ImGui::SetTooltip("%s", RenderBackendUnsupportedReason(option.Backend));
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Which native API Dawn (WebGPU) uses at runtime. The\n"
					"choice is applied when the engine requests its GPU\n"
					"adapter at startup, so the change only takes effect\n"
					"after restarting the editor / built game.\n"
					"\n"
					"  • Auto: Dawn picks the most capable backend for\n"
					"    the host platform (D3D12 on Windows, Metal on\n"
					"    macOS, Vulkan on Linux).\n"
					"  • Vulkan / D3D11 / D3D12 / OpenGL: explicit backend.\n"
					"    Selecting one not supported by the host platform\n"
					"    (e.g. D3D12 on Linux) makes adapter-request fail\n"
					"    at startup; the log line says exactly which\n"
					"    backend was requested.");
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Render Backend")

		if (SectionVisible(SettingsCategory::Graphics, "Post-Processing", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Post-Processing", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Enable Post-Processing", &project.EnablePostProcessing);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Global kill switch for the post-processing pipeline.\n"
					"When off, the renderer skips the PP pass entirely and\n"
					"writes the scene straight to the final target.\n"
					"Per-effect toggles still live on the camera's\n"
					"PostProcessing2DComponent.");
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Post-Processing")

		if (SectionVisible(SettingsCategory::Graphics, "Default Font", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Default Font", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_DefaultFontPickerKey = "ProjectSettings.DefaultFont";
			if (auto pending = ReferencePicker::ConsumeSelection(k_DefaultFontPickerKey); pending) {
				uint64_t pickedId = ParsePickerAssetId(*pending);
				project.DefaultFontAssetId = pickedId != 0 ? pickedId : k_DefaultFontAssetId;
				changed = true;
			}

			bool fontMissing = false;
			std::string fontSecondary;
			std::string fontDisplay = ReferencePicker::ResolveAssetDisplay(
				project.DefaultFontAssetId, AssetKind::Font, fontMissing, &fontSecondary);
			ImGui::TextUnformatted("Default Font");
			ImGui::SameLine(130.0f);
			ImGui::PushID("ProjectDefaultFont");
			if (ImGui::Button(fontDisplay.c_str(), ImVec2(260.0f, 0.0f))) {
				ReferencePicker::OpenForFieldKey(k_DefaultFontPickerKey, "Select Default Font",
					ReferencePicker::CollectAssetsByKind(AssetKind::Font),
					ReferencePicker::Style::Plain);
			}
			if (ImGui::IsItemHovered() && !fontSecondary.empty()) {
				ImGui::SetTooltip("%s", fontSecondary.c_str());
			}
			ImGui::PopID();
			ImGui::SameLine();
			if (ImGui::Button("Reset##ProjectDefaultFont")) {
				project.DefaultFontAssetId = k_DefaultFontAssetId;
				changed = true;
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Default Font")

		// NoSavedSettings prevents a stale imgui.ini Pos= from pinning the dialog off-center.
		if (s_RenderBackendChangePopup) {
			ImGui::OpenPopup("Restart Editor");
			s_RenderBackendChangePopup = false;
		}
		ImGuiUtils::CenterNextModal();
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		if (ImGui::BeginPopupModal("Restart Editor", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextWrapped(
				"Changing the rendering API from %s to %s requires the editor to restart.",
				RenderBackendLabel(project.ActiveRenderBackend),
				RenderBackendLabel(s_PendingRenderBackend));
			ImGui::Spacing();
			if (ImGui::Button("Restart Now", ImVec2(120.0f, 0.0f))) {
				project.ActiveRenderBackend = s_PendingRenderBackend;
				project.Save();
				ImGui::CloseCurrentPopup();
				Application::Reload();
			}
			ImGui::SameLine();
			if (ImGui::Button("Save For Later", ImVec2(120.0f, 0.0f))) {
				project.ActiveRenderBackend = s_PendingRenderBackend;
				changed = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void ImGuiEditorLayer::RenderSettings_Branding(IndexProject& project, bool& changed,
		const std::string& filterLower)
	{
		if (SectionVisible(SettingsCategory::Branding, "Splash Screen", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Splash Screen", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_SplashPickerKey = "ProjectSettings.SplashImage";

			if (auto pending = ReferencePicker::ConsumeSelection(k_SplashPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project.SplashScreen.ImagePath.clear();
					changed = true;
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						project.SplashScreen.ImagePath =
							SplashAssetResolve::NormalizeForStorage(absPath, &project);
						changed = true;
					}
				}
			}

			changed |= ImGui::Checkbox("Enabled##Splash", &project.SplashScreen.Enabled);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("When off, the runtime loads the startup scene immediately with no splash.");
			}

			if (project.SplashScreen.Enabled) {
				ImGui::Spacing();
				changed |= ImGui::SliderFloat("Duration (s)##Splash",
					&project.SplashScreen.DurationSeconds, 0.5f, 10.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade In (s)##Splash",
					&project.SplashScreen.FadeInSeconds, 0.0f, 3.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade Out (s)##Splash",
					&project.SplashScreen.FadeOutSeconds, 0.0f, 3.0f, "%.2f");

				ImGui::Spacing();
				float bg[3] = {
					project.SplashScreen.BackgroundR,
					project.SplashScreen.BackgroundG,
					project.SplashScreen.BackgroundB,
				};
				if (ImGui::ColorEdit3("Background##Splash", bg)) {
					project.SplashScreen.BackgroundR = bg[0];
					project.SplashScreen.BackgroundG = bg[1];
					project.SplashScreen.BackgroundB = bg[2];
					changed = true;
				}

				float fontColor[3] = {
					project.SplashScreen.FontColorR,
					project.SplashScreen.FontColorG,
					project.SplashScreen.FontColorB,
				};
				if (ImGui::ColorEdit3("Font Color##Splash", fontColor)) {
					project.SplashScreen.FontColorR = fontColor[0];
					project.SplashScreen.FontColorG = fontColor[1];
					project.SplashScreen.FontColorB = fontColor[2];
					changed = true;
				}
				if (ImGui::DragFloat("Font Size##Splash", &project.SplashScreen.FontSize,
						0.5f, 6.0f, 96.0f, "%.0f px")) {
					if (project.SplashScreen.FontSize < 1.0f) project.SplashScreen.FontSize = 1.0f;
					changed = true;
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Image (optional, replaces default Index logo):");
				if (project.SplashScreen.ImagePath.empty()) {
					// Render the engine default logo as a placeholder so
					// the user sees what'll ship. Same source the runtime
					// splash falls back to.
					const std::string defaultLogoPath = SplashAssetResolve::DefaultLogoPath();
					if (!defaultLogoPath.empty()) {
						TextureHandle h = TextureManager::LoadTexture(defaultLogoPath);
						Texture2D* tex = TextureManager::GetTexture(h);
						if (tex && tex->IsValid()) {
							ImGui::Image(
								static_cast<ImTextureID>(static_cast<intptr_t>(tex->GetHandle())),
								ImVec2(48, 48));
							ImGui::SameLine();
						}
					}
					ImGui::TextDisabled("(Default)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(project.SplashScreen.ImagePath);
				}
				if (ImGui::Button("Browse...##SplashImage")) {
					ReferencePicker::OpenForFieldKey(k_SplashPickerKey, "Select Splash Image",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project.SplashScreen.ImagePath =
						SplashAssetResolve::NormalizeForStorage(dropped, &project);
					changed = true;
				})) {}
				if (!project.SplashScreen.ImagePath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear##SplashImage")) {
						project.SplashScreen.ImagePath.clear();
						changed = true;
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Background Image (optional, painted full-screen behind logo):");
				constexpr const char* k_SplashBgPickerKey = "ProjectSettings.SplashBackground";

				// Apply a pending background-image picker selection — same
				// path-normalisation flow as the foreground image so the
				// stored value stays project-relative.
				if (auto pending = ReferencePicker::ConsumeSelection(k_SplashBgPickerKey); pending) {
					const std::string raw = *pending;
					uint64_t pickedId = 0;
					try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
					if (pickedId == 0) {
						project.SplashScreen.BackgroundImagePath.clear();
						changed = true;
					}
					else {
						std::string absPath = AssetRegistry::ResolvePath(pickedId);
						if (!absPath.empty()) {
							project.SplashScreen.BackgroundImagePath =
								SplashAssetResolve::NormalizeForStorage(absPath, &project);
							changed = true;
						}
					}
				}

				if (project.SplashScreen.BackgroundImagePath.empty()) {
					ImGui::TextDisabled("(no background image — solid colour fill above)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(project.SplashScreen.BackgroundImagePath);
				}
				if (ImGui::Button("Browse...##SplashBgImage")) {
					ReferencePicker::OpenForFieldKey(k_SplashBgPickerKey, "Select Splash Background",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project.SplashScreen.BackgroundImagePath =
						SplashAssetResolve::NormalizeForStorage(dropped, &project);
					changed = true;
				})) {}
				if (!project.SplashScreen.BackgroundImagePath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear##SplashBgImage")) {
						project.SplashScreen.BackgroundImagePath.clear();
						changed = true;
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Custom Text (optional, replaces version + platform line):");
				char textBuf[256];
				std::snprintf(textBuf, sizeof(textBuf), "%s", project.SplashScreen.CustomText.c_str());
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputTextWithHint("##SplashText", "Leave empty for engine default",
					textBuf, sizeof(textBuf))) {
					project.SplashScreen.CustomText = textBuf;
					changed = true;
				}

				ImGui::Spacing();
				if (ImGui::Button("Show Preview")) {
					m_SplashPreviewRequest = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Replay the splash screen using the current\n"
						"settings, drawn on top of the editor for the\n"
						"FadeIn + Duration + FadeOut window.");
				}
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Splash Screen")

		if (SectionVisible(SettingsCategory::Branding, "App Icon", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("App Icon", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			constexpr const char* k_AppIconPickerKey = "ProjectSettings.AppIcon";

			// AppIconPath is the shipped game's icon only; do NOT push to glfwSetWindowIcon — the editor keeps its own embedded RT_ICON.
			if (auto pending = ReferencePicker::ConsumeSelection(k_AppIconPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project.AppIconPath.clear();
					changed = true;
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						project.AppIconPath =
							SplashAssetResolve::NormalizeForStorage(absPath, &project);
						changed = true;
					}
				}
			}

			if (!project.AppIconPath.empty()) {
				TextureHandle iconHandle = TextureManager::LoadTexture(
					SplashAssetResolve::Resolve(project.AppIconPath, &project));
				Texture2D* iconTex = TextureManager::GetTexture(iconHandle);
				if (iconTex && iconTex->IsValid()) {
					ImGui::Image(
						static_cast<ImTextureID>(static_cast<intptr_t>(iconTex->GetHandle())),
						ImVec2(64, 64));
					ImGui::SameLine();
				}
				else {
					ImGui::TextDisabled("Failed to load:");
					ImGuiUtils::TextDisabledEllipsis(project.AppIconPath);
				}

				ImGui::BeginGroup();
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project.AppIconPath =
						SplashAssetResolve::NormalizeForStorage(dropped, &project);
					changed = true;
				})) {}
				ImGui::SameLine();
				if (ImGui::Button("Clear##AppIcon")) {
					project.AppIconPath.clear();
					changed = true;
				}
				ImGui::EndGroup();

				if (iconTex && iconTex->IsValid()) {
					ImGuiUtils::TextDisabledEllipsis(project.AppIconPath);
				}
			}
			else {
				const std::string defaultIconPath = SplashAssetResolve::DefaultLogoPath();
				if (!defaultIconPath.empty()) {
					TextureHandle defIconHandle = TextureManager::LoadTexture(defaultIconPath);
					Texture2D* defIconTex = TextureManager::GetTexture(defIconHandle);
					if (defIconTex && defIconTex->IsValid()) {
						ImGui::Image(
							static_cast<ImTextureID>(static_cast<intptr_t>(defIconTex->GetHandle())),
							ImVec2(64, 64));
						ImGui::SameLine();
					}
				}

				ImGui::BeginGroup();
				if (defaultIconPath.empty()) {
					ImGui::TextDisabled("No icon set");
				}
				else {
					ImGui::TextDisabled("(Default)");
				}
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project.AppIconPath =
						SplashAssetResolve::NormalizeForStorage(dropped, &project);
					changed = true;
				})) {}
				ImGui::SameLine();
				ImGui::TextDisabled("or drag an image onto the Browse button");
				ImGui::EndGroup();
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("App Icon")

		if (SectionVisible(SettingsCategory::Branding, "Cursors", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Cursors", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			auto resolveProjectRelativeCursor = [&](const std::string& rel) -> std::string {
				std::filesystem::path p(rel);
				if (p.is_absolute()) return rel;
				return (std::filesystem::path(project.RootDirectory) / p).string();
			};

			// Apply a project-relative path to the live Window cursor so
			// the change is visible immediately rather than requiring a
			// restart. Empty path clears the slot back to the OS default.
			auto applyCursorLive = [&](const std::string& path, void (Window::*setter)(const Texture2D*))
			{
				Window* win = Application::GetInstance() ? Application::GetInstance()->GetWindow() : nullptr;
				if (!win) return;
				if (path.empty()) {
					(win->*setter)(nullptr);
					return;
				}
				const std::string abs = resolveProjectRelativeCursor(path);
				if (!std::filesystem::exists(abs)) {
					IDX_WARN_TAG("Editor", "Cursor image not found: {}", abs);
					return;
				}
				TextureHandle h = TextureManager::LoadTexture(abs);
				if (Texture2D* tex = TextureManager::GetTexture(h); tex && tex->IsValid()) {
					(win->*setter)(tex);
				}
			};

			auto renderCursorSlot = [&](const char* label,
				const char* pickerKey, std::string& slotPath,
				void (Window::*liveSetter)(const Texture2D*))
			{
				ImGui::PushID(label);
				ImGui::TextUnformatted(label);

				if (auto pending = ReferencePicker::ConsumeSelection(pickerKey); pending) {
					const std::string raw = *pending;
					uint64_t pickedId = 0;
					try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
					if (pickedId == 0) {
						slotPath.clear();
						changed = true;
						applyCursorLive("", liveSetter);
					}
					else {
						std::string absPath = AssetRegistry::ResolvePath(pickedId);
						if (!absPath.empty()) {
							std::filesystem::path absFs(absPath);
							std::filesystem::path assetsDir(project.AssetsDirectory);
							if (absFs.string().find(assetsDir.string()) == 0) {
								slotPath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
							}
							else {
								slotPath = absFs.filename().string();
							}
							changed = true;
							applyCursorLive(slotPath, liveSetter);
						}
					}
				}

				if (slotPath.empty()) {
					ImGui::TextDisabled("(OS default)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(slotPath);
				}

				if (ImGui::Button("Browse...")) {
					ReferencePicker::OpenForFieldKey(pickerKey, "Select Cursor Image",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					slotPath = ToProjectRelativeAssetPath(dropped, project.AssetsDirectory);
					changed = true;
					applyCursorLive(slotPath, liveSetter);
				})) {}
				if (!slotPath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear")) {
						slotPath.clear();
						changed = true;
						applyCursorLive("", liveSetter);
					}
				}
				ImGui::PopID();
			};

			renderCursorSlot("Default cursor", "ProjectSettings.DefaultCursor",
				project.CursorImagePath, &Window::SetCursorImage);
			ImGui::Spacing();
			renderCursorSlot("UI hover cursor", "ProjectSettings.UICursor",
				project.UIInteractableCursorImagePath, &Window::SetUICursorImage);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Shown while the cursor sits over an Index UI element\n"
					"with an Interactable component (Button / Slider / etc.).\n"
					"Falls back to the default cursor when unset.");
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Cursors")
	}

	void ImGuiEditorLayer::RenderSettings_Build(IndexProject& project, bool& changed,
		const std::string& filterLower)
	{
		if (SectionVisible(SettingsCategory::Build, "Executable", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Executable", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::TextUnformatted("Output Name:");
			char exeBuf[256];
			std::string current = project.ExecutableName.empty() ? project.Name : project.ExecutableName;
			std::snprintf(exeBuf, sizeof(exeBuf), "%s", current.c_str());
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##ExecutableName", exeBuf, sizeof(exeBuf))) {
				const std::string newName(exeBuf);
				project.ExecutableName = (newName == project.Name) ? "" : newName;
				changed = true;
			}
			ImGui::TextDisabled("Default: project name (\"%s\"). The platform extension is appended automatically.",
				project.Name.c_str());
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Executable")

		if (SectionVisible(SettingsCategory::Build, "Diagnostics", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Show runtime stats overlay (F6)", &project.ShowRuntimeStats);
			changed |= ImGui::Checkbox("Show runtime log overlay (F7)", &project.ShowRuntimeLogs);
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Diagnostics")

		if (SectionVisible(SettingsCategory::Build, "ECS Entity Bits", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("ECS Entity Bits", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::TextUnformatted("Entity ID bits:");
			const char* entityBitsLabels[] = {
				"16 bits  (65,535 entities / 65,536 versions)",
				"20 bits  (1,048,575 entities / 4,096 versions)  [default]",
				"22 bits  (4,194,303 entities / 1,024 versions)",
				"24 bits  (16,777,215 entities / 256 versions)",
				"28 bits  (268,435,455 entities / 16 versions)"
			};
			constexpr int entityBitsValues[] = { 16, 20, 22, 24, 28 };
			int entityBitsIdx = 1; // matches the 20-bit default
			for (int i = 0; i < IM_ARRAYSIZE(entityBitsValues); ++i) {
				if (entityBitsValues[i] == project.EntityBits) {
					entityBitsIdx = i;
					break;
				}
			}
			ImGui::SetNextItemWidth(-1);
			if (ImGui::Combo("##EntityBits", &entityBitsIdx, entityBitsLabels, IM_ARRAYSIZE(entityBitsLabels))) {
				const int newBits = entityBitsValues[entityBitsIdx];
				if (newBits != project.EntityBits) {
					project.EntityBits = newBits;
					changed = true;
				}
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Maximum number of live entities the scene can hold.\n"
					"Trades against the per-slot version count EnTT uses\n"
					"to detect stale handles to recycled IDs.\n"
					"\n"
					"Compile-time setting — the engine must be rebuilt for\n"
					"the change to take effect. Click \"Rebuild Engine\" below\n"
					"to do that automatically.");
			}

			const int compiledBits = Index::GetCompiledEntityBits();
			ImGui::Spacing();
			if (compiledBits != project.EntityBits) {
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.20f, 1.0f),
					"Compiled: %d bits   Pending: %d bits",
					compiledBits, project.EntityBits);
				ImGui::Spacing();
				if (ImGui::Button("Rebuild Engine")) {
					ImGui::OpenPopup("Rebuild Engine?");
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Closes the editor and reopens it after the engine has\n"
						"been rebuilt with the new entityBits setting.\n"
						"\n"
						"A separate \"Rebuilding Engine\" window will appear\n"
						"with progress. Don't close it.");
				}
			}
			else {
				ImGui::TextDisabled("Engine matches the selected width. Choose a different size to enable rebuild.");
			}

			// Build runs in a detached cmd.exe (not in-process) because the editor/launcher both hold Index-Engine.dll locked — MSBuild would fail with LNK1104.
			// Editor writes IndexEntityBitsConfig.h directly so MSBuild incremental only recompiles TUs that include entt (no premake regen needed).
			ImGuiUtils::CenterNextModal();
			ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
			if (ImGui::BeginPopupModal("Rebuild Engine?", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
				ImGui::TextWrapped(
					"Rebuild the engine with entityBits = %d?\n\n"
					"The editor will close and a console window will open showing the build "
					"progress. The editor reopens automatically when the build finishes. "
					"Don't close the console window.",
					project.EntityBits);
				ImGui::Spacing();
				if (ImGui::Button("Rebuild", ImVec2(120.0f, 0.0f))) {
					project.Save();

					const std::filesystem::path editorExeDir = Path::ExecutableDir();
					const std::filesystem::path engineRoot =
						editorExeDir.parent_path().parent_path().parent_path();
					const std::string msbuildPath = IndexProject::GetMSBuildPath();
					const std::filesystem::path slnPath = engineRoot / "Index.sln";
					const std::filesystem::path editorExe = editorExeDir / "Index-Editor.exe";
					const std::string config = IndexProject::GetActiveBuildConfiguration();
					const std::string projectRootDir = project.RootDirectory;
					const std::string userCsprojPath = project.CsprojPath;
					const int newEntityBits = project.EntityBits;

				#ifdef IDX_PLATFORM_WINDOWS
					const unsigned long editorPid = ::GetCurrentProcessId();
				#else
					const unsigned long editorPid = 0; // Windows-only flow today; placeholder for clarity.
				#endif

					const std::filesystem::path generatedDir = engineRoot
						/ "Index-Engine" / "src" / "Generated";
					const std::filesystem::path headerPath = generatedDir
						/ "IndexEntityBitsConfig.h";
					bool headerWritten = false;
					{
						std::error_code dirEc;
						std::filesystem::create_directories(generatedDir, dirEc);
						if (!dirEc) {
							std::ofstream headerOut(headerPath,
								std::ios::binary | std::ios::trunc);
							if (headerOut) {
								headerOut <<
									"// Auto-generated by the editor's Rebuild Engine flow.\n"
									"// Matches premake's WriteIndexEntityBitsConfigHeader().\n"
									"// Do not edit by hand.\n"
									"#pragma once\n"
									"#undef  INDEX_ENTITY_BITS\n"
									"#define INDEX_ENTITY_BITS " << newEntityBits << "\n";
								headerWritten = static_cast<bool>(headerOut);
							}
						}
						if (!headerWritten) {
							IDX_ERROR_TAG("Editor",
								"Failed to write {}; engine rebuild aborted.",
								headerPath.string());
						}
					}

					if (headerWritten) {
						std::ostringstream bat;
						bat <<
							"@echo off\r\n"
							"setlocal\r\n"
							"title Rebuilding Index Engine (entityBits=" << newEntityBits << ")\r\n"
							"echo.\r\n"
							"echo ====================================================\r\n"
							"echo   Rebuilding Index Engine  (entityBits=" << newEntityBits << ")\r\n"
							"echo ====================================================\r\n"
							"echo.\r\n"
							"echo Waiting for editor (PID " << editorPid << ") to exit...\r\n"
							"set /a WAIT_ELAPSED=0\r\n"
							":wait_loop\r\n"
							"tasklist /fi \"PID eq " << editorPid << "\" 2>nul | findstr /i \"" << editorPid << "\" >nul\r\n"
							"if errorlevel 1 goto wait_done\r\n"
							"set /a WAIT_ELAPSED+=1\r\n"
							"if %WAIT_ELAPSED% geq 30 (\r\n"
							"  echo Warning: editor still alive after 30 seconds, proceeding anyway.\r\n"
							"  goto wait_done\r\n"
							")\r\n"
							"timeout /t 1 /nobreak >nul\r\n"
							"goto wait_loop\r\n"
							":wait_done\r\n"
							"echo.\r\n"
							"echo [1/2] Building user scripts (dotnet build)...\r\n"
							"echo.\r\n"
							"dotnet build \"" << userCsprojPath << "\""
							" -c " << config <<
							" --nologo -v q\r\n"
							"if errorlevel 1 (\r\n"
							"  echo.\r\n"
							"  echo *** User script build FAILED. C# errors above. ***\r\n"
							"  echo.\r\n"
							"  pause\r\n"
							"  exit /b 1\r\n"
							")\r\n"
							"echo.\r\n"
							"echo [2/2] Building solution (this is the long step)...\r\n"
							"echo.\r\n"
							"pushd \"" << engineRoot.string() << "\"\r\n"
							"\"" << msbuildPath << "\" \"" << slnPath.string() << "\""
							" /p:Configuration=" << config <<
							" /p:Platform=x64 /m /v:minimal /nologo\r\n"
							"if errorlevel 1 (\r\n"
							"  echo.\r\n"
							"  echo *** MSBuild FAILED. Scroll up for compile errors. ***\r\n"
							"  echo.\r\n"
							"  pause\r\n"
							"  exit /b 1\r\n"
							")\r\n"
							"popd\r\n"
							"echo.\r\n"
							"echo [Relaunching editor...]\r\n"
							"echo.\r\n"
							"start \"\" \"" << editorExe.string() << "\" --project=\"" << projectRootDir << "\"\r\n"
							"endlocal\r\n"
							"exit /b 0\r\n";

						std::error_code mkdirEc;
						const std::filesystem::path batDir = engineRoot / "bin-int";
						std::filesystem::create_directories(batDir, mkdirEc);
						const std::filesystem::path batPath = batDir / "engine-rebuild.bat";
						std::ofstream batFile(batPath, std::ios::binary | std::ios::trunc);
						batFile << bat.str();
						batFile.close();

						const bool spawned = Process::LaunchDetached(
							{ "cmd.exe", "/c", batPath.string() }, engineRoot);
						ImGui::CloseCurrentPopup();
						if (spawned) {
							// MUST Quit() cleanly to release the engine DLL file lock before MSBuild overwrites it.
							Application::GetInstance()->Quit();
						}
						else {
							IDX_ERROR_TAG("Editor",
								"Failed to spawn cmd.exe for engine rebuild (script at {})",
								batPath.string());
						}
					}
					else {
					}
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("ECS Entity Bits")
	}

	void ImGuiEditorLayer::RenderSettings_Editor(IndexProject& project, bool& changed,
		const std::string& filterLower)
	{
		if (SectionVisible(SettingsCategory::Editor, "Asset Browser", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Asset Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_AssetSuffixLabels[] = {
				"Asset 1",
				"Asset (1)",
				"Asset-1",
				"Asset_1",
			};
			int assetSuffixIndex = static_cast<int>(project.EditorAssetDuplicateSuffix);
			if (assetSuffixIndex < 0 || assetSuffixIndex >= IM_ARRAYSIZE(k_AssetSuffixLabels)) {
				assetSuffixIndex = static_cast<int>(IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber);
			}
			ImGui::SetNextItemWidth(180.0f);
			if (ImGui::Combo("Duplicate suffix##AssetDuplicateSuffix", &assetSuffixIndex, k_AssetSuffixLabels, IM_ARRAYSIZE(k_AssetSuffixLabels))) {
				project.EditorAssetDuplicateSuffix =
					static_cast<IndexProject::EditorEntityNameSuffixStyle>(assetSuffixIndex);
				changed = true;
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Asset Browser")

		if (SectionVisible(SettingsCategory::Editor, "Entities", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Entities", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Ensure unique editor-created names", &project.EditorEnsureUniqueEntityNames);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"When on, entities created through editor UI actions,\n"
					"shortcuts, paste, duplicate, and asset drops get a\n"
					"non-conflicting name. Runtime/script-created entities\n"
					"skip this editor-only pass.");
			}

			constexpr const char* k_StyleLabels[] = {
				"Entity 1",
				"Entity (1)",
				"Entity-1",
				"Entity_1",
			};
			int suffixIndex = static_cast<int>(project.EditorEntityNameSuffix);
			if (suffixIndex < 0 || suffixIndex >= IM_ARRAYSIZE(k_StyleLabels)) {
				suffixIndex = static_cast<int>(IndexProject::EditorEntityNameSuffixStyle::ParenthesizedNumber);
			}
			ImGui::SetNextItemWidth(180.0f);
			if (ImGui::Combo("Duplicate suffix##EntityNameSuffix", &suffixIndex, k_StyleLabels, IM_ARRAYSIZE(k_StyleLabels))) {
				project.EditorEntityNameSuffix =
					static_cast<IndexProject::EditorEntityNameSuffixStyle>(suffixIndex);
				changed = true;
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Entities")

		if (SectionVisible(SettingsCategory::Editor, "Serialization", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Serialization", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_FormatLabels[] = { "JSON", "Binary" };
			int formatIndex = project.AssetSerializationFormat == IndexProject::ProjectAssetSerializationFormat::Binary ? 1 : 0;
			ImGui::SetNextItemWidth(180.0f);
			if (ImGui::Combo("Asset format", &formatIndex, k_FormatLabels, IM_ARRAYSIZE(k_FormatLabels))) {
				const auto nextFormat = formatIndex == 1
					? IndexProject::ProjectAssetSerializationFormat::Binary
					: IndexProject::ProjectAssetSerializationFormat::Json;
				if (nextFormat != project.AssetSerializationFormat) {
					project.AssetSerializationFormat = nextFormat;
					const SceneSerializationFormat sceneFormat = ToSceneSerializationFormat(nextFormat);
					const int converted = ConvertSerializedAssetsInDirectory(project.AssetsDirectory, sceneFormat);
					if (Scene* activeScene = SceneManager::Get().GetActiveScene()) {
						if (activeScene->IsDirty()) {
							SceneSerializer::SaveToFile(*activeScene,
								project.GetSceneFilePath(activeScene->GetName()),
								sceneFormat);
						}
					}
					AssetRegistry::MarkDirty();
					AssetRegistry::Sync();
					IDX_INFO_TAG("ProjectSettings",
						"Reserialized {} scene/prefab asset(s) as {}",
						converted,
						IndexProject::ProjectAssetSerializationFormatToString(nextFormat));
					changed = true;
				}
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Controls how scene and prefab assets are written on disk.\n"
					"Changing this rewrites existing .scene and .prefab files\n"
					"only when the selected format actually changes.");
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Serialization")

		if (SectionVisible(SettingsCategory::Editor, "Scripting", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Scripting", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			Scene* activeScene = SceneManager::Get().GetActiveScene();
			ScriptSystem* scriptSys = (activeScene && activeScene->HasSystem<ScriptSystem>())
				? activeScene->GetSystem<ScriptSystem>()
				: nullptr;
			const bool canRecompile = scriptSys && !scriptSys->IsRebuilding();
			if (!canRecompile) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button("Recompile Scripts") && scriptSys) {
				scriptSys->RequestRebuildAndReloadAll();
			}
			if (!canRecompile) {
				ImGui::EndDisabled();
			}
			if (scriptSys && scriptSys->IsRebuilding()) {
				ImGui::SameLine();
				ImGui::TextDisabled("Compiling...");
			}
			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Scripting")
	}

	void ImGuiEditorLayer::RenderSettings_Systems(IndexProject& project, bool& changed,
		bool& outGlobalSystemsChanged, const std::string& filterLower)
	{
		if (SectionVisible(SettingsCategory::Systems, "Global Systems", filterLower)) {
		if (!filterLower.empty()) ImGui::SetNextItemOpen(true);
		if (ImGui::CollapsingHeader("Global Systems", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##GlobalSystemSearch", "Search global systems...",
				m_GlobalSystemSearchBuffer, sizeof(m_GlobalSystemSearchBuffer));

			std::string filter(m_GlobalSystemSearchBuffer);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
			EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);

			std::unordered_set<std::string> discoveredGlobalSystems;
			for (const auto& scriptEntry : scriptEntries) {
				if (!scriptEntry.IsGlobalSystem) {
					continue;
				}

				discoveredGlobalSystems.insert(scriptEntry.ClassName);
				if (!filter.empty()) {
					std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
					std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
					if (lowerClassName.find(filter) == std::string::npos
						&& lowerPath.find(filter) == std::string::npos) {
						continue;
					}
				}

				auto it = std::find_if(project.GlobalSystems.begin(), project.GlobalSystems.end(),
					[&](const IndexProject::GlobalSystemRegistration& registration) {
						return registration.ClassName == scriptEntry.ClassName;
					});
				bool active = it != project.GlobalSystems.end() ? it->Active : false;
				if (ImGui::Checkbox(scriptEntry.ClassName.c_str(), &active)) {
					if (it == project.GlobalSystems.end()) {
						project.GlobalSystems.push_back({ scriptEntry.ClassName, active });
					}
					else {
						it->Active = active;
					}
					changed = true;
					outGlobalSystemsChanged = true;
				}
			}

			for (auto& registration : project.GlobalSystems) {
				if (discoveredGlobalSystems.contains(registration.ClassName)) {
					continue;
				}
				bool active = registration.Active;
				std::string label = registration.ClassName + " (missing)";
				if (ImGui::Checkbox(label.c_str(), &active)) {
					registration.Active = active;
					changed = true;
					outGlobalSystemsChanged = true;
				}
			}

			ImGui::Unindent(8);
		}
		}  // end SectionVisible("Global Systems")
	}

	void ImGuiEditorLayer::TickSplashPreview() {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_SplashPreviewRequest = false;
			m_SplashPreviewActive = false;
			return;
		}

		if (m_SplashPreviewRequest) {
			m_SplashPreviewRequest = false;
			m_SplashPreviewActive = true;
			m_SplashPreviewElapsed = 0.0f;

			std::string logoPath = SplashAssetResolve::Resolve(
				project->SplashScreen.ImagePath, project);
			if (logoPath.empty()) {
				logoPath = SplashAssetResolve::DefaultLogoPath();
			}
			m_SplashPreviewLogo = logoPath.empty()
				? TextureHandle::Invalid()
				: TextureManager::LoadTexture(logoPath);

			// Background
			const std::string bgPath = SplashAssetResolve::Resolve(
				project->SplashScreen.BackgroundImagePath, project);
			m_SplashPreviewBackground = bgPath.empty()
				? TextureHandle::Invalid()
				: TextureManager::LoadTexture(bgPath);

			// Keep both textures alive across PurgeUnreferenced for
			// the preview lifetime; release in the completion path
			// below.
			if (m_SplashPreviewTexRefToken == 0) {
				m_SplashPreviewTexRefToken = TextureManager::AddReferenceProvider(
					[this](const TextureManager::ReferenceEmitter& emit) {
						if (m_SplashPreviewLogo.IsValid()) emit(m_SplashPreviewLogo);
						if (m_SplashPreviewBackground.IsValid()) emit(m_SplashPreviewBackground);
					});
			}
		}

		if (!m_SplashPreviewActive) return;

		auto* app = Application::GetInstance();
		const float dt = app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
		m_SplashPreviewElapsed += dt;

		const float fadeIn = std::max(0.0f, project->SplashScreen.FadeInSeconds);
		const float hold   = std::max(0.0f, project->SplashScreen.DurationSeconds);
		const float fadeOut = std::max(0.0f, project->SplashScreen.FadeOutSeconds);
		const float total = fadeIn + hold + fadeOut;
		if (m_SplashPreviewElapsed >= total) {
			m_SplashPreviewActive = false;
			if (m_SplashPreviewTexRefToken != 0) {
				TextureManager::RemoveReferenceProvider(m_SplashPreviewTexRefToken);
				m_SplashPreviewTexRefToken = 0;
			}
			m_SplashPreviewLogo = TextureHandle::Invalid();
			m_SplashPreviewBackground = TextureHandle::Invalid();
			return;
		}

		float alpha = 1.0f;
		if (m_SplashPreviewElapsed < fadeIn && fadeIn > 0.0f) {
			alpha = m_SplashPreviewElapsed / fadeIn;
		}
		else if (m_SplashPreviewElapsed > fadeIn + hold && fadeOut > 0.0f) {
			alpha = 1.0f - (m_SplashPreviewElapsed - fadeIn - hold) / fadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		// Use foreground draw list to float above all docks; avoids dockspace InvisibleButton/scissor issues.
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImDrawList* draw = ImGui::GetForegroundDrawList(vp);
		const ImVec2 wMin = vp->WorkPos;
		const ImVec2 wMax(wMin.x + vp->WorkSize.x, wMin.y + vp->WorkSize.y);
		const float width = vp->WorkSize.x;
		const float height = vp->WorkSize.y;

		auto packColor = [](float r, float g, float b, float a) {
			r = std::clamp(r, 0.0f, 1.0f);
			g = std::clamp(g, 0.0f, 1.0f);
			b = std::clamp(b, 0.0f, 1.0f);
			a = std::clamp(a, 0.0f, 1.0f);
			return IM_COL32(int(r * 255), int(g * 255), int(b * 255), int(a * 255));
		};

		draw->AddRectFilled(wMin, wMax, packColor(
			project->SplashScreen.BackgroundR,
			project->SplashScreen.BackgroundG,
			project->SplashScreen.BackgroundB,
			alpha));

		Texture2D* bg = TextureManager::GetTexture(m_SplashPreviewBackground);
		if (bg && bg->IsValid()) {
			const float bgW = static_cast<float>(bg->GetWidth());
			const float bgH = static_cast<float>(bg->GetHeight());
			if (bgW > 0.0f && bgH > 0.0f) {
				const float canvasAspect = width / height;
				const float bgAspect = bgW / bgH;
				float drawW = width, drawH = height;
				if (bgAspect > canvasAspect) drawW = drawH * bgAspect;
				else                          drawH = drawW / bgAspect;
				const ImVec2 bgMin(wMin.x + (width  - drawW) * 0.5f,
				                   wMin.y + (height - drawH) * 0.5f);
				const ImVec2 bgMax(bgMin.x + drawW, bgMin.y + drawH);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(bg->GetHandle())),
					bgMin, bgMax,
					ImVec2(0, 0), ImVec2(1, 1),
					packColor(1.0f, 1.0f, 1.0f, alpha));
			}
		}

		const float centerX = wMin.x + width * 0.5f;
		const float centerY = wMin.y + height * 0.5f;

		Texture2D* logo = TextureManager::GetTexture(m_SplashPreviewLogo);
		if (logo && logo->IsValid()) {
			const float maxLogoSide = std::min(width, height) * 0.35f;
			float logoW = static_cast<float>(logo->GetWidth());
			float logoH = static_cast<float>(logo->GetHeight());
			if (logoW > 0 && logoH > 0) {
				const float scale = std::min(maxLogoSide / logoW, maxLogoSide / logoH);
				logoW *= scale; logoH *= scale;
				const ImVec2 imgMin(centerX - logoW * 0.5f, centerY - logoH * 0.65f);
				const ImVec2 imgMax(imgMin.x + logoW, imgMin.y + logoH);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(logo->GetHandle())),
					imgMin, imgMax,
					ImVec2(0, 0), ImVec2(1, 1),
					packColor(1.0f, 1.0f, 1.0f, alpha));
			}
		}

		const std::string subtitle = project->SplashScreen.CustomText.empty()
			? SplashAssetResolve::DefaultSubtitleLine()
			: project->SplashScreen.CustomText;
		if (!subtitle.empty()) {
			ImFont* font = ImGui::GetFont();
			const float fontSize = (project->SplashScreen.FontSize > 0.0f)
				? project->SplashScreen.FontSize : ImGui::GetFontSize();
			const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, subtitle.c_str());
			const ImVec2 textPos(centerX - textSize.x * 0.5f,
				centerY + std::min(width, height) * 0.18f);
			draw->AddText(font, fontSize, textPos,
				packColor(project->SplashScreen.FontColorR,
					project->SplashScreen.FontColorG,
					project->SplashScreen.FontColorB,
					alpha * 0.85f),
				subtitle.c_str());
		}
	}

	void ImGuiEditorLayer::RenderAssetInspector() {
		const std::string& selectedPath = m_AssetBrowser.GetSelectedPath();
		if (selectedPath.empty()) {
			// Selection cleared. If the prefab inspector held a dirty prefab,
			// the save/discard prompt is driven from the dispatch above; here
			// just close cleanly.
			if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
				m_PrefabInspector.Close();
				m_PrefabInspectorPath.clear();
			}
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::filesystem::path path(selectedPath);
		if (!std::filesystem::exists(path)) {
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::string name = path.filename().string();
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// `.prefab` selection routes through PrefabInspector. Selection change
		// while a dirty prefab is open opens a save/discard modal — the new
		// selection is queued in m_PendingPrefabSwitchPath until the user picks.
		if (ext == ".prefab") {
			if (m_PrefabInspector.IsOpen() && m_PrefabInspectorPath != selectedPath) {
				if (m_PrefabInspector.HasUnsavedChanges()) {
					m_PendingPrefabSwitchPath = selectedPath;
					m_ShowPrefabSavePrompt = true;
				}
				else {
					m_PrefabInspector.Open(selectedPath);
					m_PrefabInspectorPath = selectedPath;
				}
			}
			else if (!m_PrefabInspector.IsOpen()) {
				m_PrefabInspector.Open(selectedPath);
				m_PrefabInspectorPath = selectedPath;
			}
			m_PrefabInspector.Render();
			return;
		}

		// Non-prefab asset: clean up any open prefab inspector (no prompt — user
		// already navigated away; if they had unsaved work the prompt UX would
		// have caught it during the .prefab→.prefab switch above).
		if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
			m_PrefabInspector.Close();
			m_PrefabInspectorPath.clear();
		}

		if (ext == ".dataasset") {
			ImGui::TextDisabled("Data Asset:");
			ImGui::SameLine();
			ImGuiUtils::TextEllipsis(name);
			ImGui::Separator();
			DrawDataAssetFields(AssetRegistry::GetOrCreateAssetUUID(selectedPath));
			return;
		}

		ImGui::TextDisabled("Asset:");
		ImGui::SameLine();
		ImGuiUtils::TextEllipsis(name);
		ImGui::Separator();

		ImGui::TextDisabled("Path:");
		ImGui::SameLine();
		ImGuiUtils::TextDisabledEllipsis(selectedPath);

		try {
			auto fileSize = std::filesystem::file_size(path);
			if (fileSize >= 1024 * 1024) ImGui::TextDisabled("Size: %.2f MB", fileSize / (1024.0f * 1024.0f));
			else if (fileSize >= 1024) ImGui::TextDisabled("Size: %.1f KB", fileSize / 1024.0f);
			else ImGui::TextDisabled("Size: %llu bytes", fileSize);
		}
		catch (...) {
		}

		ImGui::TextDisabled("Type: %s", ext.c_str());
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
			ImGui::Spacing();
			const Texture2D* tex = GetPreviewTexture(path);
			if (tex && tex->IsValid()) {
				// Use the Texture2D overload so the canonical flip rule
				// applies — preview matches the asset-browser thumbnail
				// regardless of the texture's load-time flipVertical flag.
				ImGuiUtils::DrawTexturePreview(*tex, 128.0f);
				ImGui::Text("%.0f x %.0f", tex->GetWidth(), tex->GetHeight());

				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();

				TextureMeta meta = AssetRegistry::ReadTextureMeta(selectedPath);
				if (!meta.HasImportBlock) {
					meta.Import.FilterMode = tex->GetFilter();
					meta.Import.WrapU      = tex->GetWrapU();
					meta.Import.WrapV      = tex->GetWrapV();
				}

				const auto persist = [&](TextureMeta updated) {
					updated.HasImportBlock = true;
					if (AssetRegistry::WriteTextureMeta(selectedPath, updated)) {
						TextureManager::ApplyMetaSamplerToLoaded(selectedPath);
					}
				};

				ImGui::PushID("TextureImportSettings");
				ImGuiUtils::DrawEnumCombo<Filter>("Filter Mode", meta.Import.FilterMode,
					[&](Filter newFilter) {
						TextureMeta updated = meta;
						updated.Import.FilterMode = newFilter;
						persist(updated);
					});
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap U", meta.Import.WrapU,
					[&](Wrap newWrap) {
						TextureMeta updated = meta;
						updated.Import.WrapU = newWrap;
						persist(updated);
					});
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap V", meta.Import.WrapV,
					[&](Wrap newWrap) {
						TextureMeta updated = meta;
						updated.Import.WrapV = newWrap;
						persist(updated);
					});
				ImGui::PopID();

				ImGui::Spacing();
				if (ImGui::Button("Open Sprite Editor", ImVec2(-FLT_MIN, 0.0f))) {
					m_PendingSpriteEditorPath    = selectedPath;
					m_PendingSpriteEditorAssetId = AssetRegistry::GetOrCreateAssetUUID(selectedPath);
					m_ShowSpriteEditor           = true;
				}
			}
		}
	}

	const Texture2D* ImGuiEditorLayer::GetPreviewTexture(const std::filesystem::path& path) {
		const std::string canonicalPath = NormalizePreviewTexturePath(path);
		if (canonicalPath.empty()) {
			return nullptr;
		}

		if (const auto it = m_PreviewTextureLookup.find(canonicalPath); it != m_PreviewTextureLookup.end()) {
			PreviewTextureEntry& entry = m_PreviewTextureCache[it->second];
			entry.LastTouchTick = ++m_PreviewTextureTick;
			return entry.Texture.get();
		}

		// TODO(perf): Synchronous decode on UI thread — large textures cause first-select hitches.
		// Convert to async-task pattern (see index-add-editor-panel skill).
		auto texture = std::make_unique<Texture2D>(canonicalPath.c_str(), Filter::Point, Wrap::Clamp, Wrap::Clamp);
		if (!texture->IsValid()) {
			return nullptr;
		}

		PreviewTextureEntry entry;
		entry.CanonicalPath = canonicalPath;
		entry.Texture = std::move(texture);
		entry.LastTouchTick = ++m_PreviewTextureTick;

		const size_t index = m_PreviewTextureCache.size();
		m_PreviewTextureLookup[canonicalPath] = index;
		m_PreviewTextureCache.push_back(std::move(entry));
		TrimPreviewTextureCache();
		return m_PreviewTextureCache.back().Texture.get();
	}

	void ImGuiEditorLayer::TrimPreviewTextureCache() {
		while (m_PreviewTextureCache.size() > kMaxPreviewTextures) {
			auto victimIt = std::min_element(
				m_PreviewTextureCache.begin(),
				m_PreviewTextureCache.end(),
				[](const PreviewTextureEntry& a, const PreviewTextureEntry& b) {
					return a.LastTouchTick < b.LastTouchTick;
				});
			if (victimIt == m_PreviewTextureCache.end()) {
				break;
			}

			m_PreviewTextureCache.erase(victimIt);
			m_PreviewTextureLookup.clear();
			for (size_t i = 0; i < m_PreviewTextureCache.size(); ++i) {
				m_PreviewTextureLookup[m_PreviewTextureCache[i].CanonicalPath] = i;
			}
		}
	}

	void ImGuiEditorLayer::ClearPreviewTextureCache() {
		m_PreviewTextureLookup.clear();
		m_PreviewTextureCache.clear();
		m_PreviewTextureTick = 0;
	}

	void ImGuiEditorLayer::RenderPackageManagerPanel() {
		if (!m_ShowPackageManager) return;

		if (!m_PackageManagerInitialized) {
			m_PackageManager.Initialize();

			if (m_PackageManager.IsReady()) {
				m_PackageManager.AddSource(std::make_unique<NuGetSource>(m_PackageManager.GetToolPath()));
				m_PackageManager.AddSource(std::make_unique<GitHubSource>(
					m_PackageManager.GetToolPath(),
					"https://raw.githubusercontent.com/Ben-Scr/index-packages/main/index.json",
					"Engine Packages"));
			}

			m_PackageManagerPanel.Initialize(&m_PackageManager);
			m_PackageManagerInitialized = true;
		}

		ImGui::SetNextWindowSize(ImVec2(880, 540), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(420, 320), ImVec2(FLT_MAX, FLT_MAX));
		ImGuiImplWebGPU::SetNextWindowAsNativeDialog();
		ImGui::Begin("Package Manager", &m_ShowPackageManager);
		m_PackageManagerPanel.Render();
		ImGui::End();
	}

} // namespace Index
