#include "pch.hpp"
#include "Project/BuildPlatformSupport.hpp"

namespace Index {

	bool BuildPlatformSupport::IsAvailable(BuildPlatform platform) {
		switch (platform) {
			case BuildPlatform::Windows: return true;
			// Every non-Windows platform is gated behind a future editor
			// platform-package install. Until that package system exists,
			// these profiles are surfaced in the UI for planning but the
			// Build button stays disabled.
			case BuildPlatform::Linux:
			case BuildPlatform::MacOS:
			case BuildPlatform::Android:
			case BuildPlatform::IOS:
			case BuildPlatform::VisionOS:
			case BuildPlatform::Web:
				return false;
		}
		return false;
	}

	std::string BuildPlatformSupport::UnavailableReason(BuildPlatform platform) {
		auto notInstalled = [](const char* displayName) {
			return std::string(displayName) + " platform support is not installed. "
				"Install it via the Package Manager once " + displayName +
				" platform packages are available.";
		};
		switch (platform) {
			case BuildPlatform::Windows:  return {};
			case BuildPlatform::Linux:    return notInstalled("Linux");
			case BuildPlatform::MacOS:    return notInstalled("macOS");
			case BuildPlatform::Android:  return notInstalled("Android");
			case BuildPlatform::IOS:      return notInstalled("iOS");
			case BuildPlatform::VisionOS: return notInstalled("visionOS");
			case BuildPlatform::Web:      return notInstalled("Web");
		}
		return "Unknown platform.";
	}

} // namespace Index
