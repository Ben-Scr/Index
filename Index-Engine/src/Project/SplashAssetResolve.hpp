#pragma once

#include "Core/Export.hpp"

#include <string>

namespace Index {
	struct IndexProject;

	// Splash-screen and app-icon asset paths can come from three places:
	//   1. The project's own Assets/ tree            → stored project-relative ("Assets/foo.png")
	//   2. The engine-shipped IndexAssets/ tree      → stored as "index:<subpath>"
	//      where <subpath> is the path under IndexAssets/ (e.g. "index:Textures/icon.png")
	//   3. An absolute filesystem path              → stored verbatim (not portable across machines)
	//
	// SplashAssetResolve centralises that storage convention and the inverse
	// resolution. Both the runtime splash layer (Index-Runtime) and the editor's
	// splash preview consume it so the editor's "Show Preview" matches exactly
	// what the shipped game renders, including the engine-shipped defaults.
	namespace SplashAssetResolve {
		// Resolves a stored path (any of the three forms above) to an absolute
		// filesystem path that exists, or an empty string if it can't be
		// located. Pass the active project so project-relative paths can be
		// anchored at RootDirectory / AssetsDirectory.
		INDEX_API std::string Resolve(const std::string& stored, const IndexProject* project);

		// Turns a picker's absolute result back into the portable stored form.
		// Strategy: prefer "index:<sub>" when the path is inside IndexAssets,
		// otherwise project-relative when inside the project's assets tree,
		// falling back to the bare filename for anything else. Empty input
		// passes through.
		INDEX_API std::string NormalizeForStorage(const std::string& pickedAbsolute, const IndexProject* project);

		// Engine-shipped default logo — IndexAssets/Textures/icon.png with a
		// historical Index64.png fallback for older installs. Returns empty
		// when neither exists (running outside a packaged or dev layout).
		INDEX_API std::string DefaultLogoPath();

		// "Index <version>  ·  <platform>  ·  <profile>" — the engine-generated
		// subtitle the splash uses when SplashScreen.CustomText is empty.
		// Each translation unit that includes this header compiles the
		// platform / profile branch matching its own build, so the editor
		// preview shows the editor's compile-time values and the runtime
		// shows its own.
		INDEX_API std::string DefaultSubtitleLine();
	}
}
