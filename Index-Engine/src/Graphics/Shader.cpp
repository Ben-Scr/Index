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


namespace Index {

	namespace {
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
	@location(4) i_data3: vec4<f32>,  // Sprite slice UV rect: (u0, v0, u1, v1)
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
	// Unit-quad UV: [-0.5, 0.5] -> [0, 1] on X, Y flipped so texture top-left
	// shows at the quad's visual top (engine has Y-up world space; textures
	// are top-left origin per stb's load).
	let baseUV = vec2<f32>(in.position.x + 0.5, 0.5 - in.position.y);
	// Remap unit-quad UV into the sprite-slice rect via mix(). For the
	// default full-texture case (UvRect = 0,0,1,1) this collapses to
	// identity — every legacy SpriteRenderer renders exactly as before.
	out.uv = vec2<f32>(
		mix(in.i_data3.x, in.i_data3.z, baseUV.x),
		mix(in.i_data3.y, in.i_data3.w, baseUV.y)
	);
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

		// Uniform layout: colorFilter=vec4(rgb,1), exposureCS=vec4(exposure,contrast,saturation,_), tempTint=vec4(temp,tint,_,_).
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

		// 4-pass bloom: (1) threshold extract at half-res, (2) H blur, (3) V blur, (4) composite scene+bloom. Separable H+V at half-res makes High-quality (21-tap) affordable.
		// Threshold extract — Uniform: { threshold, _, _, _ }.
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

		// Separable Gaussian blur, shared by H and V passes. Uniform: { dirX, dirY, texelSize, halfKernel }; halfK is a uniform so Dawn accepts the runtime-bounded loop.
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

		// Composite uses a 4-binding bgl: scene@0, sampler@1, bloom@2, uniform@3. Uniform: { tintR, tintG, tintB, intensity }.
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

		// Uniform: p = { BlockSize, ViewportWidth, ViewportHeight, PaletteSteps } (0=palette off, >=2=levels/channel).
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
		// fsPath ignored — WebGPU uses a single WGSL module; stem extracted from vsPath.
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
