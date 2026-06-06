#pragma once
#include "Core/Export.hpp"
#include "Graphics/DefaultTexture.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureEntry.hpp"
#include "TextureHandle.hpp"
#include "Serialization/Path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace Index {
    class Application;
}

namespace Index {
        class INDEX_API TextureManager {
        public:
            static void Initialize();
            static void Shutdown();

            static TextureHandle LoadTexture(const std::string_view& path, Filter filter = Filter::Point, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            // Meta import block Filter/Wrap wins over caller-supplied defaults when present.
            static TextureHandle LoadTextureByUUID(uint64_t assetId, Filter filter = Filter::Point, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);

            // Re-applies .meta sampler settings to every loaded slot at the given path; avoids a full re-decode.
            static size_t ApplyMetaSamplerToLoaded(const std::string& path);
            static TextureHandle GetDefaultTexture(DefaultTexture type);
            static void UnloadTexture(TextureHandle handle);
            // filter/wrap disambiguate: the same path can be loaded multiple times with different samplers.
            static TextureHandle GetTextureHandle(const std::string& name, Filter filter, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            [[deprecated("Sampler-agnostic lookup is ambiguous when the same path is loaded with multiple Filter/Wrap combinations. Pass filter+wrap explicitly.")]]
            static TextureHandle GetTextureHandle(const std::string& name);
            static Texture2D* GetTexture(TextureHandle handle);
            static bool ReloadTexture(TextureHandle handle);
            static size_t ReloadTexturePath(const std::string& path);
            static size_t ReloadTexturesFromDisk();
            static std::vector<TextureHandle> GetLoadedHandles();
            static void UnloadAll(bool defaultTextures = false);
            static uint64_t GetTextureAssetUUID(TextureHandle handle);

            // Frees every loaded slot not referenced by any scene component or registered provider; default textures are never evicted.
            static size_t PurgeUnreferenced();

            using ReferenceEmitter = std::function<void(TextureHandle)>;
            using ReferenceProvider = std::function<void(const ReferenceEmitter&)>;
            static uint32_t AddReferenceProvider(ReferenceProvider provider);
            static void RemoveReferenceProvider(uint32_t token);

            // Fired BEFORE a slot is destroyed/replaced; handle is still valid inside the callback.
            using DestroyListener = std::function<void(TextureHandle)>;
            static uint32_t AddDestroyListener(DestroyListener listener);
            static void RemoveDestroyListener(uint32_t token);

            static std::string GetTextureName(TextureHandle handle) {
                if (handle.index >= s_Textures.size() || !s_Textures[handle.index].IsValid
                    || s_Textures[handle.index].Generation != handle.generation)
                    return "";

                const std::string& fullName = s_Textures[handle.index].Name;

                // Try stripping any known texture root prefix
                auto tryStrip = [&](const std::string& root) -> std::string {
                    if (root.empty() || fullName.size() <= root.size()) return "";
                    if (fullName.compare(0, root.size(), root) != 0) return "";
                    size_t start = root.size();
                    if (start < fullName.size() && (fullName[start] == '/' || fullName[start] == '\\'))
                        start++;
                    return fullName.substr(start);
                };

                // Try primary root first
                std::string rel = tryStrip(s_RootPath);
                if (!rel.empty()) return rel;

                // Try user Assets/Textures as fallback
                std::string base = Path::ExecutableDir();
                rel = tryStrip(Path::Combine(base, "Assets", "Textures"));
                if (!rel.empty()) return rel;

                return fullName;
            }

            static bool IsValid(TextureHandle handle) {
                // Guard against callers holding handles past manager lifetime (shutdown / scene teardown).
                if (!s_IsInitialized) return false;
                if (handle.index >= s_Textures.size()) return false;
                return s_Textures[handle.index].IsValid &&
                    s_Textures[handle.index].Generation == handle.generation;
            }

            static std::size_t GetTotalTextureMemoryBytes();

            // ── Runtime / procedural textures ───────────────────────────
            // Script-created textures that have no file on disk. They live in
            // the same slot pool as file textures but are keyed by a synthetic
            // UUID (high reserved range) instead of a path, so SpriteRenderer /
            // Image references resolve through the normal LoadTextureByUUID
            // path. The C# side owns the CPU pixel buffer; UpdateRuntimeTexture
            // re-uploads it on Apply().
            //
            // CreateRuntimeTexture allocates the GPU texture from `rgba`
            // (width*height*4 bytes) and returns the synthetic UUID (0 on
            // failure). Pass that UUID to SpriteRenderer.Texture etc.
            static uint64_t CreateRuntimeTexture(int width, int height, const uint8_t* rgba,
                Filter filter = Filter::Point, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            // Re-upload the full image. byteCount must equal width*height*4.
            static bool UpdateRuntimeTexture(uint64_t runtimeUuid, const uint8_t* rgba, size_t byteCount);
            // Frees the slot + drops the synthetic UUID mapping. Idempotent.
            static void DestroyRuntimeTexture(uint64_t runtimeUuid);
            // True when the UUID is in the reserved runtime range (lets the
            // asset layer skip file-path resolution for these).
            static bool IsRuntimeTextureUUID(uint64_t uuid);

		private:
			static TextureHandle FindTextureByPath(const std::string& path, Filter filter, Wrap u, Wrap v);
			static TextureHandle FindTextureByPath(const std::string& path);
			static void LoadDefaultTextures();

            static std::array<std::string, 9> s_DefaultTextures;
            static std::vector<TextureEntry> s_Textures;
            static std::queue<uint16_t> s_FreeIndices;
            static bool s_IsInitialized;

            static std::string s_RootPath;

            friend class Renderer2D;
        };
}

namespace std {
    template<>
    struct hash<Index::TextureHandle> {
        size_t operator()(const Index::TextureHandle& h) const noexcept {
            return (static_cast<size_t>(h.index) << 16) ^ static_cast<size_t>(h.generation);
        }
    };
}
