#pragma once
#include "Vec2.hpp"
#include "Core/Export.hpp"
#include "Math/Common.hpp"

namespace Index {
	// Width/Height = logical sub-rect (letterboxed when aspect lock is on); FramebufferWidth/Height = full OS surface.
	class INDEX_API Viewport {
	public:
		Viewport() = default;
		Viewport(int width, int height)
			: Width{ width }, Height{ height },
			  FramebufferWidth{ width }, FramebufferHeight{ height } {}

		void SetSize(int width, int height) { Width = width, Height = height; }
		Vec2Int GetSize() const noexcept { return Vec2Int{ Width, Height }; }
		float GetAspect() const noexcept { return Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f; }
		Vec2  GetHalfSize() const noexcept { return Vec2(0.5f * Width, 0.5f * Height); }
		Vec2  GetCenter() const noexcept { return Vec2(0.5f * Width, 0.5f * Height); }

		void SetWidth(int width) { Width = Max(1, width); }
		void SetHeight(int height) { Height = Max(1, height); }

		int GetWidth() const { return Width; }
		int GetHeight() const { return Height; }

		// Offset of the logical sub-rect within the framebuffer. Non-zero
		// only when the Window's aspect lock kicks in; both axes are
		// always centered, so exactly one is non-zero at a time.
		int GetOffsetX() const noexcept { return OffsetX; }
		int GetOffsetY() const noexcept { return OffsetY; }
		Vec2Int GetOffset() const noexcept { return Vec2Int{ OffsetX, OffsetY }; }

		int GetFramebufferWidth() const noexcept { return FramebufferWidth; }
		int GetFramebufferHeight() const noexcept { return FramebufferHeight; }
		Vec2Int GetFramebufferSize() const noexcept { return Vec2Int{ FramebufferWidth, FramebufferHeight }; }

		// True iff the logical sub-rect doesn't cover the whole framebuffer
		// (i.e. there's at least one column or row of letterbox/pillarbox
		// pixels to paint black).
		bool HasLetterbox() const noexcept {
			return OffsetX > 0 || OffsetY > 0
				|| Width < FramebufferWidth || Height < FramebufferHeight;
		}

		void SetLetterboxedSubRect(int fbW, int fbH, int x, int y, int w, int h) {
			FramebufferWidth = Max(1, fbW);
			FramebufferHeight = Max(1, fbH);
			OffsetX = Max(0, x);
			OffsetY = Max(0, y);
			Width = Max(1, w);
			Height = Max(1, h);
		}

	private:
		int   Width = 1920;
		int   Height = 1080;
		int   OffsetX = 0;
		int   OffsetY = 0;
		int   FramebufferWidth = 1920;
		int   FramebufferHeight = 1080;
	};

	inline std::ostream& operator<<(std::ostream& os, const Viewport& vp) {
		return os << vp.GetWidth() << " x " << vp.GetHeight();
	}
}