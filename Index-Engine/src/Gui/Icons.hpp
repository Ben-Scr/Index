#pragma once

// =============================================================================
// Index UI icons — bundled-PNG first, vector fallback.
// -----------------------------------------------------------------------------
// A small library of icon primitives (arrows, search, plus, pen) that the
// launcher and editor share for buttons, search bars, and breadcrumb chrome.
//
// Two-tier rendering:
//   1. Bundled PNGs at IndexAssets/Textures/Editor/General/<name>/<name>_<sz>.png
//      (shipped at 16/32/64/128 — same convention as the rest of the editor's
//      icon set, see Index-Editor/src/Gui/EditorIcons.cpp). Loaded lazily on
//      first request, cached by (name, snapped-size), drawn via ImDrawList::
//      AddImage with the standard `(0,1)→(1,0)` UV flip used everywhere else
//      in the editor (Texture2D defaults to flipVertical=true).
//   2. Geometric fallback drawn with line strokes — used if the PNG fails to
//      load (asset dir missing, file deleted, fresh build before postbuild
//      copy ran). Keeps the UI legible during dev iteration instead of
//      blanking out icon buttons.
//
// Lives in Index-Engine/src/Gui/ so both launcher and editor pick it up via
// the EngineCore include path (Index-Engine/src is on every consumer's
// includedirs — see Dependencies.lua: IncludeDir["IndexEngine"]).
//
// Cross-DLL: the texture cache below is a header-side inline static, so each
// consumer binary that includes Icons.hpp ends up with its own per-binary
// cache. That's correct, not wasteful — the launcher and editor are separate
// processes, and the GPU resources backing each Texture2D are owned by
// engine.dll's WebGPU device which the parent process already manages.
//
// Lifecycle: callers MUST invoke `Index::Icons::Shutdown()` before WebGPU
// tears down (typically from the consumer's top-level OnDetach), otherwise
// the static cache's destructors would run at process exit, after the GPU
// device has gone away — same rule the editor's EditorIcons::Shutdown
// follows.
//
// Sizing convention: every Draw* takes a square bounding-box edge length in
// pixels. Color is ImU32 — ImDrawList::AddImage multiplies the texture by
// it, so pass `ImGui::GetColorU32(ImGuiCol_Text)` for foreground or a
// dimmed/tinted variant for state-aware buttons.
// =============================================================================

#include "Graphics/Filter.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/Wrap.hpp"
#include "Serialization/Path.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace Index::Icons {

	enum class Type {
		ArrowUp,
		ArrowDown,
		ArrowLeft,
		ArrowRight,
		Search,
		Plus,
		Minus,
		Pen,
		Settings,
		Caret,
		Delete,
		Refresh,
		Clear,
		Reset,
		AspectRatio,
		Play,
		OpenFolder,
		FileManager,
		Copy,
		Info,
	};

	namespace Detail {

		// Map enum -> on-disk asset folder/name. Mirrors the directory layout
		// under IndexAssets/Textures/Editor/General/<name>/<name>_<size>.png
		// that the user-curated icon set ships with.
		//
		// Refresh and Reset both map to "reset" — it's the circular-arrow
		// glyph that reads as "reload" in every UI convention. The curated
		// set also ships "redo" (the curved arrow), but that one means
		// "redo last action" and is the wrong affordance on a refresh
		// button.
		inline const char* TypeName(Type t) {
			switch (t) {
			case Type::ArrowUp:     return "arrow_up";
			case Type::ArrowDown:   return "arrow_down";
			case Type::ArrowLeft:   return "arrow_left";
			case Type::ArrowRight:  return "arrow_right";
			case Type::Search:      return "search";
			case Type::Plus:        return "plus";
			case Type::Minus:       return "minus";
			case Type::Pen:         return "pen";
			case Type::Settings:    return "settings";
			case Type::Caret:       return "caret";
			case Type::Delete:      return "delete";
			case Type::Refresh:     return "reset";
			case Type::Clear:       return "clear";
			case Type::Reset:       return "reset";
			case Type::AspectRatio: return "aspect_ratio";
			case Type::Play:        return "play";
			case Type::OpenFolder:  return "open_folder";
			case Type::FileManager: return "file_manager";
			case Type::Copy:        return "copy";
			case Type::Info:        return "info";
			}
			return "";
		}

		// Snap a requested pixel size to the smallest available on-disk size.
		// Must mirror EditorIcons.cpp's k_AvailableSizes so the snap rule
		// stays consistent across both icon loaders.
		inline int SnapSize(int requested) {
			constexpr int k_Sizes[] = { 16, 32, 64, 128 };
			int best = k_Sizes[0];
			for (int s : k_Sizes) {
				best = s;
				if (s >= requested) break;
			}
			return best;
		}

		struct CacheEntry {
			Texture2D Tex;
			int Size = 0;
		};

		// Per-binary cache. Inline static (C++17) so this header has a single
		// definition per DLL that includes it; consumers (launcher / editor)
		// each get their own copy, which is what we want — they're separate
		// processes, not co-resident in one address space.
		inline std::unordered_map<std::string, CacheEntry>& Cache() {
			static std::unordered_map<std::string, CacheEntry> s_Cache;
			return s_Cache;
		}

		// Lazy-load (name, snapped-size). Returns the WebGPU texture handle
		// suitable for use as an ImTextureID. Returns 0 (also cached) if the
		// file is missing — callers fall back to the vector path below.
		inline uint64_t LoadTexture(Type type, int requestedSize) {
			const char* name = TypeName(type);
			if (!name || *name == '\0') return 0;

			const int snapped = SnapSize(requestedSize);
			std::string key = std::string(name) + "_" + std::to_string(snapped);

			auto& cache = Cache();
			auto it = cache.find(key);
			if (it != cache.end()) {
				// Includes negative-cache hits (handle == 0).
				return it->second.Tex.GetHandle();
			}

			const std::string assetsDir = Path::ResolveIndexAssets("Textures");
			if (assetsDir.empty()) {
				cache[key] = {};
				return 0;
			}

			const std::string filename = key + ".png";
			const std::string fullpath = Path::Combine(
				assetsDir, "Editor", "General", name, filename);

			CacheEntry entry;
			entry.Tex = Texture2D(fullpath.c_str(),
				Filter::Bilinear, Wrap::Clamp, Wrap::Clamp);
			entry.Size = snapped;

			// Negative-cache the miss so the next per-frame lookup doesn't
			// keep re-constructing Texture2D / stb_image-decoding the same
			// missing file. Move-into-place either way; IsValid() == false
			// still hands back handle 0.
			const uint64_t handle = entry.Tex.GetHandle();
			cache[std::move(key)] = std::move(entry);
			return handle;
		}

		// Default stroke thickness for the vector fallback. 1/8 of the bbox
		// edge balances visually with the default 15 px ImGui font weight.
		inline float DefaultThickness(float size) {
			return std::max(1.0f, size * 0.125f);
		}

		inline void DrawArrowChevron(ImDrawList* dl, ImVec2 pos, float size,
			ImGuiDir dir, ImU32 color, float thickness)
		{
			const float inset = size * 0.20f;
			const float left   = pos.x + inset;
			const float right  = pos.x + size - inset;
			const float top    = pos.y + inset;
			const float bottom = pos.y + size - inset;
			const float midX   = pos.x + size * 0.5f;
			const float midY   = pos.y + size * 0.5f;

			ImVec2 a, b, c;
			switch (dir) {
			case ImGuiDir_Up:
				a = ImVec2(left,  bottom);
				b = ImVec2(midX,  top);
				c = ImVec2(right, bottom);
				break;
			case ImGuiDir_Down:
				a = ImVec2(left,  top);
				b = ImVec2(midX,  bottom);
				c = ImVec2(right, top);
				break;
			case ImGuiDir_Left:
				a = ImVec2(right, top);
				b = ImVec2(left,  midY);
				c = ImVec2(right, bottom);
				break;
			case ImGuiDir_Right:
			default:
				a = ImVec2(left,  top);
				b = ImVec2(right, midY);
				c = ImVec2(left,  bottom);
				break;
			}
			dl->AddLine(a, b, color, thickness);
			dl->AddLine(b, c, color, thickness);
		}

		inline void DrawSearch(ImDrawList* dl, ImVec2 pos, float size,
			ImU32 color, float thickness)
		{
			const float radius = size * 0.30f;
			const ImVec2 center(pos.x + radius + size * 0.08f,
				pos.y + radius + size * 0.08f);
			dl->AddCircle(center, radius, color, 0, thickness);
			const float kSqrtHalf = 0.70710678f;
			const ImVec2 handleStart(center.x + radius * kSqrtHalf,
				center.y + radius * kSqrtHalf);
			const ImVec2 handleEnd(pos.x + size - size * 0.10f,
				pos.y + size - size * 0.10f);
			dl->AddLine(handleStart, handleEnd, color, thickness);
		}

		inline void DrawPlus(ImDrawList* dl, ImVec2 pos, float size,
			ImU32 color, float thickness)
		{
			const float inset = size * 0.20f;
			const float midX  = pos.x + size * 0.5f;
			const float midY  = pos.y + size * 0.5f;
			dl->AddLine(ImVec2(pos.x + inset,        midY),
				ImVec2(pos.x + size - inset, midY),
				color, thickness);
			dl->AddLine(ImVec2(midX, pos.y + inset),
				ImVec2(midX, pos.y + size - inset),
				color, thickness);
		}

		inline void DrawPen(ImDrawList* dl, ImVec2 pos, float size,
			ImU32 color, float thickness)
		{
			const float inset = size * 0.18f;
			const float w     = size - inset * 2.0f;
			const float dx    = w * 0.18f;
			const ImVec2 topR (pos.x + size - inset, pos.y + inset);
			const ImVec2 botL (pos.x + inset,        pos.y + size - inset);
			const ImVec2 topR1(topR.x + dx * 0.5f, topR.y - dx * 0.5f);
			const ImVec2 topR2(topR.x - dx * 0.5f, topR.y + dx * 0.5f);
			const ImVec2 botL1(botL.x + dx * 0.5f, botL.y - dx * 0.5f);
			const ImVec2 botL2(botL.x - dx * 0.5f, botL.y + dx * 0.5f);
			dl->AddLine(topR1, botL1, color, thickness);
			dl->AddLine(topR2, botL2, color, thickness);
			dl->AddLine(topR1, topR2, color, thickness);
			dl->AddLine(botL1, botL2, color, thickness);
		}

		inline void DrawVectorFallback(ImDrawList* dl, Type type, ImVec2 pos,
			float size, ImU32 color)
		{
			const float t = DefaultThickness(size);
			switch (type) {
			case Type::ArrowUp:    DrawArrowChevron(dl, pos, size, ImGuiDir_Up,    color, t); break;
			case Type::ArrowDown:  DrawArrowChevron(dl, pos, size, ImGuiDir_Down,  color, t); break;
			case Type::ArrowLeft:  DrawArrowChevron(dl, pos, size, ImGuiDir_Left,  color, t); break;
			case Type::ArrowRight: DrawArrowChevron(dl, pos, size, ImGuiDir_Right, color, t); break;
			case Type::Search:     DrawSearch(dl, pos, size, color, t); break;
			case Type::Plus:       DrawPlus(dl, pos, size, color, t); break;
			case Type::Pen:        DrawPen(dl, pos, size, color, t); break;
			// Settings / Caret / Delete / Refresh / Clear / Reset / AspectRatio
			// have no vector fallback — they only render via the bundled PNGs.
			// If a PNG ever goes missing the slot stays blank rather than
			// printing a guessed sketch that wouldn't match the curated set.
			default: break;
			}
		}

	} // namespace Detail

	// Fetch the backing texture handle. Useful for direct ImGui::Image /
	// ImGui::ImageButton call sites that want to do their own layout. Pass
	// the *display* size in pixels; the loader snaps up to the nearest
	// shipped asset size. Returns 0 if the bundled PNG can't be found —
	// callers should `if (handle) ImGui::Image(...)` and fall back as needed,
	// or simply use Draw/IconButton/ButtonWithIcon/TextIcon below which
	// already do this for you.
	inline uint64_t Get(Type type, int displaySize = 16) {
		return Detail::LoadTexture(type, displaySize);
	}

	// Release every cached Texture2D. MUST be called before the WebGPU
	// device shuts down — otherwise the cache map's static destructor
	// runs at process exit (after WebGPUBackend::Shutdown) and Texture2D's
	// destructor would try to talk to a dead device. Mirrors the rule
	// EditorIcons::Shutdown follows for the texture-based editor icon
	// set; both should be called from the same teardown site.
	inline void Shutdown() {
		auto& cache = Detail::Cache();
		for (auto& [k, v] : cache) v.Tex.Destroy();
		cache.clear();
	}

	// Low-level: draw an icon into a draw list. `pos` is the top-left of the
	// square bbox; `size` is its edge length in pixels. Tries the bundled
	// PNG first; falls back to a vector stroke if the asset isn't on disk.
	// `color` tints the icon: ImDrawList::AddImage multiplies the texture
	// by it (icons ship as alpha-on-white silhouettes, so a tinted text
	// colour just colours the silhouette).
	inline void Draw(ImDrawList* dl, Type type, ImVec2 pos, float size,
		ImU32 color)
	{
		if (!dl || size <= 0.0f) return;

		const uint64_t tex = Detail::LoadTexture(type, static_cast<int>(size + 0.5f));
		if (tex != 0) {
			// Editor-wide convention for Texture2D-backed icons: load with
			// flipVertical=true (the default), display with the UV pair
			// flipped on Y so the image reads right-side up. See e.g.
			// ImGuiEditorLayerPanels.cpp filter-button blocks.
			dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex)),
				pos, ImVec2(pos.x + size, pos.y + size),
				ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
				color);
			return;
		}

		Detail::DrawVectorFallback(dl, type, pos, size, color);
	}

	// Square icon-only button. Same hover/active framing + disabled handling
	// as ImGui::Button. Size defaults to a frame-height square; pass a
	// non-zero ImVec2 to override.
	inline bool IconButton(const char* id, Type type, ImVec2 size = ImVec2(0, 0)) {
		const float frameH = ImGui::GetFrameHeight();
		if (size.x <= 0.0f) size.x = frameH;
		if (size.y <= 0.0f) size.y = frameH;

		const ImVec2 cursor = ImGui::GetCursorScreenPos();

		// ##id label: ImGui::Button gives us the frame + hover/active colours
		// + disabled handling for free; the icon is overlaid on top.
		std::string label = std::string("##") + (id ? id : "icon");
		const bool clicked = ImGui::Button(label.c_str(), size);

		// Icon a tad smaller than the font so chevron strokes don't crowd
		// the rounded frame corners. The Text colour gets ImGui's auto
		// disabled-alpha multiplier applied for free, so we don't need a
		// separate disabled-state branch here. The hover/active framing
		// is owned by ImGui::Button above — we only paint the icon foreground.
		const float iconSize = ImGui::GetFontSize() * 0.95f;
		const ImVec2 iconPos(
			cursor.x + (size.x - iconSize) * 0.5f,
			cursor.y + (size.y - iconSize) * 0.5f);
		Draw(ImGui::GetWindowDrawList(), type, iconPos, iconSize,
			ImGui::GetColorU32(ImGuiCol_Text));
		return clicked;
	}

	// Text button with a leading icon. Drawn fully ourselves (no underlying
	// ImGui::Button) so we control the icon+text layout directly without
	// paying for a double-paint. Width=0 auto-fits to [icon][spacing][text]
	// + 2× FramePadding. Width<0 fills the remaining content region like
	// ImGui::Button does (-1 = full width). Height=0 uses frame height.
	//
	// The full `label` string is used as the widget ID; everything up to
	// the first `##` is the display name. Append `##unique` for buttons
	// that share a display name — same convention as ImGui::Button.
	inline bool ButtonWithIcon(Type type, const char* label,
		ImVec2 size = ImVec2(0, 0),
		bool centerContent = false)
	{
		if (!label) return false;

		ImGuiStyle& style = ImGui::GetStyle();
		const float iconSize = ImGui::GetFontSize();
		const float spacing  = style.ItemInnerSpacing.x;

		const char* displayEnd = ImGui::FindRenderedTextEnd(label);
		const ImVec2 textSize = ImGui::CalcTextSize(label, displayEnd);

		ImVec2 buttonSize = size;
		if (buttonSize.x == 0.0f) {
			buttonSize.x = iconSize + spacing + textSize.x + style.FramePadding.x * 2.0f;
		} else if (buttonSize.x < 0.0f) {
			// Mirror ImGui::CalcItemSize: clamp the "fill remaining" path
			// to at least 4 px so a fully-consumed content region doesn't
			// trip ImGui::InvisibleButton's "size must be > 0" assertion.
			buttonSize.x = std::max(4.0f,
				ImGui::GetContentRegionAvail().x + buttonSize.x + 1.0f);
		}
		if (buttonSize.y == 0.0f) {
			buttonSize.y = ImGui::GetFrameHeight();
		}

		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton(label, buttonSize);
		const bool active  = ImGui::IsItemActive();
		const bool hovered = ImGui::IsItemHovered();

		// Frame colour rule mirrors ImGui::ButtonEx.
		const ImU32 bgColor = ImGui::GetColorU32(
			active  ? ImGuiCol_ButtonActive
			: hovered ? ImGuiCol_ButtonHovered
			: ImGuiCol_Button);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(cursor,
			ImVec2(cursor.x + buttonSize.x, cursor.y + buttonSize.y),
			bgColor, style.FrameRounding);

		// When centerContent is true, the icon+spacing+text block is centered
		// inside the button rect instead of left-anchored at FramePadding.x —
		// matches Unity-style "Add Component" buttons whose content sits in
		// the middle of a full-width button.
		const float contentWidth = iconSize + spacing + textSize.x;
		float startX;
		if (centerContent) {
			startX = cursor.x + (buttonSize.x - contentWidth) * 0.5f;
			if (startX < cursor.x + style.FramePadding.x) {
				startX = cursor.x + style.FramePadding.x;
			}
		} else {
			startX = cursor.x + style.FramePadding.x;
		}
		const float iconY    = cursor.y + (buttonSize.y - iconSize) * 0.5f;
		const float textY    = cursor.y + (buttonSize.y - textSize.y) * 0.5f;
		const ImU32 textCol  = ImGui::GetColorU32(ImGuiCol_Text);

		Draw(dl, type, ImVec2(startX, iconY), iconSize, textCol);
		dl->AddText(ImVec2(startX + iconSize + spacing, textY),
			textCol, label, displayEnd);

		return clicked;
	}

	// Stamp an icon at the current cursor position as if it were a glyph,
	// then advance with SameLine so subsequent widgets sit next to it.
	// Useful as a leading decoration before an InputText / Button on the
	// same line.
	//
	// Vertical alignment: we center inside a *FrameHeight*-tall row, not a
	// TextLineHeight-tall row. ImGui::Begin's caret puts the icon's top at
	// cursor.y; the framed widget that follows on the same line draws its
	// text inset by FramePadding.y, so a TextLineHeight-tall icon would
	// visibly sit ~FramePadding.y higher than the text inside the input
	// field next to it ("a bit too far up"). Reserving FrameHeight and
	// centering the icon inside it lines the icon's vertical center up
	// with text-in-frame, which is what every current caller (launcher
	// search bar, AddComponentPopup search, AddSystemPopup search,
	// ReferencePicker search) actually wants.
	inline void TextIcon(Type type, float size = 0.0f) {
		const float iconSize = (size > 0.0f) ? size : ImGui::GetFontSize();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float frameH = ImGui::GetFrameHeight();
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const float yOffset = (frameH - iconSize) * 0.5f;
		Draw(ImGui::GetWindowDrawList(), type,
			ImVec2(cursor.x, cursor.y + yOffset), iconSize,
			ImGui::GetColorU32(ImGuiCol_Text));
		// Reserve FrameHeight so this row's tracked max-height already
		// matches what the next framed widget will produce — no extra
		// inter-row gap, no surprise reflow.
		ImGui::Dummy(ImVec2(iconSize, frameH));
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	}

	// Popup-menu row with a leading icon. Mirrors ImGui::MenuItem
	// semantics — full-width clickable row, click auto-closes the
	// parent popup (Selectable's default inside a BeginPopup* scope)
	// — but renders a rounded hover/active highlight that spans the
	// full popup width regardless of label length, so rows look
	// consistent. The visible label hashes into the widget ID; for
	// menu items that share a display name, append `##unique` like
	// with ImGui::Button/MenuItem.
	inline bool MenuItemWithIcon(Type type, const char* label) {
		if (!label) return false;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float iconSize = ImGui::GetFontSize();
		const float spacing  = style.ItemInnerSpacing.x;

		const char* displayEnd = ImGui::FindRenderedTextEnd(label);
		const ImVec2 textSize = ImGui::CalcTextSize(label, displayEnd);

		// Look up the highlight colours *before* pushing the transparent
		// overrides below — we still want the real Header colours for
		// our own rounded fill.
		const ImU32 hoverCol  = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
		const ImU32 activeCol = ImGui::GetColorU32(ImGuiCol_HeaderActive);

		// Suppress Selectable's built-in highlight by zeroing its
		// Header* colours: it uses RenderFrame with hard-coded 0
		// rounding, so the only way to round the row is to draw the
		// fill ourselves. We restore the colours immediately after.
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0,0,0,0));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(0,0,0,0));

		// SpanAvailWidth: hit area extends to the popup's right edge so
		// every row has the same clickable width. The size-hint feeds
		// the popup's content-fit pass (without it, the hidden-label
		// Selectable would collapse the popup to a sliver because it
		// contributes zero content width).
		const ImVec2 sizeHint(iconSize + spacing + textSize.x, 0.0f);
		ImGui::PushID(label);
		const bool clicked = ImGui::Selectable("##menuitem_icon", false,
			ImGuiSelectableFlags_SpanAvailWidth, sizeHint);
		const bool hovered = ImGui::IsItemHovered();
		const bool active  = ImGui::IsItemActive();
		ImGui::PopID();

		ImGui::PopStyleColor(2);

		// SpanAvailWidth makes the item rect cover the full popup
		// width, so painting our fill against ItemRectMin/Max gives a
		// row-spanning, rounded highlight identical for every entry.
		const ImVec2 rectMin = ImGui::GetItemRectMin();
		const ImVec2 rectMax = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hovered || active) {
			dl->AddRectFilled(rectMin, rectMax,
				active ? activeCol : hoverCol,
				style.FrameRounding);
		}

		const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
		const float rowH    = rectMax.y - rectMin.y;
		const float iconY   = rectMin.y + (rowH - iconSize) * 0.5f;
		Draw(dl, type, ImVec2(rectMin.x, iconY), iconSize, textCol);

		const float textY = rectMin.y + (rowH - textSize.y) * 0.5f;
		dl->AddText(ImVec2(rectMin.x + iconSize + spacing, textY),
			textCol, label, displayEnd);

		return clicked;
	}

}  // namespace Index::Icons
