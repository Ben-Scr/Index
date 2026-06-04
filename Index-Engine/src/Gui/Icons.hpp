#pragma once

// Index UI icons — bundled PNG (IndexAssets/Textures/Editor/General/<name>/<name>_<sz>.png) with geometric vector fallback.
// MUST call Shutdown() before WebGPU tears down; each binary has its own per-binary cache (inline static).

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

		// Refresh and Reset both map to "reset" (circular-arrow glyph); "redo" is the wrong affordance for a refresh button.
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

		// Must mirror EditorIcons.cpp's k_AvailableSizes.
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

		inline std::unordered_map<std::string, CacheEntry>& Cache() {
			static std::unordered_map<std::string, CacheEntry> s_Cache;
			return s_Cache;
		}

		inline uint64_t LoadTexture(Type type, int requestedSize) {
			const char* name = TypeName(type);
			if (!name || *name == '\0') return 0;

			const int snapped = SnapSize(requestedSize);
			std::string key = std::string(name) + "_" + std::to_string(snapped);

			auto& cache = Cache();
			auto it = cache.find(key);
			if (it != cache.end()) {
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

			const uint64_t handle = entry.Tex.GetHandle();
			cache[std::move(key)] = std::move(entry);
			return handle;
		}

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
			default: break;
			}
		}

	} // namespace Detail

	inline uint64_t Get(Type type, int displaySize = 16) {
		return Detail::LoadTexture(type, displaySize);
	}

	// MUST be called before WebGPU shuts down; Texture2D destructors would otherwise run against a dead device.
	inline void Shutdown() {
		auto& cache = Detail::Cache();
		for (auto& [k, v] : cache) v.Tex.Destroy();
		cache.clear();
	}

	inline void Draw(ImDrawList* dl, Type type, ImVec2 pos, float size,
		ImU32 color)
	{
		if (!dl || size <= 0.0f) return;

		const uint64_t tex = Detail::LoadTexture(type, static_cast<int>(size + 0.5f));
		if (tex != 0) {
			dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex)),
				pos, ImVec2(pos.x + size, pos.y + size),
				ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
				color);
			return;
		}

		Detail::DrawVectorFallback(dl, type, pos, size, color);
	}

	inline bool IconButton(const char* id, Type type, ImVec2 size = ImVec2(0, 0)) {
		const float frameH = ImGui::GetFrameHeight();
		if (size.x <= 0.0f) size.x = frameH;
		if (size.y <= 0.0f) size.y = frameH;

		const ImVec2 cursor = ImGui::GetCursorScreenPos();

		std::string label = std::string("##") + (id ? id : "icon");
		const bool clicked = ImGui::Button(label.c_str(), size);

		const float iconSize = ImGui::GetFontSize() * 0.95f;
		const ImVec2 iconPos(
			cursor.x + (size.x - iconSize) * 0.5f,
			cursor.y + (size.y - iconSize) * 0.5f);
		Draw(ImGui::GetWindowDrawList(), type, iconPos, iconSize,
			ImGui::GetColorU32(ImGuiCol_Text));
		return clicked;
	}

	// Frameless IconButton: paints only the glyph (no button background), dimmed at rest and full-strength on hover so it still reads as clickable. iconSize <= 0 fills the hit box.
	inline bool IconButtonBorderless(const char* id, Type type,
		ImVec2 size = ImVec2(0, 0), float iconSize = 0.0f)
	{
		const float frameH = ImGui::GetFrameHeight();
		if (size.x <= 0.0f) size.x = frameH;
		if (size.y <= 0.0f) size.y = frameH;
		if (iconSize <= 0.0f) iconSize = std::min(size.x, size.y);

		const ImVec2 cursor = ImGui::GetCursorScreenPos();

		std::string label = std::string("##") + (id ? id : "icon");
		const bool clicked = ImGui::InvisibleButton(label.c_str(), size);

		ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		if (!ImGui::IsItemHovered() && !ImGui::IsItemActive()) col.w *= 0.75f;

		const ImVec2 iconPos(
			cursor.x + (size.x - iconSize) * 0.5f,
			cursor.y + (size.y - iconSize) * 0.5f);
		Draw(ImGui::GetWindowDrawList(), type, iconPos, iconSize,
			ImGui::GetColorU32(col));
		return clicked;
	}

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

		const ImU32 bgColor = ImGui::GetColorU32(
			active  ? ImGuiCol_ButtonActive
			: hovered ? ImGuiCol_ButtonHovered
			: ImGuiCol_Button);

		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(cursor,
			ImVec2(cursor.x + buttonSize.x, cursor.y + buttonSize.y),
			bgColor, style.FrameRounding);

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

	// Stamp an icon then SameLine. Centers inside FrameHeight so it aligns with adjacent framed widgets (InputText etc.), not just TextLineHeight.
	inline void TextIcon(Type type, float size = 0.0f) {
		const float iconSize = (size > 0.0f) ? size : ImGui::GetFontSize();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float frameH = ImGui::GetFrameHeight();
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const float yOffset = (frameH - iconSize) * 0.5f;
		Draw(ImGui::GetWindowDrawList(), type,
			ImVec2(cursor.x, cursor.y + yOffset), iconSize,
			ImGui::GetColorU32(ImGuiCol_Text));
		// Reserve FrameHeight (not iconSize) so the row height matches adjacent framed widgets.
		ImGui::Dummy(ImVec2(iconSize, frameH));
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	}

	// Rounded full-width popup row. Shares a display name? append ##unique to disambiguate.
	inline bool MenuItemWithIcon(Type type, const char* label) {
		if (!label) return false;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float iconSize = ImGui::GetFontSize();
		const float spacing  = style.ItemInnerSpacing.x;

		const char* displayEnd = ImGui::FindRenderedTextEnd(label);
		const ImVec2 textSize = ImGui::CalcTextSize(label, displayEnd);

		const ImU32 hoverCol  = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
		const ImU32 activeCol = ImGui::GetColorU32(ImGuiCol_HeaderActive);

		// Zero Header* colours so Selectable's built-in 0-rounding highlight is invisible; we paint our own rounded fill.
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0,0,0,0));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(0,0,0,0));

		// sizeHint prevents the popup from collapsing when the Selectable label is hidden (##).
		const ImVec2 sizeHint(iconSize + spacing + textSize.x, 0.0f);
		ImGui::PushID(label);
		const bool clicked = ImGui::Selectable("##menuitem_icon", false,
			ImGuiSelectableFlags_SpanAvailWidth, sizeHint);
		const bool hovered = ImGui::IsItemHovered();
		const bool active  = ImGui::IsItemActive();
		ImGui::PopID();

		ImGui::PopStyleColor(2);

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
