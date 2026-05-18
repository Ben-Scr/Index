#include "pch.hpp"
#include "Graphics/Shader.hpp"

#include "Core/Log.hpp"
#include "Graphics/Backend/WebGPUBackend.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// =============================================================================
// Shader — WebGPU (Dawn) implementation.
// -----------------------------------------------------------------------------
// `unsigned m_Program` is an opaque ID with 0 = invalid.
//
// Two ways a shader name comes in here:
//   1. Built-in (sprite, gizmo, text, ...). Resolved via the static table
//      below — WGSL source is embedded in this TU so a release build doesn't
//      need .wgsl files on disk to bring up the engine's own pipelines.
//   2. User / project (.wgsl on disk). Path is the vsPath argument's stem —
//      e.g. `IndexAssets/Shaders/SpriteShader.vs` -> lookup
//      `IndexAssets/Shaders/webgpu/sprite.wgsl` first under the executable
//      dir, then under the source tree.
//
// Why one module per Shader (not vs+fs separately):
// WGSL is multi-entry-point. The sprite module here has `vs_main` plus two
// fragment entry points — `fs_main` (textured, instance-coloured) and
// `fs_wire_main` (constant opaque black for the wireframe-debug pipeline);
// we don't pre-split into separate modules because Dawn's pipeline creation
// takes (module, entry_point) per stage and can re-use the same module
// across stages. The renderer side knows the entry-point names; this class
// only owns the module.
//
// The renderer ports (Renderer2D, GuiRenderer, TextRenderer, GizmoRenderer)
// look up the wgpu::ShaderModule via WebGPUBackend::LookupShader(m_Program)
// and feed it into a wgpu::RenderPipeline along with the bind-group layout
// + target format.
// =============================================================================

namespace Index {

	namespace {
		// ── Built-in WGSL registry ──────────────────────────────────────────
		// Sprite shader — used by Renderer2D (world-space sprites) and
		// GuiRenderer (screen-space UI quads):
		//   * Vertex inputs: unit-quad position (loc 0) + 3 instance vec4s
		//     (loc 1..3) carrying (Pos.xy, Scale.xy), (Color RGBA), and
		//     (cos, sin, _, _) — packed identically to
		//     WebGPUSpriteResources's SpriteInstance layout.
		//   * Bind group 0: { 0: u_ViewProj (mat4 uniform), 1: albedo
		//     texture, 2: albedo sampler }. The matrix uses column-major
		//     storage — matches how the engine's math layer pushes it.
		constexpr const char* k_SpriteWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var t_albedo: texture_2d<f32>;
@group(0) @binding(2) var s_albedo: sampler;

struct VertexInput {
	@location(0) position: vec3<f32>,
	@location(1) i_data0: vec4<f32>,  // Pos.xy, Scale.xy
	@location(2) i_data1: vec4<f32>,  // Color RGBA
	@location(3) i_data2: vec4<f32>,  // rotation (radians), _, _, _
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
	@location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	// Per-instance scale, then rotate by the rotation angle, then translate.
	// sin/cos previously ran once per instance on the CPU during
	// EncodeInstance44; doing it here parallelises it across the GPU and
	// keeps the CPU encode path as branch-free struct stores.
	let scaled = in.position.xy * in.i_data0.zw;
	let r = in.i_data2.x;
	let c = cos(r);
	let s = sin(r);
	let rotated = vec2<f32>(
		scaled.x * c - scaled.y * s,
		scaled.x * s + scaled.y * c
	);
	let world = rotated + in.i_data0.xy;

	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(world, 0.0, 1.0);
	out.color = in.i_data1;
	// UV maps [-0.5, 0.5] -> [0, 1] on X, and flips Y so texture top-left
	// shows at the quad's visual top (engine has Y-up world space; textures
	// are top-left origin per stb's load).
	out.uv = vec2<f32>(in.position.x + 0.5, 0.5 - in.position.y);
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let texel = textureSample(t_albedo, s_albedo, in.uv);
	return texel * in.color;
}

// Wireframe-debug fragment entry point. Used by the Wireframe sprite
// pipeline (LineList topology, blending disabled) — outputs solid opaque
// black regardless of texture or instance colour so quad-edge lines render
// as the Editor View's debug overlay. Keeping the black-write in the
// shader (rather than mutating the shared instance buffer on the CPU)
// avoids aliasing the filled-pass's instance data in Mixed draw mode.
@fragment
fn fs_wire_main(in: VertexOutput) -> @location(0) vec4<f32> {
	return vec4<f32>(0.0, 0.0, 0.0, 1.0);
}
)WGSL";

		// Text shader — used by TextRenderer (Stage 6). The atlas is an R8
		// alpha texture baked by stbtt_pack; the fragment shader pulls the
		// red channel as coverage and modulates it into the per-vertex
		// color's alpha. Vertices are per-glyph (6 verts per glyph, not
		// instanced) because each glyph has a unique (UV, position) pair.
		constexpr const char* k_TextWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var t_atlas: texture_2d<f32>;
@group(0) @binding(2) var s_atlas: sampler;

struct VertexInput {
	@location(0) position: vec2<f32>,
	@location(1) uv:       vec2<f32>,
	@location(2) color:    vec4<f32>,
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
	@location(1) uv:    vec2<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(in.position, 0.0, 1.0);
	out.color = in.color;
	out.uv = in.uv;
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let alpha = textureSample(t_atlas, s_atlas, in.uv).r;
	return vec4<f32>(in.color.rgb, in.color.a * alpha);
}
)WGSL";

		// Gizmo shader — used by GizmoRenderer2D for debug line drawing
		// (squares, circles, raw lines). Vertex layout: 1 buffer with
		// position vec3<f32> + color packed as Unorm8x4 (the engine's
		// PosColorVertex stores 4 bytes RGBA as a uint32 — Dawn decodes
		// it through Unorm8x4 at the vertex-fetch stage). No texture
		// sampling, no per-vertex UV — just pass color through to the
		// fragment shader. Pipeline uses LineList primitive topology.
		constexpr const char* k_GizmoWGSL = R"WGSL(
struct Uniforms {
	viewProj: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> u: Uniforms;

struct VertexInput {
	@location(0) position: vec3<f32>,
	@location(1) color:    vec4<f32>,
};

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
	var out: VertexOutput;
	out.clip_position = u.viewProj * vec4<f32>(in.position, 1.0);
	out.color = in.color;
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	return in.color;
}
)WGSL";

		// Post-process blit shader — used by PostProcessor for the
		// fullscreen passthrough that turns the HDR intermediate scene FBO
		// back into the caller's target. No vertex buffer needed: we draw
		// 3 verts via the "oversized fullscreen triangle" trick (covers
		// the viewport with a single triangle, faster than a clipped quad).
		//   * Bind group 0: { 0: src texture, 1: linear sampler }.
		//   * Output: textureSample(src, sampler, uv) — straight passthrough.
		// Future effect shaders (vignette, bloom, ...) follow the same
		// vertex layout and bind-group-0 shape so they can share the
		// PostProcessor's bind-group-layout, sampler, and vertex stage.
		constexpr const char* k_PostBlitWGSL = R"WGSL(
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	// Fullscreen triangle covering the viewport. Vertices in NDC:
	//   idx 0 -> (-1, -1)    uv (0, 1)
	//   idx 1 -> ( 3, -1)    uv (2, 1)
	//   idx 2 -> (-1,  3)    uv (0, -1)
	// The triangle extends past the clip rect; the rasterizer discards
	// the off-screen pixels. One triangle instead of a quad's two means
	// no diagonal seam where the two halves meet.
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0),
		vec2<f32>( 3.0, -1.0),
		vec2<f32>(-1.0,  3.0),
	);
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0),
		vec2<f32>(2.0, 1.0),
		vec2<f32>(0.0, -1.0),
	);
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	return textureSample(t_src, s_src, in.uv);
}
)WGSL";

		// Vignette post-effect — reads the intermediate scene, modulates the
		// rgb toward `u.color.rgb` near the corners. The fullscreen-triangle
		// vertex shader matches k_PostBlitWGSL so the same pipeline-layout
		// pattern works (only the BindGroupLayout differs to add the
		// uniform-buffer binding).
		//
		// Uniform layout (48 bytes, three vec4s — packed for std140-style
		// 16-byte alignment so the C++ side can WriteBuffer a fixed-shape
		// struct):
		//   color:          rgb tint applied at full vignette strength; a = unused
		//   centerSmooth:   xy = vignette centre (0..1 UV), z = intensity (0..1),
		//                   w = smoothness (0..1, falloff width)
		//   aspectRound:    x = aspect ratio (dstWidth / dstHeight),
		//                   y = roundness (1 = pure UV-space circle, 0 = aspect-
		//                       corrected screen-space circle),
		//                   zw = pad
		constexpr const char* k_PostVignetteWGSL = R"WGSL(
struct VignetteParams {
	color:        vec4<f32>,
	centerSmooth: vec4<f32>,
	aspectRound:  vec4<f32>,
};

@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: VignetteParams;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0),
		vec2<f32>( 3.0, -1.0),
		vec2<f32>(-1.0,  3.0),
	);
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0),
		vec2<f32>(2.0, 1.0),
		vec2<f32>(0.0, -1.0),
	);
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let scene = textureSample(t_src, s_src, in.uv);

	// Distance from configurable centre, with horizontal stretch driven by
	// (1 - roundness). At roundness=1 the mask is a pure UV circle (which
	// looks elliptical on a 16:9 viewport). At roundness=0 the mask is
	// aspect-corrected so it appears as a true screen-space circle.
	let aspect = u.aspectRound.x;
	let roundness = u.aspectRound.y;
	let stretch = mix(aspect, 1.0, roundness);  // roundness=1 -> 1.0; roundness=0 -> aspect
	let d = (in.uv - u.centerSmooth.xy);
	let d_corrected = vec2<f32>(d.x * stretch, d.y);

	// 1.4142 normalises so r ≈ 1.0 at the corner of a centred unit-square
	// vignette (length of (0.5, 0.5) * sqrt(2) = 1.0). Off-centre or
	// stretched cases shift the "corner = 1" point but the intensity /
	// smoothness sliders still scrub through the same useful range.
	let r = length(d_corrected) * 1.4142;
	let intensity = u.centerSmooth.z;
	let smoothness = max(u.centerSmooth.w, 0.0001); // avoid step()-like binary mask
	let inner = 1.0 - intensity;
	let outer = inner + smoothness;
	let mask = smoothstep(inner, outer, r);

	return vec4<f32>(mix(scene.rgb, u.color.rgb, mask), scene.a);
}
)WGSL";

		struct BuiltIn {
			std::string_view Name;
			const char*      WGSL;
		};
		// Each entry maps a "name" (the stem of vsPath after stripping
		// the "Shader" suffix and lowercasing) to its embedded WGSL
		// source. Future renderer ports register here as they land.
		// Shared fullscreen-triangle vertex stage embedded in every post
		// effect's WGSL. The vertex stage never changes per effect — only
		// the fragment math + bind-group-2 uniform shape vary. Inlining
		// the same vs_main in each effect string keeps each shader module
		// self-contained (Dawn needs the module to expose both vs_main
		// and fs_main when the same module backs both stages, which
		// avoids cross-module pipeline links).

		// Chromatic Aberration — sample R/G/B at three UVs along the
		// radial direction (uv - 0.5). Uniform layout (16 bytes, single
		// vec4):
		//   params.x = Intensity (0..1, controls UV offset magnitude)
		constexpr const char* k_PostChromaticWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let dir = (in.uv - vec2<f32>(0.5, 0.5));
	// Scale offset by distance so corners get more shift than centre —
	// matches the lens-defect aesthetic of real CA.
	let offset = dir * u.p.x * 0.05;
	let r = textureSample(t_src, s_src, in.uv + offset).r;
	let g = textureSample(t_src, s_src, in.uv).g;
	let b = textureSample(t_src, s_src, in.uv - offset).b;
	let a = textureSample(t_src, s_src, in.uv).a;
	return vec4<f32>(r, g, b, a);
}
)WGSL";

		// Grain — additive noise overlay. Uniform layout (16 bytes):
		//   p.x = Intensity (0..1)
		//   p.y = Size (0.3..3, smaller = finer grain)
		//   p.z = Colored (0 = mono, 1 = per-channel coloured noise)
		//   p.w = uTime (seconds, animates the noise pattern)
		constexpr const char* k_PostGrainWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

// IQ-style cheap hash. Stable per UV; the time seed perturbs each frame.
fn hash21(p: vec2<f32>) -> f32 {
	let q = fract(p * vec2<f32>(123.34, 456.21));
	let r = q + dot(q, q + 78.233);
	return fract(r.x * r.y);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let scene = textureSample(t_src, s_src, in.uv);
	let scale = max(u.p.y, 0.001);
	// Multiply uv into the noise function's input. Time perturbation
	// shifts the input so the grain animates instead of being static.
	let n_seed = in.uv * (1.0 / scale) * 1000.0 + vec2<f32>(u.p.w * 17.0, u.p.w * 23.0);
	let mono = hash21(n_seed) - 0.5;
	var noise = vec3<f32>(mono, mono, mono);
	if (u.p.z > 0.5) {
		let g = hash21(n_seed + vec2<f32>(11.7, 4.3)) - 0.5;
		let b = hash21(n_seed + vec2<f32>(31.2, 17.9)) - 0.5;
		noise = vec3<f32>(mono, g, b);
	}
	return vec4<f32>(scene.rgb + noise * u.p.x, scene.a);
}
)WGSL";

		// Lens Distortion — radial UV remap. Positive intensity = barrel
		// distortion (image bows outward at edges); negative = pincushion.
		// Uniform layout (16 bytes):
		//   p.x = Intensity (-1..1)
		//   p.y = Scale (0.5..1.5, post-warp zoom; <1 reveals warp edges)
		//   p.z = Center.x  (UV space, 0..1)
		//   p.w = Center.y
		constexpr const char* k_PostLensDistortionWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let center = u.p.zw;
	let d = in.uv - center;
	let r2 = dot(d, d);
	// Polynomial distortion: warped = d * (1 + k * r^2), then divided
	// by Scale so the user can pull the warped frame back into view.
	// Intensity > 0 -> barrel (image bows outward); < 0 -> pincushion.
	let warped = d * (1.0 + u.p.x * r2 * 2.0);
	let scale = max(u.p.y, 0.001);
	let src_uv = center + warped / scale;
	// Soft falloff at the edge instead of a hard discard. The linear
	// sampler uses ClampToEdge so out-of-bounds samples return the
	// edge color; the smoothstep on signed-distance-to-uv-bounds fades
	// those clamped pixels to black so they read as "outside the lens"
	// instead of stretching the edge pixel across the corner.
	let edge_d = min(min(src_uv.x, 1.0 - src_uv.x),
	                 min(src_uv.y, 1.0 - src_uv.y));
	let edge_mask = smoothstep(-0.02, 0.0, edge_d);
	let sampled = textureSample(t_src, s_src, clamp(src_uv, vec2<f32>(0.0), vec2<f32>(1.0)));
	return vec4<f32>(sampled.rgb * edge_mask, sampled.a);
}
)WGSL";

		// Color Grading — exposure → contrast → saturation →
		// temperature/tint → color filter. Uniform layout (48 bytes, three
		// vec4s):
		//   colorFilter   = vec4(rgb tint, 1)
		//   exposureCS    = vec4(exposure stops, contrast, saturation, _)
		//   tempTint      = vec4(temperature -1..1, tint -1..1, _, _)
		// Order matches Unity's URP color-grading post.
		constexpr const char* k_PostColorGradingWGSL = R"WGSL(
struct Params {
	colorFilter: vec4<f32>,
	exposureCS:  vec4<f32>,
	tempTint:    vec4<f32>,
};
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let scene = textureSample(t_src, s_src, in.uv);
	var c = scene.rgb;

	// Exposure (stops) — multiply in linear.
	c = c * exp2(u.exposureCS.x);

	// Color filter — tint multiply.
	c = c * u.colorFilter.rgb;

	// Temperature + Tint — simple two-axis colour shift. Temperature
	// shifts the blue/red balance; Tint shifts green/magenta. Not a
	// proper Kelvin curve but the cheap approximation tracks Unity's
	// "Temperature/Tint" sliders within visual tolerance.
	let temp = u.tempTint.x;
	let tint = u.tempTint.y;
	c.r = c.r + temp * 0.1;
	c.b = c.b - temp * 0.1;
	c.g = c.g + tint * 0.1;
	c = max(c, vec3<f32>(0.0));

	// Contrast around 0.5 (mid grey).
	c = (c - 0.5) * u.exposureCS.y + 0.5;
	c = max(c, vec3<f32>(0.0));

	// Saturation — mix toward Rec.709 luma.
	let luma = dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
	c = mix(vec3<f32>(luma), c, u.exposureCS.z);

	return vec4<f32>(c, scene.a);
}
)WGSL";

		// Bloom is a 4-pass pipeline (replaces the original single-pass
		// wide-tap shader, which produced visible sample-grid artifacts on
		// wide halos). The flow inside PostProcessor::RunBloomPass is:
		//   1. Threshold extract: scene -> BloomTempA (half-res, RGBA16F)
		//        Pulls out pixels brighter than the user's threshold via
		//        a soft-knee falloff so the bloom doesn't pop on at the
		//        exact luminance boundary.
		//   2. Horizontal separable Gaussian: BloomTempA -> BloomTempB
		//        N-tap blur in the x direction. The tap count comes from
		//        BloomSettings::Quality (Low=7 / Med=13 / High=21 taps).
		//   3. Vertical separable Gaussian: BloomTempB -> BloomTempA
		//        Same kernel, applied along y. Combined H+V produces a
		//        true 2D Gaussian at ~N+N cost instead of N*N — the
		//        whole reason for going separable.
		//   4. Composite: scene + bloom -> dst
		//        Reads BOTH the source scene texture and the blurred
		//        bloom (BloomTempA), adds bloom * intensity * tint on
		//        top. Needs a 4-binding bgl (scene tex + bloom tex +
		//        sampler + uniform) — see Impl::BloomCompositeBgl.
		//
		// Doing the blur at half resolution amplifies the effective
		// radius (a 21-tap blur at half-res covers ~42 px at full-res)
		// and cuts shader work to 1/4 per blur pass, which is what makes
		// the separable approach affordable even at High quality.

		// Threshold extract — uses the existing 3-binding effect bgl.
		// Uniform: { threshold, _, _, _ }.
		constexpr const char* k_PostBloomThresholdWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

fn luma(c: vec3<f32>) -> f32 {
	return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let scene = textureSample(t_src, s_src, in.uv).rgb;
	let l = luma(scene);
	// Soft-knee threshold. The /0.5 keeps the falloff narrow enough
	// that a pixel only slightly over threshold contributes a small
	// fraction (avoids a hard binary cutoff).
	let excess = max(l - u.p.x, 0.0);
	let knee = excess / (excess + 0.5);
	return vec4<f32>(scene * knee, 1.0);
}
)WGSL";

		// Separable Gaussian blur — one pass per axis. Same shader
		// handles both H and V; the direction uniform picks which.
		// Uses the existing 3-binding effect bgl.
		// Uniform: { dirX, dirY, texelSize, halfKernel }
		//   dirX/Y: (1,0) for horizontal, (0,1) for vertical.
		//   texelSize: 1/sourceWidth or 1/sourceHeight in UV space.
		//   halfKernel: per-direction sample half-count (3/6/10 from
		//               BloomSettings::Quality).
		// halfK is uniform across the dispatch so Dawn accepts the
		// runtime-bounded loop.
		constexpr const char* k_PostBloomBlurWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let dir = u.p.xy;
	let texel = u.p.z;
	// Clamp upper bound bumped to 250 so the user-facing Taps slider
	// (7..500 taps per pass, mapping to halfK 3..250) fits with
	// headroom. Past ~250 the Gaussian tail is microscopic so the
	// extra taps are wasted GPU work, but the upper bound matches the
	// inspector's slider range. Loop is uniform-bounded across the
	// dispatch (halfK is a uniform), which lets Dawn unroll up to a
	// driver-dependent point.
	let half_k = i32(clamp(u.p.w, 1.0, 250.0));
	let half_kf = f32(half_k);

	// sigma chosen so the kernel-edge tap is ~exp(-4.5) ~ 0.011 of the
	// centre tap (3-sigma covers the kernel half). That gives a clean
	// Gaussian falloff without leaving a hard edge at the kernel boundary.
	let sigma = half_kf / 3.0;
	let sigma_inv2 = 1.0 / (2.0 * sigma * sigma);

	var result = vec3<f32>(0.0);
	var weight_sum = 0.0;
	for (var i = -half_k; i <= half_k; i = i + 1) {
		let offset = dir * f32(i) * texel;
		let sample = textureSample(t_src, s_src, in.uv + offset).rgb;
		let w = exp(-f32(i * i) * sigma_inv2);
		result = result + sample * w;
		weight_sum = weight_sum + w;
	}
	return vec4<f32>(result / weight_sum, 1.0);
}
)WGSL";

		// Composite — two source textures (scene + blurred bloom), output
		// scene + bloom * intensity * tint. This is the one bloom pass
		// that needs a new bgl shape: scene at binding 0, sampler at 1,
		// bloom at 2, uniform at 3.
		// Uniform: { tintR, tintG, tintB, intensity }
		constexpr const char* k_PostBloomCompositeWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_scene: texture_2d<f32>;
@group(0) @binding(1) var s_lin:   sampler;
@group(0) @binding(2) var t_bloom: texture_2d<f32>;
@group(0) @binding(3) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let scene = textureSample(t_scene, s_lin, in.uv);
	let bloom = textureSample(t_bloom, s_lin, in.uv).rgb;
	let bloomContribution = bloom * u.p.rgb * u.p.a;
	return vec4<f32>(scene.rgb + bloomContribution, scene.a);
}
)WGSL";

		// Pixelated — UV-quantise to a coarse grid of `BlockSize` pixels,
		// optionally palette-quantise the rgb channels to `PaletteSteps`
		// levels each. Sampling the quantised UV (at the cell centre)
		// produces uniform-coloured "fat pixels".
		//
		// Uniform layout (16 bytes, single vec4):
		//   p.x = BlockSize (in caller-space pixels)
		//   p.y = ViewportWidth  (in caller-space pixels)
		//   p.z = ViewportHeight (in caller-space pixels)
		//   p.w = PaletteSteps (0 = palette quantise off; >= 2 = levels/channel)
		constexpr const char* k_PostPixelatedWGSL = R"WGSL(
struct Params { p: vec4<f32>, };
@group(0) @binding(0) var t_src: texture_2d<f32>;
@group(0) @binding(1) var s_src: sampler;
@group(0) @binding(2) var<uniform> u: Params;

struct VertexOutput {
	@builtin(position) clip_position: vec4<f32>,
	@location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
	var pos = array<vec2<f32>, 3>(
		vec2<f32>(-1.0, -1.0), vec2<f32>( 3.0, -1.0), vec2<f32>(-1.0,  3.0));
	var uv = array<vec2<f32>, 3>(
		vec2<f32>(0.0, 1.0), vec2<f32>(2.0, 1.0), vec2<f32>(0.0, -1.0));
	var out: VertexOutput;
	out.clip_position = vec4<f32>(pos[idx], 0.0, 1.0);
	out.uv = uv[idx];
	return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
	let block = max(u.p.x, 1.0);
	let vw = max(u.p.y, 1.0);
	let vh = max(u.p.z, 1.0);
	let palette = u.p.w;

	// Snap UV to the centre of its block. floor((uv * pixelDims) / block)
	// gives the integer cell index; multiply back to UV and add a half-
	// block centre offset so the sample lands on cell centre instead of
	// cell edge (looks more stable when the grid scrolls).
	let cellsX = vw / block;
	let cellsY = vh / block;
	let cell = floor(vec2<f32>(in.uv.x * cellsX, in.uv.y * cellsY));
	let snappedUv = (cell + vec2<f32>(0.5, 0.5)) / vec2<f32>(cellsX, cellsY);

	var sampled = textureSample(t_src, s_src, snappedUv);

	if (palette >= 2.0) {
		// Per-channel quantise to `palette` levels: round(c * (n-1)) / (n-1).
		let n = palette - 1.0;
		sampled = vec4<f32>(
			round(sampled.r * n) / n,
			round(sampled.g * n) / n,
			round(sampled.b * n) / n,
			sampled.a
		);
	}
	return sampled;
}
)WGSL";

		constexpr BuiltIn k_BuiltIns[] = {
			{ "sprite",              k_SpriteWGSL              },
			{ "text",                k_TextWGSL                },
			{ "gizmo",               k_GizmoWGSL               },
			{ "postblit",            k_PostBlitWGSL            },
			{ "postvignette",        k_PostVignetteWGSL        },
			{ "postchromatic",       k_PostChromaticWGSL       },
			{ "postgrain",           k_PostGrainWGSL           },
			{ "postlensdistortion",  k_PostLensDistortionWGSL  },
			{ "postcolorgrading",    k_PostColorGradingWGSL    },
			{ "postbloomthreshold",  k_PostBloomThresholdWGSL  },
			{ "postbloomblur",       k_PostBloomBlurWGSL       },
			{ "postbloomcomposite",  k_PostBloomCompositeWGSL  },
			{ "postpixelated",       k_PostPixelatedWGSL       },
		};

		// ── Pool ────────────────────────────────────────────────────────────
		struct GpuShader {
			wgpu::ShaderModule Module;
			std::string        Name;
		};
		std::unordered_map<unsigned, GpuShader> g_Shaders;
		unsigned g_NextShaderId = 1;  // 0 reserved as "invalid"

		unsigned AllocateShaderSlot(GpuShader&& s) {
			unsigned id = g_NextShaderId++;
			if (id == 0) id = g_NextShaderId++;
			g_Shaders.emplace(id, std::move(s));
			return id;
		}

		void FreeShaderSlot(unsigned id) {
			if (id == 0) return;
			g_Shaders.erase(id);
		}

		// Stem extraction — strips path + extension, drops "Shader"
		// suffix, lowercases. Callers (WebGPUSpriteResources /
		// GizmoRenderer / TextRenderer) hit this to look up by name.
		std::string ExtractName(const std::string& path) {
			std::filesystem::path p(path);
			std::string stem = p.stem().string();
			static const std::string suffix = "Shader";
			if (stem.size() > suffix.size()
				&& stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				stem.erase(stem.size() - suffix.size());
			}
			for (char& c : stem) {
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			}
			return stem;
		}

		const char* FindBuiltinWGSL(std::string_view name) {
			for (const BuiltIn& b : k_BuiltIns) {
				if (b.Name == name) return b.WGSL;
			}
			return nullptr;
		}

		std::string ResolveOnDiskWGSL(const std::string& name) {
			const std::string rel = std::string("IndexAssets/Shaders/webgpu/") + name + ".wgsl";
			const std::string exeRel = Path::Combine(Path::ExecutableDir(), rel);
			if (std::filesystem::exists(exeRel)) return exeRel;
			if (std::filesystem::exists(rel)) return rel;
			return {};
		}

		// Build a wgpu::ShaderModule from WGSL text. Returns null module on
		// failure. The compilation-info callback fires asynchronously on
		// Dawn — we surface errors via the device's uncaptured-error hook
		// set up in WebGPUApi.cpp::RequestDeviceSync.
		wgpu::ShaderModule CompileWGSL(const std::string& name, std::string_view wgsl) {
			wgpu::Device device = WebGPUBackend::GetDevice();
			if (!device) {
				IDX_CORE_ERROR_TAG("Shader",
					"CompileWGSL '{}' called before WebGPU device exists", name);
				return nullptr;
			}

			wgpu::ShaderSourceWGSL src{};
			src.code = wgsl.data();

			wgpu::ShaderModuleDescriptor desc{};
			desc.nextInChain = &src;
			desc.label = name.c_str();

			wgpu::ShaderModule module = device.CreateShaderModule(&desc);
			if (!module) {
				IDX_CORE_ERROR_TAG("Shader",
					"wgpu::Device::CreateShaderModule returned null for '{}'", name);
			}
			return module;
		}
	}

	// ── WebGPUBackend::LookupShader (declared in WebGPUBackend.hpp) ─────────
	namespace WebGPUBackend {
		ShaderLookup LookupShader(unsigned shaderHandleId) {
			if (shaderHandleId == 0) return ShaderLookup{};
			auto it = g_Shaders.find(shaderHandleId);
			if (it == g_Shaders.end()) return ShaderLookup{};
			ShaderLookup out;
			out.Module = it->second.Module;
			out.Valid  = static_cast<bool>(out.Module);
			return out;
		}
	}

	// ── Shader ──────────────────────────────────────────────────────────────

	Shader::Shader(const std::string& vsPath, const std::string& /*fsPath*/) {
		// vsPath / fsPath were the two-source-files convention from the
		// OpenGL era. WebGPU collapses both stages into a single module,
		// so fsPath is ignored and the stem is extracted from vsPath.
		const std::string name = ExtractName(vsPath);

		std::string wgslHolder;          // owns the on-disk source if loaded
		std::string_view wgslSource;     // points into the holder OR a built-in

		if (const char* builtin = FindBuiltinWGSL(name)) {
			wgslSource = builtin;
		} else {
			const std::string path = ResolveOnDiskWGSL(name);
			if (path.empty()) {
				IDX_CORE_WARN_TAG("Shader",
					"No WGSL source for '{}' (no built-in, no IndexAssets/Shaders/webgpu/{}.wgsl) — Shader will be invalid",
					name, name);
				return;
			}
			std::vector<uint8_t> bytes = File::ReadAllBytes(path);
			if (bytes.empty()) {
				IDX_CORE_WARN_TAG("Shader", "WGSL file empty: {}", path);
				return;
			}
			wgslHolder.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			wgslSource = wgslHolder;
		}

		wgpu::ShaderModule module = CompileWGSL(name, wgslSource);
		if (!module) return;

		GpuShader slot;
		slot.Module = std::move(module);
		slot.Name   = name;

		m_Program = AllocateShaderSlot(std::move(slot));
		m_IsValid = (m_Program != 0);
	}

	Shader::Shader(GLuint /*program*/) {}

	Shader::~Shader() {
		if (m_Program != 0) {
			FreeShaderSlot(m_Program);
			m_Program = 0;
			m_IsValid = false;
		}
	}

	Shader::Shader(Shader&& other) noexcept
		: m_Program(other.m_Program)
		, m_IsValid(other.m_IsValid)
	{
		other.m_Program = 0;
		other.m_IsValid = false;
	}

	Shader& Shader::operator=(Shader&& other) noexcept {
		if (this != &other) {
			if (m_Program != 0) FreeShaderSlot(m_Program);
			m_Program = other.m_Program;
			m_IsValid = other.m_IsValid;
			other.m_Program = 0;
			other.m_IsValid = false;
		}
		return *this;
	}

	Shader Shader::FromBinary(const std::string& binaryPath) {
		// WebGPU has no pre-compiled binary format — .wgsl is the
		// canonical authoring format (SPIR-V via Tint is an option but
		// the engine doesn't ship a SPIR-V build step). Treat the input
		// as a stem and re-route through the regular constructor.
		return Shader{ binaryPath, binaryPath };
	}

	bool Shader::ExportBinary(const std::string& /*outputPath*/) const {
		// No native binary representation worth exporting — WGSL text is
		// already the source-of-truth.
		return false;
	}

	Shader Shader::LoadWithBinaryCache(const std::string& /*binaryPath*/,
		const std::string& vsPath, const std::string& fsPath)
	{
		return Shader{ vsPath, fsPath };
	}

	void Shader::Submit() const {
		// WebGPU binds shader modules via wgpu::RenderPipeline at
		// pipeline-creation time, not per-draw. Submit stays a no-op.
	}

	GLuint Shader::LoadAndCompile(GLenum /*type*/, const std::string& /*path*/) {
		// Legacy single-stage helper from the OpenGL era — unused.
		return 0;
	}

}  // namespace Index
