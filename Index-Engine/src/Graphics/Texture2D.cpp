#include "pch.hpp"
#include "Graphics/Texture2D.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/ImageData.hpp"

#include <webgpu/webgpu_cpp.h>

#define STB_IMAGE_IMPLEMENTATION
#if defined(IDX_DEBUG)
// Debug builds compile stb_image at -O0; some compilers do not fold the
// SSE2 shift immediates early enough for the intrinsic wrappers.
#define STBI_NO_SIMD
#endif
#include <stb_image.h>

#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

// Texture2D WebGPU impl. m_Tex stores the raw WGPUTextureView pointer as uint64_t — this lets imgui_impl_wgpu receive a real Dawn handle directly from GetHandle() rather than a pool index it would crash dereferencing.

namespace Index {

	namespace {
		// ── Pool ────────────────────────────────────────────────────────────
		struct GpuTexture {
			wgpu::Texture     Texture;
			wgpu::TextureView View;
			wgpu::Sampler     Sampler;
			uint32_t          Width  = 0;
			uint32_t          Height = 0;
			Filter            CachedFilter = Filter::Point;
			Wrap              CachedWrapU  = Wrap::Clamp;
			Wrap              CachedWrapV  = Wrap::Clamp;
		};
		std::unordered_map<uint64_t, GpuTexture> g_Textures;

		uint64_t RegisterTexture(GpuTexture&& tex) {
			const uint64_t key = reinterpret_cast<uint64_t>(tex.View.Get());
			if (key == 0) return 0;  // shouldn't happen; defensive
			g_Textures.emplace(key, std::move(tex));
			return key;
		}

		void FreeTextureSlot(uint64_t id) {
			if (id == 0) return;
			g_Textures.erase(id);
		}

		GpuTexture* TryLookup(uint64_t id) {
			if (id == 0) return nullptr;
			auto it = g_Textures.find(id);
			return (it == g_Textures.end()) ? nullptr : &it->second;
		}

		// ── Enum mapping ────────────────────────────────────────────────────
		using WB = ::Index::WebGPUBackend::FilterMode;
		using WW = ::Index::WebGPUBackend::WrapMode;

		WB ToBackendFilter(Filter f) noexcept {
			switch (f) {
				case Filter::Point:       return WB::Point;
				case Filter::Bilinear:    return WB::Bilinear;
				case Filter::Trilinear:   return WB::Trilinear;
				case Filter::Anisotropic: return WB::Anisotropic;
				case Filter::Default:     return WB::Point;  // sentinel; normally resolved before sampler creation — safety net (no-meta default is Point)
			}
			return WB::Point;
		}
		WW ToBackendWrap(Wrap w) noexcept {
			switch (w) {
				case Wrap::Repeat: return WW::Repeat;
				case Wrap::Clamp:  return WW::Clamp;
				case Wrap::Mirror: return WW::Mirror;
				case Wrap::Border: return WW::Border;
			}
			return WW::Clamp;
		}

		wgpu::Sampler MakeSamplerFor(Filter f, Wrap u, Wrap v) {
			return WebGPUBackend::CreateSampler(
				ToBackendFilter(f), ToBackendWrap(u), ToBackendWrap(v));
		}
	}


	namespace WebGPUBackend {

		TextureLookup LookupTexture2D(uint64_t backendId) {
			GpuTexture* slot = TryLookup(backendId);
			if (!slot) return TextureLookup{};
			TextureLookup out;
			out.View   = slot->View;
			out.Sampler= slot->Sampler;
			out.Width  = slot->Width;
			out.Height = slot->Height;
			out.Valid  = true;
			return out;
		}

		wgpu::Sampler CreateSampler(FilterMode filter, WrapMode wrapU, WrapMode wrapV) {
			wgpu::Device device = GetDevice();
			if (!device) return nullptr;

			auto toAddress = [](WrapMode w) {
				switch (w) {
					case WrapMode::Repeat: return wgpu::AddressMode::Repeat;
					case WrapMode::Clamp:  return wgpu::AddressMode::ClampToEdge;
					case WrapMode::Mirror: return wgpu::AddressMode::MirrorRepeat;
					case WrapMode::Border: return wgpu::AddressMode::ClampToEdge;  // WebGPU has no border-color mode
				}
				return wgpu::AddressMode::ClampToEdge;
			};

			wgpu::SamplerDescriptor desc{};
			desc.addressModeU = toAddress(wrapU);
			desc.addressModeV = toAddress(wrapV);
			desc.addressModeW = wgpu::AddressMode::ClampToEdge;

			switch (filter) {
				case FilterMode::Point:
					desc.magFilter    = wgpu::FilterMode::Nearest;
					desc.minFilter    = wgpu::FilterMode::Nearest;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Bilinear:
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Trilinear:
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
					desc.maxAnisotropy = 1;
					break;
				case FilterMode::Anisotropic:
					// maxAnisotropy > 1 requires linear filtering on all axes
					// per the spec; the renderer's actual aniso level can be
					// project-driven later (Stage 5 settings panel).
					desc.magFilter    = wgpu::FilterMode::Linear;
					desc.minFilter    = wgpu::FilterMode::Linear;
					desc.mipmapFilter = wgpu::MipmapFilterMode::Linear;
					desc.maxAnisotropy = 16;
					break;
			}

			return device.CreateSampler(&desc);
		}

	}  // namespace WebGPUBackend

	// ── Texture2D ───────────────────────────────────────────────────────────

	Texture2D::Texture2D(const char* path,
		Filter filter, Wrap wrapU, Wrap wrapV,
		bool generateMipmaps, bool srgb, bool flipVertical)
		: m_Filter(filter), m_WrapU(wrapU), m_WrapV(wrapV), m_HasMips(generateMipmaps)
	{
		if (path != nullptr) {
			Load(path, generateMipmaps, srgb, flipVertical);
		}
	}

	Texture2D::~Texture2D() {
		Destroy();
	}

	Texture2D::Texture2D(Texture2D&& other) noexcept
		: m_Tex(other.m_Tex)
		, m_Width(other.m_Width)
		, m_Height(other.m_Height)
		, m_Channels(other.m_Channels)
		, m_Filter(other.m_Filter)
		, m_WrapU(other.m_WrapU)
		, m_WrapV(other.m_WrapV)
		, m_HasMips(other.m_HasMips)
		, m_FlippedY(other.m_FlippedY)
		, m_SourcePath(std::move(other.m_SourcePath))
	{
		other.m_Tex = 0;
		other.m_Width = 0;
		other.m_Height = 0;
		other.m_Channels = 0;
		other.m_SourcePath.clear();
	}

	Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
		if (this == &other) return *this;
		Destroy();
		m_Tex      = other.m_Tex;
		m_Width    = other.m_Width;
		m_Height   = other.m_Height;
		m_Channels = other.m_Channels;
		m_Filter   = other.m_Filter;
		m_WrapU    = other.m_WrapU;
		m_WrapV    = other.m_WrapV;
		m_HasMips  = other.m_HasMips;
		m_FlippedY = other.m_FlippedY;
		m_SourcePath = std::move(other.m_SourcePath);
		other.m_Tex = 0;
		other.m_Width = 0;
		other.m_Height = 0;
		other.m_Channels = 0;
		other.m_SourcePath.clear();
		return *this;
	}

	void Texture2D::Destroy() {
		if (m_Tex != 0) {
			FreeTextureSlot(m_Tex);
			m_Tex = 0;
		}
		m_Width = 0;
		m_Height = 0;
		m_Channels = 0;
		m_SourcePath.clear();
	}

	bool Texture2D::Load(const char* path, bool generateMipmaps, bool srgb, bool flipVertical) {
		Destroy();
		m_FlippedY = flipVertical;
		(void)srgb;  // Stage 2 follow-up — wgpu::TextureFormat::RGBA8UnormSrgb when wired

		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_WARN_TAG("Texture2D",
				"Load called before WebGPU backend initialized: {}", path);
			return false;
		}

		stbi_set_flip_vertically_on_load(flipVertical);
		int w = 0, h = 0, n = 0;
		unsigned char* pixels = stbi_load(path, &w, &h, &n, 4);
		stbi_set_flip_vertically_on_load(false);
		if (!pixels) {
			IDX_CORE_WARN_TAG("Texture2D", "Failed to load texture: {}", path);
			return false;
		}

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();

		wgpu::TextureDescriptor texDesc{};
		texDesc.dimension       = wgpu::TextureDimension::e2D;
		texDesc.size            = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		texDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		texDesc.usage           = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
		texDesc.mipLevelCount   = 1;  // see "What this does NOT do yet" header note
		texDesc.sampleCount     = 1;
		texDesc.viewFormatCount = 0;

		wgpu::Texture texture = device.CreateTexture(&texDesc);
		if (!texture) {
			stbi_image_free(pixels);
			IDX_CORE_WARN_TAG("Texture2D", "wgpu::Device::CreateTexture failed: {}", path);
			return false;
		}

		// Dawn mid-2025 rename: ImageCopyTexture -> TexelCopyTextureInfo, TextureDataLayout -> TexelCopyBufferLayout (layout unchanged). stbi pixels freed immediately after WriteTexture (Dawn has its own staging buffer).
		wgpu::TexelCopyTextureInfo dst{};
		dst.texture  = texture;
		dst.mipLevel = 0;
		dst.origin   = { 0, 0, 0 };
		dst.aspect   = wgpu::TextureAspect::All;

		wgpu::TexelCopyBufferLayout layout{};
		layout.offset        = 0;
		layout.bytesPerRow   = static_cast<uint32_t>(w) * 4u;
		layout.rowsPerImage  = static_cast<uint32_t>(h);

		wgpu::Extent3D writeSize{ static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

		queue.WriteTexture(&dst, pixels, byteCount, &layout, &writeSize);
		stbi_image_free(pixels);

		// Default 2D view spanning all (one) mip levels.
		wgpu::TextureViewDescriptor viewDesc{};
		viewDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		viewDesc.mipLevelCount   = 1;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView view = texture.CreateView(&viewDesc);

		GpuTexture slot;
		slot.Texture      = std::move(texture);
		slot.View         = std::move(view);
		slot.Sampler      = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
		slot.Width        = static_cast<uint32_t>(w);
		slot.Height       = static_cast<uint32_t>(h);
		slot.CachedFilter = m_Filter;
		slot.CachedWrapU  = m_WrapU;
		slot.CachedWrapV  = m_WrapV;

		m_Tex      = RegisterTexture(std::move(slot));
		m_Width    = w;
		m_Height   = h;
		m_Channels = n;
		m_HasMips  = generateMipmaps;
		m_SourcePath = path ? path : "";
		return true;
	}

	bool Texture2D::CreateFromPixels(int width, int height, const uint8_t* rgba,
		Filter filter, Wrap wrapU, Wrap wrapV)
	{
		Destroy();
		if (width <= 0 || height <= 0 || rgba == nullptr) return false;
		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_WARN_TAG("Texture2D", "CreateFromPixels called before WebGPU backend initialized");
			return false;
		}

		m_Filter = filter;
		m_WrapU  = wrapU;
		m_WrapV  = wrapV;
		m_FlippedY = false;

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();

		wgpu::TextureDescriptor texDesc{};
		texDesc.dimension       = wgpu::TextureDimension::e2D;
		texDesc.size            = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		texDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		texDesc.usage           = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
		texDesc.mipLevelCount   = 1;
		texDesc.sampleCount     = 1;
		texDesc.viewFormatCount = 0;

		wgpu::Texture texture = device.CreateTexture(&texDesc);
		if (!texture) {
			IDX_CORE_WARN_TAG("Texture2D", "CreateFromPixels: device.CreateTexture failed");
			return false;
		}

		wgpu::TexelCopyTextureInfo dst{};
		dst.texture  = texture;
		dst.mipLevel = 0;
		dst.origin   = { 0, 0, 0 };
		dst.aspect   = wgpu::TextureAspect::All;

		wgpu::TexelCopyBufferLayout layout{};
		layout.offset       = 0;
		layout.bytesPerRow  = static_cast<uint32_t>(width) * 4u;
		layout.rowsPerImage = static_cast<uint32_t>(height);

		wgpu::Extent3D writeSize{ static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
		const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		queue.WriteTexture(&dst, rgba, byteCount, &layout, &writeSize);

		wgpu::TextureViewDescriptor viewDesc{};
		viewDesc.format          = wgpu::TextureFormat::RGBA8Unorm;
		viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
		viewDesc.mipLevelCount   = 1;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect          = wgpu::TextureAspect::All;
		wgpu::TextureView view = texture.CreateView(&viewDesc);

		GpuTexture slot;
		slot.Texture      = std::move(texture);
		slot.View         = std::move(view);
		slot.Sampler      = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
		slot.Width        = static_cast<uint32_t>(width);
		slot.Height       = static_cast<uint32_t>(height);
		slot.CachedFilter = m_Filter;
		slot.CachedWrapU  = m_WrapU;
		slot.CachedWrapV  = m_WrapV;

		m_Tex      = RegisterTexture(std::move(slot));
		m_Width    = width;
		m_Height   = height;
		m_Channels = 4;
		m_HasMips  = false;
		m_SourcePath.clear();
		return m_Tex != 0;
	}

	bool Texture2D::UploadPixels(const uint8_t* rgba, size_t byteCount) {
		if (rgba == nullptr || !IsValid()) return false;
		const size_t expected = static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height) * 4;
		if (byteCount != expected) {
			IDX_CORE_WARN_TAG("Texture2D",
				"UploadPixels byte count {} != expected {} ({}x{} RGBA8)",
				byteCount, expected, m_Width, m_Height);
			return false;
		}
		GpuTexture* slot = TryLookup(m_Tex);
		if (!slot || !slot->Texture) return false;

		wgpu::Queue queue = WebGPUBackend::GetQueue();

		wgpu::TexelCopyTextureInfo dst{};
		dst.texture  = slot->Texture;
		dst.mipLevel = 0;
		dst.origin   = { 0, 0, 0 };
		dst.aspect   = wgpu::TextureAspect::All;

		wgpu::TexelCopyBufferLayout layout{};
		layout.offset       = 0;
		layout.bytesPerRow  = static_cast<uint32_t>(m_Width) * 4u;
		layout.rowsPerImage = static_cast<uint32_t>(m_Height);

		wgpu::Extent3D writeSize{ static_cast<uint32_t>(m_Width), static_cast<uint32_t>(m_Height), 1 };
		queue.WriteTexture(&dst, rgba, byteCount, &layout, &writeSize);
		return true;
	}

	void Texture2D::Submit(uint8_t /*unit*/) const {}

	// WebGPU samplers are immutable; SetFilter/SetWrapU/V replace the pool entry sampler rather than modifying it.
	void Texture2D::SetFilter(Filter filter) {
		m_Filter = filter;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedFilter = m_Filter;
		}
	}
	void Texture2D::SetWrapU(Wrap u) {
		m_WrapU = u;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedWrapU = m_WrapU;
		}
	}
	void Texture2D::SetWrapV(Wrap v) {
		m_WrapV = v;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedWrapV = m_WrapV;
		}
	}
	void Texture2D::SetSampler(Filter filter, Wrap u, Wrap v) {
		m_Filter = filter;
		m_WrapU  = u;
		m_WrapV  = v;
		if (GpuTexture* slot = TryLookup(m_Tex)) {
			slot->Sampler = MakeSamplerFor(m_Filter, m_WrapU, m_WrapV);
			slot->CachedFilter = m_Filter;
			slot->CachedWrapU  = m_WrapU;
			slot->CachedWrapV  = m_WrapV;
		}
	}
	void Texture2D::ApplySamplerParams() const {}

	std::unique_ptr<ImageData> Texture2D::GetImageData() const {
		if (m_SourcePath.empty()) return nullptr;
		return DecodeFileToCpu(m_SourcePath.c_str(), m_FlippedY);
	}

	std::unique_ptr<ImageData> Texture2D::DecodeFileToCpu(const char* path,
		bool flipVertical)
	{
		if (path == nullptr || *path == '\0') return nullptr;

		stbi_set_flip_vertically_on_load(flipVertical);
		int w = 0, h = 0, n = 0;
		unsigned char* pixels = stbi_load(path, &w, &h, &n, 4);
		stbi_set_flip_vertically_on_load(false);
		if (!pixels) return nullptr;

		const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
		std::vector<unsigned char> owned(byteCount);
		std::memcpy(owned.data(), pixels, byteCount);
		stbi_image_free(pixels);

		return std::make_unique<ImageData>(w, h, std::move(owned));
	}

}  // namespace Index
