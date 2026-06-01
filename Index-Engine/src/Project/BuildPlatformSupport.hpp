#pragma once
#include "Core/Export.hpp"
#include "Project/IndexBuildProfile.hpp"

#include <string>

namespace Index {

	// Hard-coded stubs until editor platform-packages exist; replace with a PackageHost query when Pkg.Index.Platform.<Name> packages land.
	struct INDEX_API BuildPlatformSupport {
		static bool IsAvailable(BuildPlatform platform);

		// Human-readable explanation for the warning row. Empty when the
		// platform is available.
		static std::string UnavailableReason(BuildPlatform platform);
	};

} // namespace Index
