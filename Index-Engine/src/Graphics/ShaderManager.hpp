#pragma once
#include "Core/Export.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/ShaderEntry.hpp"
#include "Graphics/ShaderHandle.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

    class INDEX_API ShaderManager {
    public:
        static void Initialize();
        static void Shutdown();
        static bool IsInitialized() { return s_IsInitialized; }

        static ShaderHandle LoadShader(const std::string_view& vsPath, const std::string_view& fsPath);
        static ShaderHandle LoadShaderByUUID(uint64_t vsAssetId, uint64_t fsAssetId);

        static void UnloadShader(ShaderHandle handle);
        static Shader* GetShader(ShaderHandle handle);
        static bool IsValid(ShaderHandle handle);
        static ShaderHandle GetShaderHandle(const std::string& vsPath, const std::string& fsPath);

        // On failure, existing shader is kept so renderers don't break mid-frame.
        static bool ReloadShader(ShaderHandle handle);

        // Reload every entry whose vs OR fs path matches (canonicalized).
        // Returns the count of entries successfully reloaded.
        static size_t ReloadShaderPath(const std::string& path);

        // Reload every cached shader from disk. Used by the editor on a
        // manual "Reload shaders" command.
        static size_t ReloadShadersFromDisk();

        static std::vector<ShaderHandle> GetLoadedHandles();
        static void UnloadAll();

        static size_t PurgeUnreferenced();

        using ReferenceEmitter = std::function<void(ShaderHandle)>;
        using ReferenceProvider = std::function<void(const ReferenceEmitter&)>;
        static uint32_t AddReferenceProvider(ReferenceProvider provider);
        static void RemoveReferenceProvider(uint32_t token);

    private:
        static ShaderHandle FindShaderByPaths(const std::string& vsPath, const std::string& fsPath);

        static std::vector<ShaderEntry> s_Shaders;
        static std::queue<uint16_t> s_FreeIndices;
        static bool s_IsInitialized;
    };

} // namespace Index
