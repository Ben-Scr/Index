#pragma once
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/ImageData.hpp"
#include "Graphics/Wrap.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Index {
	class INDEX_API Texture2D {
	public:
		Texture2D() = default;
		Texture2D(const char* path,
			Filter filter = Filter::Point,
			Wrap wrapU = Wrap::Clamp,
			Wrap wrapV = Wrap::Clamp,
			bool generateMipmaps = true,
			bool srgb = false,
			bool flipVertical = true);

		~Texture2D();

		Texture2D(const Texture2D&) = delete;
		Texture2D& operator=(const Texture2D&) = delete;
		Texture2D(Texture2D&&) noexcept;
		Texture2D& operator=(Texture2D&&) noexcept;

		void Destroy();

		bool Load(const char* path,
			bool generateMipmaps = true,
			bool srgb = false,
			bool flipVertical = true);

		// Create a GPU texture from raw RGBA8 bytes (w*h*4). Used by the
		// runtime/procedural texture path (script-created textures). The
		// texture is created with CopyDst usage so UploadPixels can re-upload
		// after the CPU-side buffer changes.
		bool CreateFromPixels(int width, int height, const uint8_t* rgba,
			Filter filter = Filter::Point, Wrap wrapU = Wrap::Clamp, Wrap wrapV = Wrap::Clamp);

		// Re-upload the full image to the existing GPU texture. byteCount must
		// equal width*height*4. No-op (returns false) if the texture wasn't
		// created with CreateFromPixels / a CopyDst-capable path.
		bool UploadPixels(const uint8_t* rgba, size_t byteCount);

		void Submit(uint8_t unit) const;

		void SetFilter(Filter filter);
		void SetWrapU(Wrap u);
		void SetWrapV(Wrap v);
		void SetSampler(Filter filter, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);

		Filter GetFilter() const { return m_Filter; }
		Wrap GetWrapU() const { return m_WrapU; }
		Wrap GetWrapV() const { return m_WrapV; }

		bool IsValid() const { return m_Tex != 0; }

		std::unique_ptr<ImageData> GetImageData() const;

		static std::unique_ptr<ImageData> DecodeFileToCpu(const char* path,
			bool flipVertical = false);
		// Returns the raw WGPUTextureView pointer cast to uint64_t — ImGui reinterpret-casts ImTextureID directly to WGPUTextureView, so this must not be a pool index.
		uint64_t GetHandle() const { return m_Tex; }
		Vec2 Size() const { return Vec2{ float(m_Width), float(m_Height) }; }
		float GetWidth() const { return m_Width; }
		float GetHeight() const { return m_Height; }

		float AspectRatio() const { return m_Height != 0 ? float(m_Width) / float(m_Height) : 0.0f; }

		bool IsFlippedY() const { return m_FlippedY; }

	private:
		uint64_t m_Tex = 0;
		int m_Width = 0, m_Height = 0, m_Channels = 0;

		Filter m_Filter = Filter::Point;
		Wrap   m_WrapU = Wrap::Clamp;
		Wrap   m_WrapV = Wrap::Clamp;
		bool   m_HasMips = true;
		bool   m_FlippedY = false;
		std::string m_SourcePath;
		void ApplySamplerParams() const;
	};

} // namespace Index
