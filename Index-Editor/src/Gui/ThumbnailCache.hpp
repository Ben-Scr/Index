#pragma once
#include "Graphics/Texture2D.hpp"
#include "Gui/AssetType.hpp"

#include <imgui.h>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

namespace Index {

	class ThumbnailCache {
	public:
		// E31: hard cap so the cache cannot grow unbounded as the user navigates
		// large project trees. Past this, the least-recently-used entry is evicted.
		static constexpr size_t k_MaxEntries = 256;

		void Initialize();
		void Shutdown();

		// Returns raw WGPUTextureView handle (cast to uint64_t) for ImGui::Image; 0 for non-image types.
		uint64_t GetThumbnail(const std::string& absolutePath);

		// Returns the cached Texture2D for an already-loaded thumbnail, or nullptr.
		Texture2D* GetCacheEntry(const std::string& absolutePath);

		// Removes a cached thumbnail (e.g. after file delete/rename).
		void Invalidate(const std::string& absolutePath);

		// Clears the entire cache.
		void Clear();

		// Draws a type-appropriate icon using ImGui draw primitives.
		// Used for folders and non-image assets.
		static void DrawAssetIcon(AssetType type, ImVec2 pos, float size);

		// Determines the asset type from a file extension.
		static AssetType GetAssetType(const std::string& extension);

		// Returns a display label for an asset type.
		static const char* GetAssetTypeLabel(AssetType type);

	private:
		// E31: classic LRU layout — m_LRU stores paths in access order (front =
		// most recent). Each cache entry holds an iterator into that list so
		// touching an entry on lookup is O(1) (splice to front + map lookup).
		struct CachedThumbnail {
			std::unique_ptr<Texture2D> Texture;
			// Backend handle returned by Texture2D::GetHandle(). Under WebGPU
			// this is the raw WGPUTextureView pointer (cast to uint64_t); the
			// field name predates the OpenGL → WebGPU port.
			uint64_t GlHandle = 0;
			std::list<std::string>::iterator LruIt;
			// ImGui frame this entry was last accessed. EnforceCapacity refuses to
			// evict an entry used this frame, because its WGPUTextureView is still
			// referenced by an ImGui::Image draw command that submits at frame end —
			// destroying it here would be a GPU use-after-free.
			uint64_t LastUsedFrame = 0;
		};

		// Touches a path to mark it most-recently-used. Caller must hold a valid
		// iterator into m_Cache for the same path.
		void TouchLru(std::unordered_map<std::string, CachedThumbnail>::iterator it);

		// Evicts least-recently-used entries until size <= k_MaxEntries.
		void EnforceCapacity();

		std::unordered_map<std::string, CachedThumbnail> m_Cache;
		std::list<std::string> m_LRU;
	};

} // namespace Index
