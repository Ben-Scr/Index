#pragma once
#include "Graphics/Texture2D.hpp"
#include "Serialization/Path.hpp"
#include "Core/Export.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Index {
	class EditorIcons {
	public:
		static void Initialize();
		static void Shutdown();

		/// Returns WGPUTextureView cast to uint64_t for ImGui::Image; size snaps to nearest of 16,24,32,48,64,128,256,512.
		static uint64_t Get(const std::string& name, int size = 32);

	private:
		struct IconEntry {
			Texture2D Texture;
			int Size = 0;
		};

		static std::string MakeKey(const std::string& name, int size);
		static int SnapSize(int requested);

		static std::unordered_map<std::string, IconEntry> s_Icons;
		static bool s_Initialized;
	};
}
