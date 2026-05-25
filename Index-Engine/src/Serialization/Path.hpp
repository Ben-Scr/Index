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

		/// Convert an absolute filesystem path to a short, user-facing display
		/// string anchored at a recognised project / engine root. Used by the
		/// editor anywhere a tooltip or secondary label would otherwise leak
		/// the user's full machine-local path.
		///   • Inside the active project root  -> "Assets/Square.png",
		///                                        "IndexAssets/Textures/icon.png",
		///                                        "Packages/Foo/...", etc.
		///   • Inside the engine's IndexAssets -> "IndexAssets/<rest>"
		///                                        (covers built-in fonts/icons
		///                                        when no project is loaded).
		///   • Anything outside both           -> the filename only.
		/// Always uses forward slashes for display. Empty input yields empty output.
		static std::string ToProjectRelativeDisplay(const std::string& absolutePath);
	};
}
