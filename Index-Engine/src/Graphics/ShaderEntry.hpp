#pragma once

#include "Graphics/Shader.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Index {

    // unique_ptr<Shader> so vector growth never move-constructs a live GPU object.
    struct ShaderEntry {
        ShaderEntry() = default;
        ShaderEntry(ShaderEntry&&) noexcept = default;
        ShaderEntry& operator=(ShaderEntry&&) noexcept = default;
        ShaderEntry(const ShaderEntry&) = delete;
        ShaderEntry& operator=(const ShaderEntry&) = delete;

        std::unique_ptr<Shader> Shader;
        uint16_t Generation = 0;
        std::string VsPath;
        std::string FsPath;
        bool IsValid = false;
    };

} // namespace Index
