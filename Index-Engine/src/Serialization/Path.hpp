#pragma once
#include "Core/Export.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <filesystem>
#include <string>

namespace Index {
	class INDEX_API Path {
	public:
		static std::string GetSpecialFolderPath(SpecialFolder folder);

		template <typename... Args>
		static std::string Combine(Args&&... args) {
			std::filesystem::path combined;
			((combined /= std::filesystem::path(std::forward<Args>(args))), ...);
			return combined.make_preferred().string();
		}

		static std::string Current() {
			return std::filesystem::current_path().string();
		}

		static std::string ExecutableDir();

		/// Resolves a IndexAssets subdirectory (e.g. "Textures", "Shader").
		/// Checks: exeDir/IndexAssets/<sub> (packaged), then exeDir/../IndexAssets/<sub> (dev layout).
		/// Returns empty string if not found.
		static std::string ResolveIndexAssets(const std::string& subdirectory);
	};
}
