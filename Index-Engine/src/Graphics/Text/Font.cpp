#include "pch.hpp"
#include "Graphics/Text/Font.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Serialization/File.hpp"

#include <webgpu/webgpu_cpp.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <utility>


namespace Index {

	namespace {
		struct CodepointRange { uint32_t First; uint32_t Last; };
		constexpr CodepointRange k_BakedRanges[] = {
			{ 32,  126 },  // ASCII printable
			{ 160, 255 },  // Latin-1 supplement
		};
		constexpr int k_CodepointRangeCount =
			static_cast<int>(sizeof(k_BakedRanges) / sizeof(k_BakedRanges[0]));

		constexpr int ComputeBakedCodepointCount() {
			int total = 0;
			for (const auto& r : k_BakedRanges) total += int(r.Last - r.First + 1);
			return total;
		}
		constexpr int k_CodepointCount = ComputeBakedCodepointCount();

		constexpr int k_AtlasInitialSide = 256;
		constexpr int k_AtlasMaxSide     = 4096;

		// ── Pool ────────────────────────────────────────────────────────────
		struct GpuFontAtlas {
			wgpu::Texture     Texture;
			wgpu::TextureView View;
		};
		std::unordered_map<unsigned, GpuFontAtlas> g_Atlases;
		unsigned g_NextAtlasId = 1;

		unsigned RegisterAtlas(GpuFontAtlas&& atlas) {
			unsigned id = g_NextAtlasId++;
			if (id == 0) id = g_NextAtlasId++;
			g_Atlases.emplace(id, std::move(atlas));
			return id;
		}
		void DeregisterAtlas(unsigned id) {
			if (id == 0) return;
			g_Atlases.erase(id);
		}
		GpuFontAtlas* TryLookupAtlas(unsigned id) {
			if (id == 0) return nullptr;
			auto it = g_Atlases.find(id);
			return (it == g_Atlases.end()) ? nullptr : &it->second;
		}

		struct CpuBakeResult {
			std::vector<uint8_t> Bitmap;
			std::vector<stbtt_packedchar> Packed;
			std::vector<uint8_t> StbFontInfoStorage;  // raw bytes, copied back into Font
			std::vector<uint8_t> TtfBuffer;           // ownership returns to Font after publish
			std::string Filepath;
			int   AtlasSide = 0;
			float PixelSize = 0.0f;
			float Ascent = 0.0f;
			float Descent = 0.0f;
			float LineHeight = 0.0f;
			float StbScale = 0.0f;
			bool  Ok = false;  // worker sets to true on successful pack
		};

		// CPU rasterization shared between the synchronous BakeAtlas path and
		// the async worker. Touches no engine state — pure stbtt work against
		// the provided TTF buffer. Fills `out` on success.
		bool RasterizeAtlasCpu(const std::vector<uint8_t>& ttfBuffer,
			float pixelSize, CpuBakeResult& out)
		{
			if (ttfBuffer.empty() || pixelSize <= 0.0f) return false;

			out.StbFontInfoStorage.assign(sizeof(stbtt_fontinfo), 0);
			stbtt_fontinfo* font = reinterpret_cast<stbtt_fontinfo*>(out.StbFontInfoStorage.data());
			if (!stbtt_InitFont(font, ttfBuffer.data(),
				stbtt_GetFontOffsetForIndex(ttfBuffer.data(), 0)))
			{
				return false;
			}

			const float scale = stbtt_ScaleForPixelHeight(font, pixelSize);
			int ascent = 0, descent = 0, lineGap = 0;
			stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
			out.StbScale   = scale;
			out.PixelSize  = pixelSize;
			out.Ascent     = ascent * scale;
			out.Descent    = descent * scale;
			out.LineHeight = (ascent - descent + lineGap) * scale;

			out.Packed.assign(k_CodepointCount, stbtt_packedchar{});

			// Two-tier bake: 2× oversample at sizes ≤ 48 px for sharper text,
			// 1× otherwise. Same ladder as the synchronous path.
			constexpr float k_OversampleSkipThresholdPx = 48.0f;
			const std::initializer_list<int> oversampleTiers = (pixelSize > k_OversampleSkipThresholdPx)
				? std::initializer_list<int>{ 1 }
				: std::initializer_list<int>{ 2, 1 };

			for (int oversample : oversampleTiers) {
				int side = k_AtlasInitialSide;
				while (side <= k_AtlasMaxSide) {
					out.Bitmap.assign(static_cast<size_t>(side) * static_cast<size_t>(side), 0);
					stbtt_pack_context pctx{};
					if (!stbtt_PackBegin(&pctx, out.Bitmap.data(), side, side, 0, 1, nullptr)) break;
					stbtt_PackSetOversampling(&pctx, oversample, oversample);

					stbtt_pack_range packRanges[k_CodepointRangeCount]{};
					int cumulative = 0;
					for (int r = 0; r < k_CodepointRangeCount; ++r) {
						const auto& cr = k_BakedRanges[r];
						const int count = int(cr.Last - cr.First + 1);
						packRanges[r].font_size = pixelSize;
						packRanges[r].first_unicode_codepoint_in_range = int(cr.First);
						packRanges[r].array_of_unicode_codepoints = nullptr;
						packRanges[r].num_chars = count;
						packRanges[r].chardata_for_range = out.Packed.data() + cumulative;
						cumulative += count;
					}
					const int packResult = stbtt_PackFontRanges(&pctx, ttfBuffer.data(), 0,
						packRanges, k_CodepointRangeCount);
					stbtt_PackEnd(&pctx);

					if (packResult) {
						out.AtlasSide = side;
						out.Ok = true;
						return true;
					}
					side *= 2;
				}
			}
			return false;
		}
	}

	struct FontAsyncBakeState {
		std::thread                       Worker;
		std::shared_ptr<CpuBakeResult>    Result;
		std::atomic<bool>                 Ready{ false };
	};

	namespace WebGPUBackend {
		wgpu::TextureView LookupFontAtlas(unsigned atlasId) {
			GpuFontAtlas* slot = TryLookupAtlas(atlasId);
			return slot ? slot->View : wgpu::TextureView{};
		}
	}

	Font::Font() = default;

	Font::~Font() {
		Cleanup();
	}

	Font::Font(Font&& other) noexcept
		: m_TtfBuffer(std::move(other.m_TtfBuffer))
		, m_Glyphs(std::move(other.m_Glyphs))
		, m_AtlasTexture(other.m_AtlasTexture)
		, m_AtlasWidth(other.m_AtlasWidth)
		, m_AtlasHeight(other.m_AtlasHeight)
		, m_PixelSize(other.m_PixelSize)
		, m_Ascent(other.m_Ascent)
		, m_Descent(other.m_Descent)
		, m_LineHeight(other.m_LineHeight)
		, m_StbScale(other.m_StbScale)
		, m_StbFontInfoStorage(std::move(other.m_StbFontInfoStorage))
		, m_Filepath(std::move(other.m_Filepath))
		, m_AsyncBake(std::move(other.m_AsyncBake))
	{
		other.m_AtlasTexture = 0;
		other.m_AtlasWidth = 0;
		other.m_AtlasHeight = 0;
		other.m_PixelSize = 0.0f;
		other.m_Ascent = 0.0f;
		other.m_Descent = 0.0f;
		other.m_LineHeight = 0.0f;
		other.m_StbScale = 0.0f;
	}

	Font& Font::operator=(Font&& other) noexcept {
		if (this == &other) return *this;
		Cleanup();
		m_TtfBuffer = std::move(other.m_TtfBuffer);
		m_Glyphs = std::move(other.m_Glyphs);
		m_AtlasTexture = other.m_AtlasTexture;
		m_AtlasWidth = other.m_AtlasWidth;
		m_AtlasHeight = other.m_AtlasHeight;
		m_PixelSize = other.m_PixelSize;
		m_Ascent = other.m_Ascent;
		m_Descent = other.m_Descent;
		m_LineHeight = other.m_LineHeight;
		m_StbScale = other.m_StbScale;
		m_StbFontInfoStorage = std::move(other.m_StbFontInfoStorage);
		m_Filepath = std::move(other.m_Filepath);
		m_AsyncBake = std::move(other.m_AsyncBake);
		other.m_AtlasTexture = 0;
		other.m_AtlasWidth = 0;
		other.m_AtlasHeight = 0;
		other.m_PixelSize = 0.0f;
		other.m_Ascent = 0.0f;
		other.m_Descent = 0.0f;
		other.m_LineHeight = 0.0f;
		other.m_StbScale = 0.0f;
		return *this;
	}

	bool Font::LoadFromFile(const std::string& path, float pixelSize) {
		if (path.empty() || pixelSize <= 0.0f) return false;
		Cleanup();
		if (!File::Exists(path)) {
			IDX_CORE_ERROR_TAG("Font", "TTF not found: {}", path);
			return false;
		}
		m_TtfBuffer = File::ReadAllBytes(path);
		if (m_TtfBuffer.empty()) {
			IDX_CORE_ERROR_TAG("Font", "TTF empty / unreadable: {}", path);
			return false;
		}
		m_Filepath = path;
		return BakeAtlas(pixelSize);
	}

	bool Font::LoadFromBuffer(const std::string& sourcePath,
		const std::vector<uint8_t>& ttfBuffer, float pixelSize)
	{
		if (ttfBuffer.empty() || pixelSize <= 0.0f) return false;
		Cleanup();
		m_TtfBuffer = ttfBuffer;
		m_Filepath = sourcePath;
		return BakeAtlas(pixelSize);
	}

	namespace {
		bool PublishAtlasGpu(const CpuBakeResult& result,
			unsigned& outAtlasTexture, int& outAtlasSide)
		{
			if (!result.Ok) return false;

			wgpu::Device device = WebGPUBackend::GetDevice();
			wgpu::Queue  queue  = WebGPUBackend::GetQueue();
			if (!device || !queue) {
				IDX_CORE_ERROR_TAG("Font", "Device / queue unavailable while publishing atlas: {}",
					result.Filepath);
				return false;
			}

			// Upload as R8Unorm (single-channel coverage). Power-of-two atlas
			// sides ≥ 256 already satisfy WebGPU's COPY_BYTES_PER_ROW_ALIGNMENT
			// (256-byte) requirement, so bytesPerRow = AtlasSide is valid as-is.
			wgpu::TextureDescriptor texDesc{};
			texDesc.dimension       = wgpu::TextureDimension::e2D;
			texDesc.size            = { static_cast<uint32_t>(result.AtlasSide),
			                            static_cast<uint32_t>(result.AtlasSide), 1 };
			texDesc.format          = wgpu::TextureFormat::R8Unorm;
			texDesc.usage           = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
			texDesc.mipLevelCount   = 1;
			texDesc.sampleCount     = 1;
			texDesc.viewFormatCount = 0;
			texDesc.label           = "font-atlas";

			wgpu::Texture texture = device.CreateTexture(&texDesc);
			if (!texture) {
				IDX_CORE_ERROR_TAG("Font", "CreateTexture failed for atlas: {}", result.Filepath);
				return false;
			}

			// Dawn renamed these structs mid-2025: ImageCopyTexture ->
			// TexelCopyTextureInfo, TextureDataLayout -> TexelCopyBufferLayout.
			wgpu::TexelCopyTextureInfo dst{};
			dst.texture  = texture;
			dst.mipLevel = 0;
			dst.origin   = { 0, 0, 0 };
			dst.aspect   = wgpu::TextureAspect::All;

			wgpu::TexelCopyBufferLayout layout{};
			layout.offset       = 0;
			layout.bytesPerRow  = static_cast<uint32_t>(result.AtlasSide);
			layout.rowsPerImage = static_cast<uint32_t>(result.AtlasSide);

			wgpu::Extent3D writeSize{ static_cast<uint32_t>(result.AtlasSide),
			                          static_cast<uint32_t>(result.AtlasSide), 1 };
			const size_t byteCount = static_cast<size_t>(result.AtlasSide)
				* static_cast<size_t>(result.AtlasSide);

			queue.WriteTexture(&dst, result.Bitmap.data(), byteCount, &layout, &writeSize);

			wgpu::TextureViewDescriptor viewDesc{};
			viewDesc.format          = wgpu::TextureFormat::R8Unorm;
			viewDesc.dimension       = wgpu::TextureViewDimension::e2D;
			viewDesc.mipLevelCount   = 1;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect          = wgpu::TextureAspect::All;
			wgpu::TextureView view   = texture.CreateView(&viewDesc);
			if (!view) {
				IDX_CORE_ERROR_TAG("Font", "Atlas view creation failed: {}", result.Filepath);
				return false;
			}

			GpuFontAtlas atlas;
			atlas.Texture = std::move(texture);
			atlas.View    = std::move(view);
			outAtlasTexture = RegisterAtlas(std::move(atlas));
			outAtlasSide    = result.AtlasSide;
			return true;
		}

		// Translate stb's packed[] table into a Font's m_Glyphs map. Shared
		// between the sync and async publish paths.
		void PopulateGlyphTable(const std::vector<stbtt_packedchar>& packed,
			int atlasSide, std::unordered_map<uint32_t, GlyphMetrics>& outGlyphs)
		{
			int packedIndex = 0;
			for (int r = 0; r < k_CodepointRangeCount; ++r) {
				const auto& cr = k_BakedRanges[r];
				const int count = int(cr.Last - cr.First + 1);
				for (int i = 0; i < count; ++i) {
					const stbtt_packedchar& pc = packed[packedIndex++];
					GlyphMetrics m;
					m.U0 = float(pc.x0) / float(atlasSide);
					m.V0 = float(pc.y0) / float(atlasSide);
					m.U1 = float(pc.x1) / float(atlasSide);
					m.V1 = float(pc.y1) / float(atlasSide);
					m.Width    = pc.xoff2 - pc.xoff;
					m.Height   = pc.yoff2 - pc.yoff;
					m.XOffset  = pc.xoff;
					m.YOffset  = pc.yoff;
					m.XAdvance = pc.xadvance;
					outGlyphs[cr.First + uint32_t(i)] = m;
				}
			}
		}
	}

	bool Font::BakeAtlas(float pixelSize) {
		if (m_TtfBuffer.empty() || pixelSize <= 0.0f) return false;
		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("Font", "BakeAtlas called before WebGPU backend initialized: {}",
				m_Filepath);
			m_TtfBuffer.clear();
			return false;
		}

		CpuBakeResult result;
		result.Filepath = m_Filepath;
		if (!RasterizeAtlasCpu(m_TtfBuffer, pixelSize, result)) {
			IDX_CORE_ERROR_TAG("Font", "Atlas pack failed (max {}px): {}", k_AtlasMaxSide, m_Filepath);
			m_StbFontInfoStorage.clear();
			m_TtfBuffer.clear();
			return false;
		}

		unsigned atlasTexture = 0;
		int      atlasSide    = 0;
		if (!PublishAtlasGpu(result, atlasTexture, atlasSide)) {
			m_StbFontInfoStorage.clear();
			m_TtfBuffer.clear();
			return false;
		}

		m_AtlasWidth         = atlasSide;
		m_AtlasHeight        = atlasSide;
		m_PixelSize          = result.PixelSize;
		m_Ascent             = result.Ascent;
		m_Descent            = result.Descent;
		m_LineHeight         = result.LineHeight;
		m_StbScale           = result.StbScale;
		m_StbFontInfoStorage = std::move(result.StbFontInfoStorage);
		PopulateGlyphTable(result.Packed, atlasSide, m_Glyphs);
		m_AtlasTexture       = atlasTexture;  // last: makes IsLoaded() flip true
		return true;
	}

	bool Font::BeginAsyncBake(const std::string& sourcePath,
		std::vector<uint8_t> ttfBuffer, float pixelSize)
	{
		if (ttfBuffer.empty() || pixelSize <= 0.0f) return false;
		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("Font", "BeginAsyncBake called before WebGPU backend initialized: {}",
				sourcePath);
			return false;
		}
		Cleanup();
		m_Filepath = sourcePath;

		m_AsyncBake = std::make_unique<FontAsyncBakeState>();
		m_AsyncBake->Result = std::make_shared<CpuBakeResult>();
		m_AsyncBake->Result->Filepath = sourcePath;
		m_AsyncBake->Result->TtfBuffer = std::move(ttfBuffer);

		// Capture shared state by value into the worker so Font teardown
		// doesn't dangle the worker's writes.
		std::shared_ptr<CpuBakeResult> result = m_AsyncBake->Result;
		std::atomic<bool>* ready              = &m_AsyncBake->Ready;
		m_AsyncBake->Worker = std::thread([result, ready, pixelSize]() {
			RasterizeAtlasCpu(result->TtfBuffer, pixelSize, *result);
			ready->store(true, std::memory_order_release);
		});
		return true;
	}

	bool Font::PollAsyncBake() {
		if (!m_AsyncBake) return false;
		if (!m_AsyncBake->Ready.load(std::memory_order_acquire)) return false;

		// Worker is done; reclaim it before touching its result.
		if (m_AsyncBake->Worker.joinable()) m_AsyncBake->Worker.join();
		auto state  = std::move(m_AsyncBake);
		auto result = state->Result;

		if (!result->Ok) {
			IDX_CORE_ERROR_TAG("Font", "Async atlas pack failed (max {}px): {}",
				k_AtlasMaxSide, result->Filepath);
			return true;  // bake is "done" — failed, but no longer pending
		}

		unsigned atlasTexture = 0;
		int      atlasSide    = 0;
		if (!PublishAtlasGpu(*result, atlasTexture, atlasSide)) {
			return true;
		}

		m_TtfBuffer          = std::move(result->TtfBuffer);
		m_AtlasWidth         = atlasSide;
		m_AtlasHeight        = atlasSide;
		m_PixelSize          = result->PixelSize;
		m_Ascent             = result->Ascent;
		m_Descent            = result->Descent;
		m_LineHeight         = result->LineHeight;
		m_StbScale           = result->StbScale;
		m_StbFontInfoStorage = std::move(result->StbFontInfoStorage);
		PopulateGlyphTable(result->Packed, atlasSide, m_Glyphs);
		m_AtlasTexture       = atlasTexture;  // last: flips IsLoaded() true
		return true;
	}

	bool Font::IsBakingAsync() const {
		return m_AsyncBake != nullptr;
	}

	const GlyphMetrics* Font::GetGlyph(uint32_t codepoint) const {
		auto it = m_Glyphs.find(codepoint);
		return it == m_Glyphs.end() ? nullptr : &it->second;
	}

	float Font::GetKerning(uint32_t a, uint32_t b) const {
		if (m_StbFontInfoStorage.empty() || m_StbScale == 0.0f) return 0.0f;
		const stbtt_fontinfo* font = reinterpret_cast<const stbtt_fontinfo*>(m_StbFontInfoStorage.data());
		int kern = stbtt_GetCodepointKernAdvance(const_cast<stbtt_fontinfo*>(font),
			static_cast<int>(a), static_cast<int>(b));
		return kern * m_StbScale;
	}

	void Font::Cleanup() {
		// Join the worker before teardown to avoid an unjoined thread if Font is destroyed mid-bake (worker holds shared_ptr to its result, but join makes teardown deterministic).
		if (m_AsyncBake) {
			if (m_AsyncBake->Worker.joinable()) m_AsyncBake->Worker.join();
			m_AsyncBake.reset();
		}
		if (m_AtlasTexture != 0) {
			DeregisterAtlas(m_AtlasTexture);
			m_AtlasTexture = 0;
		}
		m_TtfBuffer.clear();
		m_Glyphs.clear();
		m_AtlasWidth = 0;
		m_AtlasHeight = 0;
		m_PixelSize = 0.0f;
		m_Ascent = 0.0f;
		m_Descent = 0.0f;
		m_LineHeight = 0.0f;
		m_StbScale = 0.0f;
		m_StbFontInfoStorage.clear();
		m_Filepath.clear();
	}

}  // namespace Index
