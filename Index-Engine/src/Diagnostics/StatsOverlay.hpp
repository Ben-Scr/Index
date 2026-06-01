#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// NOT INDEX_API: each consumer (Editor, Runtime) compiles the .cpp directly; the engine DLL excludes it (see premake5.lua) so INDEX_API would cause LNK2019.
struct ImVec2;

namespace Index::Diagnostics {

	class StatsOverlay {
	public:
		struct Snapshot {
			float Fps              = 0.0f;
			float CpuMainMs        = 0.0f;
			float RenderThreadMs   = 0.0f;
			std::size_t Tris       = 0;
			std::size_t Verts      = 0;
			int   ScreenWidth      = 0;
			int   ScreenHeight     = 0;
			std::size_t MemAllocBytes    = 0; // process working set (resident)
			std::size_t MemReservedBytes = 0; // committed virtual memory
			std::uint32_t AudioPlaying   = 0;
			std::size_t Entities         = 0;
		};

		bool RefreshIfDue(int screenWidth, int screenHeight);

		const Snapshot& GetCached() const { return m_Cached; }

		float RenderInRect(const ImVec2& imageMin, const ImVec2& imageMax, float yOffset = 0.0f) const;
		float RenderInMainViewport(float yOffset = 0.0f) const;

	private:
		void RenderBody() const; // shared body of RenderInRect / RenderInMainViewport
		float RenderOverlayWindow(const char* uniqueId, const ImVec2& topRight, float yOffset) const;

		Snapshot m_Cached{};
		std::chrono::steady_clock::time_point m_LastRefresh{};
	};

} // namespace Index::Diagnostics
