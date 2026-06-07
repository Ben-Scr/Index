#include "pch.hpp"
#include "Graphics/GizmoRenderer.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/Text/Font.hpp"
#include "Graphics/Text/FontManager.hpp"
#include "Graphics/Text/TextRenderer.hpp"
#include "Memory/FrameArenas.hpp"

#include <webgpu/webgpu_cpp.h>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>


namespace Index {

	// Static class members (declared in GizmoRenderer.hpp) ────────────────────
	bool                           GizmoRenderer2D::m_IsInitialized = false;
	std::unique_ptr<Shader>        GizmoRenderer2D::m_GizmoShader;
	std::unique_ptr<TextRenderer>  GizmoRenderer2D::m_TextRenderer;
	std::vector<uint32_t>          GizmoRenderer2D::m_GizmoIndices;
	std::vector<GizmoUploadVertex> GizmoRenderer2D::s_UploadBuffer;
	uint16_t                       GizmoRenderer2D::m_GizmoViewId = 1;
	unsigned int                   GizmoRenderer2D::m_VAO = 0;
	unsigned int                   GizmoRenderer2D::m_VBO = 0;
	unsigned int                   GizmoRenderer2D::m_EBO = 0;
	std::size_t                    GizmoRenderer2D::m_VBOCapacityBytes = 0;
	std::size_t                    GizmoRenderer2D::m_EBOCapacityBytes = 0;
	int                            GizmoRenderer2D::m_uMVP = -1;

	namespace {
		// ── TU-local GPU state ──────────────────────────────────────────────
		wgpu::ShaderModule    g_Module;
		wgpu::BindGroupLayout g_BindGroupLayout;
		wgpu::PipelineLayout  g_PipelineLayout;
		wgpu::Buffer          g_UniformBuffer;
		wgpu::BindGroup       g_BindGroup;
		wgpu::Buffer          g_VertexBuffer;
		uint32_t              g_VertexBufferCapacityBytes = 0;
		// Filled (triangle-list) geometry rides its own buffer: a single submission
		// flushes ALL WriteBuffer calls before ANY pass runs, so the line pass and
		// the filled pass must not share a vertex buffer or the second write aliases
		// the first. (See WebGPUApi.cpp FlushFrameCommands.)
		wgpu::Buffer          g_FilledVertexBuffer;
		uint32_t              g_FilledVertexBufferCapacityBytes = 0;
		// Pipeline cache keyed by (colorFormat << 2) | (hasDepth << 1) | triangleTopology,
		// same scheme SpriteResources / TextRenderer use (extended with a topology bit).
		std::unordered_map<uint32_t, wgpu::RenderPipeline> g_PipelineCache;

		uint32_t MakePipelineKey(wgpu::TextureFormat fmt, bool hasDepth, bool triangleTopology) {
			return (static_cast<uint32_t>(fmt) << 2)
				| (hasDepth ? 2u : 0u)
				| (triangleTopology ? 1u : 0u);
		}

		uint32_t PackRgba(const Color& c) {
			auto u8 = [](float v) {
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				return static_cast<uint32_t>(v * 255.0f + 0.5f);
			};
			// WebGPU's Unorm8x4 vertex format decodes bytes in little-endian
			// order as (r, g, b, a) → vec4.
			return u8(c.r)
				| (u8(c.g) << 8)
				| (u8(c.b) << 16)
				| (u8(c.a) << 24);
		}

		// No bounds check — caller pre-sized `out` to the upper-bound
		// vertex count.
		inline void EmitLine(PosColorVertex* out, std::size_t& cursor,
			float x0, float y0, float x1, float y1, uint32_t rgba)
		{
			out[cursor++] = PosColorVertex{ x0, y0, 0.0f, rgba };
			out[cursor++] = PosColorVertex{ x1, y1, 0.0f, rgba };
		}

		inline void EmitTri(PosColorVertex* out, std::size_t& cursor,
			float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba)
		{
			out[cursor++] = PosColorVertex{ x0, y0, 0.0f, rgba };
			out[cursor++] = PosColorVertex{ x1, y1, 0.0f, rgba };
			out[cursor++] = PosColorVertex{ x2, y2, 0.0f, rgba };
		}

		// Scratch for gizmo text commands; capacity persists across frames.
		std::vector<TextDrawCmd> g_GizmoTextCmds;

		// ── GPU resource helpers ────────────────────────────────────────────

		wgpu::BindGroupLayout BuildBindGroupLayout(wgpu::Device device) {
			wgpu::BindGroupLayoutEntry entry{};
			entry.binding    = 0;
			entry.visibility = wgpu::ShaderStage::Vertex;
			entry.buffer.type           = wgpu::BufferBindingType::Uniform;
			entry.buffer.hasDynamicOffset = false;
			entry.buffer.minBindingSize = 64;

			wgpu::BindGroupLayoutDescriptor desc{};
			desc.entryCount = 1;
			desc.entries    = &entry;
			desc.label      = "gizmo-bindgroup-layout";
			return device.CreateBindGroupLayout(&desc);
		}

		wgpu::PipelineLayout BuildPipelineLayout(wgpu::Device device, wgpu::BindGroupLayout bgl) {
			wgpu::BindGroupLayout bgls[1] = { bgl };
			wgpu::PipelineLayoutDescriptor desc{};
			desc.bindGroupLayoutCount = 1;
			desc.bindGroupLayouts     = bgls;
			desc.label                = "gizmo-pipeline-layout";
			return device.CreatePipelineLayout(&desc);
		}

		wgpu::RenderPipeline BuildGizmoPipeline(wgpu::Device device,
			wgpu::ShaderModule module, wgpu::PipelineLayout layout,
			wgpu::TextureFormat colorFormat, bool hasDepth, bool triangleTopology)
		{
			wgpu::VertexAttribute attrs[2] = {};
			attrs[0].format         = wgpu::VertexFormat::Float32x3;
			attrs[0].offset         = offsetof(PosColorVertex, x);
			attrs[0].shaderLocation = 0;
			attrs[1].format         = wgpu::VertexFormat::Unorm8x4;
			attrs[1].offset         = offsetof(PosColorVertex, color);
			attrs[1].shaderLocation = 1;

			wgpu::VertexBufferLayout vbl{};
			vbl.arrayStride    = sizeof(PosColorVertex);  // 16
			vbl.stepMode       = wgpu::VertexStepMode::Vertex;
			vbl.attributeCount = 2;
			vbl.attributes     = attrs;

			wgpu::BlendState blend{};
			blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
			blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.color.operation = wgpu::BlendOperation::Add;
			blend.alpha.srcFactor = wgpu::BlendFactor::One;
			blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
			blend.alpha.operation = wgpu::BlendOperation::Add;

			wgpu::ColorTargetState colorTarget{};
			colorTarget.format    = colorFormat;
			colorTarget.blend     = &blend;
			colorTarget.writeMask = wgpu::ColorWriteMask::All;

			wgpu::FragmentState fragState{};
			fragState.module      = module;
			fragState.entryPoint  = "fs_main";
			fragState.targetCount = 1;
			fragState.targets     = &colorTarget;

			wgpu::PrimitiveState prim{};
			prim.topology         = triangleTopology
				? wgpu::PrimitiveTopology::TriangleList
				: wgpu::PrimitiveTopology::LineList;
			// List topologies don't need a strip-index-format (only used by
			// *Strip topologies); explicit Undefined matches the spec.
			prim.stripIndexFormat = wgpu::IndexFormat::Undefined;
			prim.frontFace        = wgpu::FrontFace::CCW;
			// Filled gizmos are authored CCW above but a caller may pass a
			// mirrored transform; never cull so a flipped winding still fills.
			prim.cullMode         = wgpu::CullMode::None;

			wgpu::DepthStencilState depthState{};
			if (hasDepth) {
				depthState.format            = wgpu::TextureFormat::Depth24PlusStencil8;
				depthState.depthWriteEnabled = false;  // gizmos don't write depth
				depthState.depthCompare      = wgpu::CompareFunction::Always;
			}

			wgpu::RenderPipelineDescriptor desc{};
			desc.label  = "gizmo-pipeline";
			desc.layout = layout;

			desc.vertex.module      = module;
			desc.vertex.entryPoint  = "vs_main";
			desc.vertex.bufferCount = 1;
			desc.vertex.buffers     = &vbl;

			desc.fragment     = &fragState;
			desc.primitive    = prim;
			desc.depthStencil = hasDepth ? &depthState : nullptr;

			desc.multisample.count = 1;
			desc.multisample.mask  = 0xFFFFFFFF;

			return device.CreateRenderPipeline(&desc);
		}

		wgpu::RenderPipeline GetOrBuildPipeline(wgpu::TextureFormat colorFormat, bool hasDepth, bool triangleTopology) {
			if (!g_Module || !g_PipelineLayout) return nullptr;
			const uint32_t key = MakePipelineKey(colorFormat, hasDepth, triangleTopology);
			auto it = g_PipelineCache.find(key);
			if (it != g_PipelineCache.end()) return it->second;
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) return nullptr;
			wgpu::RenderPipeline pipeline = BuildGizmoPipeline(
				device, g_Module, g_PipelineLayout, colorFormat, hasDepth, triangleTopology);
			if (!pipeline) return nullptr;
			g_PipelineCache.emplace(key, pipeline);
			return pipeline;
		}

		bool EnsureUniformBuffer(wgpu::Device device) {
			if (g_UniformBuffer) return true;
			wgpu::BufferDescriptor desc{};
			desc.size  = 64;  // mat4x4<f32>
			desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
			desc.label = "gizmo-viewproj-ubo";
			g_UniformBuffer = device.CreateBuffer(&desc);
			return static_cast<bool>(g_UniformBuffer);
		}

		bool EnsureBindGroup(wgpu::Device device) {
			if (g_BindGroup) return true;
			if (!g_BindGroupLayout || !g_UniformBuffer) return false;

			wgpu::BindGroupEntry entry{};
			entry.binding = 0;
			entry.buffer  = g_UniformBuffer;
			entry.offset  = 0;
			entry.size    = 64;

			wgpu::BindGroupDescriptor desc{};
			desc.layout     = g_BindGroupLayout;
			desc.entryCount = 1;
			desc.entries    = &entry;
			desc.label      = "gizmo-bindgroup";
			g_BindGroup = device.CreateBindGroup(&desc);
			return static_cast<bool>(g_BindGroup);
		}

		bool EnsureVertexBuffer(wgpu::Device device, wgpu::Buffer& buffer,
			uint32_t& capacityBytes, uint32_t neededBytes, const char* label)
		{
			if (capacityBytes >= neededBytes && buffer) return true;
			uint32_t cap = capacityBytes > 0
				? capacityBytes
				: 4096u;  // 256 lines × 16 bytes = 4 KB initial
			while (cap < neededBytes) cap *= 2;

			wgpu::BufferDescriptor desc{};
			desc.size  = cap;
			desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
			desc.label = label;
			wgpu::Buffer buf = device.CreateBuffer(&desc);
			if (!buf) return false;
			buffer        = std::move(buf);
			capacityBytes = cap;
			return true;
		}
	}  // anonymous namespace

	bool GizmoRenderer2D::Initialize() {
		if (m_IsInitialized) return true;
		if (!WebGPUBackend::IsInitialized()) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Initialize called before WebGPU backend initialized");
			return false;
		}

		// Shader_WebGPU's built-in registry resolves "gizmo" to the embedded
		// WGSL. Same vs/fs path pattern Renderer2D + TextRenderer use.
		m_GizmoShader = std::make_unique<Shader>(
			std::string("IndexAssets/Shaders/GizmoShader.vs"),
			std::string("IndexAssets/Shaders/GizmoShader.fs"));
		if (!m_GizmoShader || !m_GizmoShader->IsValid()) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Gizmo shader failed to load — gizmos disabled");
			m_GizmoShader.reset();
			return false;
		}
		const auto lookup = WebGPUBackend::LookupShader(m_GizmoShader->GetHandle());
		if (!lookup.Valid) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Gizmo shader has no module in WebGPUBackend pool");
			m_GizmoShader.reset();
			return false;
		}
		g_Module = lookup.Module;

		wgpu::Device device = WebGPUBackend::GetDevice();
		g_BindGroupLayout = BuildBindGroupLayout(device);
		g_PipelineLayout  = BuildPipelineLayout(device, g_BindGroupLayout);
		if (!g_BindGroupLayout || !g_PipelineLayout) {
			IDX_CORE_ERROR_TAG("GizmoRenderer",
				"Failed to build bindgroup / pipeline layout");
			g_Module          = nullptr;
			g_BindGroupLayout = nullptr;
			g_PipelineLayout  = nullptr;
			m_GizmoShader.reset();
			return false;
		}

		// Private TextRenderer for Gizmo::DrawText. A failed init just means no
		// gizmo text — line/filled gizmos still work, so don't fail the whole renderer.
		m_TextRenderer = std::make_unique<TextRenderer>();
		m_TextRenderer->Initialize();
		if (!m_TextRenderer->IsInitialized()) {
			IDX_CORE_WARN_TAG("GizmoRenderer",
				"Gizmo text renderer failed to initialize — Gizmo::DrawText disabled");
			m_TextRenderer.reset();
		}

		m_IsInitialized = true;
		return true;
	}

	void GizmoRenderer2D::Shutdown() {
		if (!m_IsInitialized) return;
		if (m_TextRenderer) {
			m_TextRenderer->Shutdown();
			m_TextRenderer.reset();
		}
		g_PipelineCache.clear();
		g_VertexBuffer                    = nullptr;
		g_VertexBufferCapacityBytes       = 0;
		g_FilledVertexBuffer              = nullptr;
		g_FilledVertexBufferCapacityBytes = 0;
		g_BindGroup                 = nullptr;
		g_UniformBuffer             = nullptr;
		g_PipelineLayout            = nullptr;
		g_BindGroupLayout           = nullptr;
		g_Module                    = nullptr;
		m_GizmoShader.reset();

		m_GizmoIndices.clear();
		m_GizmoIndices.shrink_to_fit();
		s_UploadBuffer.clear();
		s_UploadBuffer.shrink_to_fit();
		m_IsInitialized = false;
	}

	void GizmoRenderer2D::OnResize(int /*w*/, int /*h*/) {}

	void GizmoRenderer2D::BeginFrame(uint16_t viewId) {
		// viewId is unused under WebGPU — the active framebuffer is set
		// by RenderApi::BindFramebuffer before this call. Stored for ABI
		// but not consulted at submit time.
		m_GizmoViewId = viewId;
	}

	void GizmoRenderer2D::EndFrame() {
		// Single per-frame clear point. RenderWithVP only READS the gizmo
		// buffer now — it must NOT clear, because a single frame can flush
		// the same buffer into multiple targets (editor Editor View FBO,
		// Game View FBO, and the main-window pass). If each flush cleared,
		// only the first target would ever show gizmos and every later one
		// (e.g. the Game View, or any script `Gizmo.DrawLine` in play mode)
		// would draw from an empty buffer. EndFrame is called exactly once
		// after all RenderWithVP calls in both render paths
		// (RenderPipelineOnly + RenderOnceForRefresh), so this is the correct
		// place to reset for the next frame.
		Gizmo::Clear();
	}

	void GizmoRenderer2D::RenderWithVP(const glm::mat4& vp, GizmoLayerMask layerMask) {
		if (!m_IsInitialized) return;
		// Filled first so wireframe / lines paint on top of fills, then text on top of all.
		FlushGizmosImpl(vp, BuildFilledGeometry(layerMask), /*filled=*/true);
		FlushGizmosImpl(vp, BuildGeometry(layerMask),       /*filled=*/false);
		RenderGizmoText(vp, layerMask);
		// NOTE: do NOT clear here — see GizmoRenderer2D::EndFrame above.
	}

	std::span<const PosColorVertex>
	GizmoRenderer2D::BuildGeometry(GizmoLayerMask layerMask) {
		std::size_t cap = Gizmo::s_Squares.size() * 8   // 4 edges × 2 verts
		                + Gizmo::s_Lines.size()   * 2;
		for (const Circle& ci : Gizmo::s_Circles) {
			if (ci.Segments >= 3) {
				cap += static_cast<std::size_t>(ci.Segments) * 2;
			}
		}
		if (cap == 0) return {};

		std::span<PosColorVertex> buf =
			FrameArenas::Frame().CreateArray<PosColorVertex>(cap);
		if (buf.empty()) {
			static bool s_OverflowLogged = false;
			if (!s_OverflowLogged) {
				IDX_CORE_WARN_TAG("GizmoRenderer",
					"Frame arena exhausted: {} verts requested ({} bytes). "
					"Increase ApplicationConfig::FrameArenaCapacityBytes.",
					cap, cap * sizeof(PosColorVertex));
				s_OverflowLogged = true;
			}
			return {};
		}

		PosColorVertex* out = buf.data();
		std::size_t cursor = 0;

		for (const Square& sq : Gizmo::s_Squares) {
			if (!HasAnyLayer(sq.Layer, layerMask)) continue;
			const uint32_t rgba = PackRgba(sq.Color);
			const float cx = sq.Center.x;
			const float cy = sq.Center.y;
			const float hx = sq.HalfExtents.x;
			const float hy = sq.HalfExtents.y;
			const float c  = std::cos(sq.Radiant);
			const float s  = std::sin(sq.Radiant);
			auto corner = [&](float lx, float ly) {
				return std::pair<float, float>{
					cx + lx * c - ly * s,
					cy + lx * s + ly * c
				};
			};
			const auto [x0, y0] = corner(-hx, -hy);
			const auto [x1, y1] = corner(+hx, -hy);
			const auto [x2, y2] = corner(+hx, +hy);
			const auto [x3, y3] = corner(-hx, +hy);
			EmitLine(out, cursor, x0, y0, x1, y1, rgba);
			EmitLine(out, cursor, x1, y1, x2, y2, rgba);
			EmitLine(out, cursor, x2, y2, x3, y3, rgba);
			EmitLine(out, cursor, x3, y3, x0, y0, rgba);
		}

		for (const Line& ln : Gizmo::s_Lines) {
			if (!HasAnyLayer(ln.Layer, layerMask)) continue;
			EmitLine(out, cursor, ln.Start.x, ln.Start.y, ln.End.x, ln.End.y, PackRgba(ln.Color));
		}

		for (const Circle& ci : Gizmo::s_Circles) {
			if (!HasAnyLayer(ci.Layer, layerMask)) continue;
			if (ci.Segments < 3) continue;
			const uint32_t rgba = PackRgba(ci.Color);
			const float step = 6.28318530717958647692f / static_cast<float>(ci.Segments);
			float prevX = ci.Center.x + ci.Radius;
			float prevY = ci.Center.y;
			for (int i = 1; i <= ci.Segments; ++i) {
				const float a = static_cast<float>(i) * step;
				const float nx = ci.Center.x + std::cos(a) * ci.Radius;
				const float ny = ci.Center.y + std::sin(a) * ci.Radius;
				EmitLine(out, cursor, prevX, prevY, nx, ny, rgba);
				prevX = nx;
				prevY = ny;
			}
		}

		return buf.first(cursor);
	}

	std::span<const PosColorVertex>
	GizmoRenderer2D::BuildFilledGeometry(GizmoLayerMask layerMask) {
		std::size_t cap = Gizmo::s_FilledSquares.size() * 6   // 2 tris × 3 verts
		                + Gizmo::s_ThickLines.size()   * 6;  // 2 tris per thick segment
		for (const Circle& ci : Gizmo::s_FilledCircles) {
			if (ci.Segments >= 3) {
				cap += static_cast<std::size_t>(ci.Segments) * 3;  // fan: Segments tris × 3 verts
			}
		}
		if (cap == 0) return {};

		std::span<PosColorVertex> buf =
			FrameArenas::Frame().CreateArray<PosColorVertex>(cap);
		if (buf.empty()) {
			static bool s_OverflowLogged = false;
			if (!s_OverflowLogged) {
				IDX_CORE_WARN_TAG("GizmoRenderer",
					"Frame arena exhausted (filled): {} verts requested ({} bytes). "
					"Increase ApplicationConfig::FrameArenaCapacityBytes.",
					cap, cap * sizeof(PosColorVertex));
				s_OverflowLogged = true;
			}
			return {};
		}

		PosColorVertex* out = buf.data();
		std::size_t cursor = 0;

		for (const Square& sq : Gizmo::s_FilledSquares) {
			if (!HasAnyLayer(sq.Layer, layerMask)) continue;
			const uint32_t rgba = PackRgba(sq.Color);
			const float cx = sq.Center.x;
			const float cy = sq.Center.y;
			const float hx = sq.HalfExtents.x;
			const float hy = sq.HalfExtents.y;
			const float c  = std::cos(sq.Radiant);
			const float s  = std::sin(sq.Radiant);
			auto corner = [&](float lx, float ly) {
				return std::pair<float, float>{
					cx + lx * c - ly * s,
					cy + lx * s + ly * c
				};
			};
			const auto [x0, y0] = corner(-hx, -hy);
			const auto [x1, y1] = corner(+hx, -hy);
			const auto [x2, y2] = corner(+hx, +hy);
			const auto [x3, y3] = corner(-hx, +hy);
			EmitTri(out, cursor, x0, y0, x1, y1, x2, y2, rgba);
			EmitTri(out, cursor, x0, y0, x2, y2, x3, y3, rgba);
		}

		for (const Circle& ci : Gizmo::s_FilledCircles) {
			if (!HasAnyLayer(ci.Layer, layerMask)) continue;
			if (ci.Segments < 3) continue;
			const uint32_t rgba = PackRgba(ci.Color);
			const float step = 6.28318530717958647692f / static_cast<float>(ci.Segments);
			float prevX = ci.Center.x + ci.Radius;
			float prevY = ci.Center.y;
			for (int i = 1; i <= ci.Segments; ++i) {
				const float a = static_cast<float>(i) * step;
				const float nx = ci.Center.x + std::cos(a) * ci.Radius;
				const float ny = ci.Center.y + std::sin(a) * ci.Radius;
				EmitTri(out, cursor, ci.Center.x, ci.Center.y, prevX, prevY, nx, ny, rgba);
				prevX = nx;
				prevY = ny;
			}
		}

		// Thick lines last so the selection outline paints over filled gizmos.
		// Each segment expands to a quad whose ends are extended by HalfWidth
		// (square caps), so segments sharing an endpoint overlap into a
		// continuous outline.
		for (const ThickLine& tl : Gizmo::s_ThickLines) {
			if (!HasAnyLayer(tl.Layer, layerMask)) continue;
			const uint32_t rgba = PackRgba(tl.Color);
			float dx = tl.End.x - tl.Start.x;
			float dy = tl.End.y - tl.Start.y;
			float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-12f) { dx = 1.0f; dy = 0.0f; len = 1.0f; } // degenerate → tiny square
			const float inv = 1.0f / len;
			const float ux = dx * inv * tl.HalfWidth;   // dir * halfWidth (end extension)
			const float uy = dy * inv * tl.HalfWidth;
			const float nx = -uy;                         // perpendicular * halfWidth
			const float ny = ux;
			const float ax = tl.Start.x - ux, ay = tl.Start.y - uy;
			const float bx = tl.End.x + ux,   by = tl.End.y + uy;
			EmitTri(out, cursor, ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, rgba);
			EmitTri(out, cursor, ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, rgba);
		}

		return buf.first(cursor);
	}

	void GizmoRenderer2D::FlushGizmosImpl(const glm::mat4& vp,
		std::span<const PosColorVertex> verts, bool filled)
	{
		if (verts.empty()) return;

		auto target = WebGPUBackend::BeginRenderToCurrentTarget();
		if (!target.Valid) return;

		wgpu::Device device = WebGPUBackend::GetDevice();
		wgpu::Queue  queue  = WebGPUBackend::GetQueue();
		if (!device || !queue) return;

		wgpu::RenderPipeline pipeline = GetOrBuildPipeline(target.ColorFormat, target.HasDepth, filled);
		if (!pipeline) {
			IDX_CORE_WARN_TAG("GizmoRenderer",
				"No pipeline for color-format {} hasDepth={} filled={} — gizmos not submitted",
				static_cast<int>(target.ColorFormat), target.HasDepth, filled);
			return;
		}

		if (!EnsureUniformBuffer(device)) return;
		if (!EnsureBindGroup(device))    return;
		queue.WriteBuffer(g_UniformBuffer, 0, glm::value_ptr(vp), 64);

		// Distinct buffers per topology: a single submission flushes all WriteBuffer
		// calls before any pass runs, so line and filled passes must not share one.
		wgpu::Buffer& vertexBuffer  = filled ? g_FilledVertexBuffer : g_VertexBuffer;
		uint32_t&     capacityBytes = filled ? g_FilledVertexBufferCapacityBytes : g_VertexBufferCapacityBytes;
		const char*   label         = filled ? "gizmo-filled-vertex-buffer" : "gizmo-vertex-buffer";

		const uint32_t numVerts = static_cast<uint32_t>(verts.size());
		const uint32_t totalBytes = numVerts * static_cast<uint32_t>(sizeof(PosColorVertex));
		if (!EnsureVertexBuffer(device, vertexBuffer, capacityBytes, totalBytes, label)) return;
		queue.WriteBuffer(vertexBuffer, 0, verts.data(), totalBytes);

		wgpu::CommandEncoder encoder = WebGPUBackend::GetFrameEncoder();
		if (!encoder) return;

		wgpu::RenderPassColorAttachment colorAtt{};
		colorAtt.view       = target.ColorView;
		colorAtt.loadOp     = wgpu::LoadOp::Load;
		colorAtt.storeOp    = wgpu::StoreOp::Store;
		colorAtt.depthSlice = wgpu::kDepthSliceUndefined;

		wgpu::RenderPassDepthStencilAttachment depthAtt{};
		if (target.HasDepth) {
			depthAtt.view              = target.DepthView;
			depthAtt.depthLoadOp       = wgpu::LoadOp::Load;
			depthAtt.depthStoreOp      = wgpu::StoreOp::Store;
			depthAtt.stencilLoadOp     = wgpu::LoadOp::Load;
			depthAtt.stencilStoreOp    = wgpu::StoreOp::Store;
		}

		wgpu::RenderPassDescriptor passDesc{};
		passDesc.label                  = filled ? "gizmo-filled-pass" : "gizmo-pass";
		passDesc.colorAttachmentCount   = 1;
		passDesc.colorAttachments       = &colorAtt;
		passDesc.depthStencilAttachment = target.HasDepth ? &depthAtt : nullptr;

		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
		WebGPUBackend::ApplyCachedViewportToPass(pass);
		pass.SetPipeline(pipeline);
		pass.SetBindGroup(0, g_BindGroup);
		pass.SetVertexBuffer(0, vertexBuffer);
		pass.Draw(numVerts, /*instanceCount=*/1, /*firstVertex=*/0, /*firstInstance=*/0);
		pass.End();

		if (target.IsSwapChain) {
			WebGPUBackend::MarkSwapChainRendered();
		}
	}

	void GizmoRenderer2D::RenderGizmoText(const glm::mat4& vp, GizmoLayerMask layerMask) {
		if (!m_TextRenderer || !m_TextRenderer->IsInitialized() || Gizmo::s_Texts.empty())
			return;

		Font* font = FontManager::GetFont(FontManager::GetDefaultFont());
		if (!font || !font->IsLoaded()) return;

		const float bakedSize = font->GetPixelSize() > 0.0f ? font->GetPixelSize() : 32.0f;

		g_GizmoTextCmds.clear();
		g_GizmoTextCmds.reserve(Gizmo::s_Texts.size());
		for (const GizmoText& t : Gizmo::s_Texts) {
			if (!HasAnyLayer(t.Layer, layerMask)) continue;
			if (t.Text.empty()) continue;

			// Mirror entity-text world sizing: FontSize(px) → world units.
			const float drawScale = (t.Size / bakedSize) / k_TextPixelsPerWorldUnit;

			TextDrawCmd cmd;
			cmd.FontPtr  = font;
			cmd.Text     = std::string_view(t.Text);
			cmd.X        = t.Position.x;
			cmd.Y        = t.Position.y;
			cmd.Scale    = drawScale;
			cmd.Tint     = t.Color;
			cmd.Align    = TextAlignment::Center;
			cmd.Rotation = t.Radiant;     // already radians (Gizmo::DrawText converted from degrees)
			cmd.Pivot    = t.Position;    // rotate about the anchor
			g_GizmoTextCmds.push_back(cmd);
		}

		if (!g_GizmoTextCmds.empty())
			m_TextRenderer->RenderInstances(g_GizmoTextCmds, vp);
	}

}  // namespace Index
