#pragma once

#include "Core/Export.hpp"

#include <string>

namespace Index {
	struct IndexProject;

	// Centralises the three stored-path forms (project-relative, "index:<sub>", absolute) for splash/icon assets.
	namespace SplashAssetResolve {
		INDEX_API std::string Resolve(const std::string& stored, const IndexProject* project);
		INDEX_API std::string NormalizeForStorage(const std::string& pickedAbsolute, const IndexProject* project);
		INDEX_API std::string DefaultLogoPath();
		// Returns "Index <version> · <platform> · <profile>"; each binary compiles its own values.
		INDEX_API std::string DefaultSubtitleLine();
	}
}
