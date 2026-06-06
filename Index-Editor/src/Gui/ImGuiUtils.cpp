#include <pch.hpp>
#include "Gui/ImGuiUtils.hpp"
#include "Graphics/Texture2D.hpp"
#include "Scene/Scene.hpp"
#include "Collections/Curve.hpp"
#include <imgui.h>
#include <cmath>

namespace Index::ImGuiUtils {
	void MarkSelectionDirty(std::span<const Entity> entities)
	{
		for (const Entity& e : entities) {
			if (Scene* scene = const_cast<Entity&>(e).GetScene()) {
				scene->MarkDirty();
			}
		}
	}

	bool DrawCurveEditor(const char* id, Curve& curve, bool enabled)
	{
		using Key = Curve::Key;
		auto& keys = curve.Keys;
		if (keys.size() < 2) keys = Curve::DefaultKeys();
		constexpr float kYMin = 0.0f, kYMax = 2.0f;
		constexpr float kKeyR = 5.0f;   // key handle radius
		constexpr float kTanR = 4.0f;   // tangent handle radius
		constexpr float kSnap = 0.15f;  // Left-Ctrl drag-snap increment
		ImGui::PushID(id);
		bool changed = false;

		// Pad the clickable area beyond the drawn graph: endpoint dots sit on the graph
		// edges and straddle them, so the button must extend by the dot radius for clicks
		// on the outer half of those dots to register.
		constexpr float kPad = kKeyR + 2.0f;
		const ImVec2 outer = ImGui::GetCursorScreenPos();
		const ImVec2 fullSize(std::max(80.0f, ImGui::GetContentRegionAvail().x), 140.0f + 2.0f * kPad);
		ImGui::InvisibleButton("##curve", fullSize,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const ImVec2 p0(outer.x + kPad, outer.y + kPad);
		const ImVec2 p1(outer.x + fullSize.x - kPad, outer.y + fullSize.y - kPad);
		const ImVec2 size(p1.x - p0.x, p1.y - p0.y);
		const ImVec2 btnMin = outer;
		const ImVec2 btnMax(outer.x + fullSize.x, outer.y + fullSize.y);
		const bool hovered = enabled && ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();

		// Dim every drawn element when the editor is disabled so the graph reads as
		// inactive (greyed) rather than vanishing.
		auto col = [&](int r, int g, int b, int a) -> ImU32 {
			return IM_COL32(r, g, b, enabled ? a : a * 35 / 100);
		};

		dl->AddRectFilled(p0, p1, col(25, 25, 28, 255));
		dl->AddRect(p0, p1, col(70, 70, 78, 255));
		for (int i = 1; i < 4; ++i) {
			const float gy = p0.y + size.y * (static_cast<float>(i) / 4.0f);
			dl->AddLine(ImVec2(p0.x, gy), ImVec2(p1.x, gy), col(48, 48, 54, 255));
		}
		const float refY = p1.y - ((1.0f - kYMin) / (kYMax - kYMin)) * size.y;
		dl->AddLine(ImVec2(p0.x, refY), ImVec2(p1.x, refY), col(95, 95, 105, 255)); // y = 1 reference

		auto toScreen = [&](const Vec2& v) -> ImVec2 {
			const float sx = p0.x + std::clamp(v.x, 0.0f, 1.0f) * size.x;
			const float sy = p1.y - (std::clamp(v.y, kYMin, kYMax) - kYMin) / (kYMax - kYMin) * size.y;
			return ImVec2(sx, sy);
		};
		auto fromScreen = [&](const ImVec2& s) -> Vec2 {
			const float x = std::clamp((s.x - p0.x) / std::max(1.0f, size.x), 0.0f, 1.0f);
			const float y = std::clamp(kYMin + (p1.y - s.y) / std::max(1.0f, size.y) * (kYMax - kYMin), kYMin, kYMax);
			return Vec2{ x, y };
		};
		auto inHandle  = [](const Key& k) { return Vec2{ k.Pos.x + k.InTangent.x,  k.Pos.y + k.InTangent.y }; };
		auto outHandle = [](const Key& k) { return Vec2{ k.Pos.x + k.OutTangent.x, k.Pos.y + k.OutTangent.y }; };

		// Tangent handles draw at a fixed, short screen length along the tangent direction,
		// ray-clamped to the widget so they stay on-screen and grabbable. Drag sets the
		// tangent's angle; the handle keeps this constant compact length.
		const float kHandleLen = 28.0f;
		auto handleDot = [&](const ImVec2& keyS, const Vec2& off) -> ImVec2 {
			float dx = off.x * size.x;
			float dy = -off.y / (kYMax - kYMin) * size.y;
			float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-4f) { dx = (off.x < 0.0f) ? -1.0f : 1.0f; dy = 0.0f; len = 1.0f; }
			dx = dx / len * kHandleLen;
			dy = dy / len * kHandleLen;
			float t = 1.0f;
			if (dx >  1e-4f)      t = std::min(t, (btnMax.x - keyS.x) / dx);
			else if (dx < -1e-4f) t = std::min(t, (btnMin.x - keyS.x) / dx);
			if (dy >  1e-4f)      t = std::min(t, (btnMax.y - keyS.y) / dy);
			else if (dy < -1e-4f) t = std::min(t, (btnMin.y - keyS.y) / dy);
			t = std::clamp(t, 0.0f, 1.0f);
			return ImVec2(keyS.x + dx * t, keyS.y + dy * t);
		};

		// Curve: sample each bezier segment into a polyline.
		for (std::size_t i = 1; i < keys.size(); ++i) {
			const Vec2 c0 = keys[i - 1].Pos;
			const Vec2 c1 = outHandle(keys[i - 1]);
			const Vec2 c2 = inHandle(keys[i]);
			const Vec2 c3 = keys[i].Pos;
			constexpr int kSamples = 24;
			ImVec2 prevS = toScreen(c0);
			for (int s = 1; s <= kSamples; ++s) {
				const float t = static_cast<float>(s) / static_cast<float>(kSamples);
				const ImVec2 curS = toScreen(Curve::BezierPoint(c0, c1, c2, c3, t));
				dl->AddLine(prevS, curS, col(120, 200, 255, 255), 2.0f);
				prevS = curS;
			}
		}

		// Persistent selection + drag target, keyed to this curve's address so it resets
		// when a different graph is shown. Part: 0 = key, 1 = in-tangent, 2 = out-tangent.
		static const void* s_OwnerKeys = nullptr;
		static int s_Selected = -1;
		static int s_DragKey = -1;
		static int s_DragPart = -1;
		static int s_CtxKey = -1;
		if (s_OwnerKeys != static_cast<const void*>(&curve)) {
			s_OwnerKeys = static_cast<const void*>(&curve);
			s_Selected = s_DragKey = s_DragPart = s_CtxKey = -1;
		}
		if (s_Selected >= static_cast<int>(keys.size())) s_Selected = -1;

		if (enabled) {
			const ImVec2 mouse = ImGui::GetMousePos();
			const bool ctrlSnap = ImGui::IsKeyDown(ImGuiKey_LeftCtrl);
			auto snap = [](float v) { return std::round(v / kSnap) * kSnap; };
			auto withinR = [&](const ImVec2& sp, float r) {
				const float dx = mouse.x - sp.x, dy = mouse.y - sp.y;
				return dx * dx + dy * dy <= (r + 3.0f) * (r + 3.0f);
			};

			int hitKey = -1, hitPart = -1;
			if (s_Selected >= 0 && s_Selected < static_cast<int>(keys.size())) {
				const Key& k = keys[s_Selected];
				const ImVec2 keyS = toScreen(k.Pos);
				if (s_Selected > 0 && withinR(handleDot(keyS, k.InTangent), kTanR)) { hitKey = s_Selected; hitPart = 1; }
				else if (s_Selected < static_cast<int>(keys.size()) - 1 && withinR(handleDot(keyS, k.OutTangent), kTanR)) { hitKey = s_Selected; hitPart = 2; }
			}
			if (hitPart < 0) {
				for (std::size_t i = 0; i < keys.size(); ++i) {
					if (withinR(toScreen(keys[i].Pos), kKeyR)) { hitKey = static_cast<int>(i); hitPart = 0; break; }
				}
			}

			if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				s_DragKey = hitKey;
				s_DragPart = hitPart;
				if (hitPart == 0) s_Selected = hitKey;
			}
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				s_DragKey = -1; s_DragPart = -1;
			}

			if (s_DragKey >= 0 && s_DragKey < static_cast<int>(keys.size())
				&& ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
				Key& k = keys[static_cast<std::size_t>(s_DragKey)];
				if (s_DragPart == 0) {
					Vec2 np = fromScreen(mouse);
					if (ctrlSnap) { np.x = snap(np.x); np.y = snap(np.y); }
					const bool isFirst = (s_DragKey == 0);
					const bool isLast = (s_DragKey == static_cast<int>(keys.size()) - 1);
					if (isFirst) np.x = 0.0f;
					else np.x = std::max(np.x, keys[s_DragKey - 1].Pos.x + 0.001f);
					if (isLast) np.x = 1.0f;
					else np.x = std::min(np.x, keys[s_DragKey + 1].Pos.x - 0.001f);
					k.Pos = np;
					changed = true;
				}
				else if (s_DragPart == 1 || s_DragPart == 2) {
					// Drag sets the tangent ANGLE; its magnitude is fixed relative to the
					// neighbouring segment so the handle keeps a consistent (long) length.
					const ImVec2 keyS = toScreen(k.Pos);
					float dx = (mouse.x - keyS.x) / std::max(1.0f, size.x);
					float dy = -(mouse.y - keyS.y) / std::max(1.0f, size.y) * (kYMax - kYMin);
					const float l = std::sqrt(dx * dx + dy * dy);
					if (l > 1e-4f) { dx /= l; dy /= l; }
					const bool isIn = (s_DragPart == 1);
					const float segW = isIn
						? (s_DragKey > 0 ? k.Pos.x - keys[s_DragKey - 1].Pos.x : 0.5f)
						: (s_DragKey < static_cast<int>(keys.size()) - 1 ? keys[s_DragKey + 1].Pos.x - k.Pos.x : 0.5f);
					const float mag = 0.45f * std::max(0.05f, segW);
					Vec2 off{ dx * mag, dy * mag };
					off.x = isIn ? std::clamp(off.x, -segW, 0.0f) : std::clamp(off.x, 0.0f, segW);
					if (ctrlSnap) { off.x = snap(off.x); off.y = snap(off.y); }
					if (isIn) k.InTangent = off; else k.OutTangent = off;
					changed = true;
				}
			}

			if (hovered && hitPart < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				Vec2 m = fromScreen(mouse);
				if (ctrlSnap) { m.x = snap(m.x); m.y = snap(m.y); }
				if (m.x > 1e-3f && m.x < 1.0f - 1e-3f) {
					Key nk; nk.Pos = m;
					auto it = std::lower_bound(keys.begin(), keys.end(), m.x,
						[](const Key& k, float x) { return k.Pos.x < x; });
					s_Selected = static_cast<int>(it - keys.begin());
					keys.insert(it, nk);
					changed = true;
				}
			}
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
				s_CtxKey = (hitPart == 0) ? hitKey : -1;
				if (hitPart == 0) s_Selected = hitKey;
			}

			// Shared clipboard so curve values can be copy/pasted across any graphs.
			static Curve s_Clipboard;
			static bool s_HasClipboard = false;

			if (ImGui::BeginPopupContextItem("##curveCtx")) {
				if (ImGui::MenuItem("Copy values")) {
					s_Clipboard = curve;
					s_HasClipboard = true;
				}
				if (ImGui::MenuItem("Paste values", nullptr, false, s_HasClipboard)) {
					curve = s_Clipboard;
					s_Selected = -1;
					s_CtxKey = -1;
					changed = true;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset to defaults")) {
					curve.Keys = Curve::DefaultKeys();
					s_Selected = -1;
					s_CtxKey = -1;
					changed = true;
				}
				const bool canDelete = s_CtxKey > 0 && s_CtxKey < static_cast<int>(keys.size()) - 1;
				if (ImGui::MenuItem("Delete point", nullptr, false, canDelete)) {
					keys.erase(keys.begin() + s_CtxKey);
					s_Selected = -1;
					s_CtxKey = -1;
					changed = true;
				}
				ImGui::EndPopup();
			}
		}

		// Tangent handles for the selected key (drawn only while enabled).
		if (enabled && s_Selected >= 0 && s_Selected < static_cast<int>(keys.size())) {
			const Key& k = keys[s_Selected];
			const ImVec2 kp = toScreen(k.Pos);
			if (s_Selected > 0) {
				const ImVec2 hp = handleDot(kp, k.InTangent);
				dl->AddLine(kp, hp, col(255, 200, 120, 200), 1.5f);
				dl->AddCircleFilled(hp, kTanR, col(255, 200, 120, 255));
			}
			if (s_Selected < static_cast<int>(keys.size()) - 1) {
				const ImVec2 hp = handleDot(kp, k.OutTangent);
				dl->AddLine(kp, hp, col(255, 200, 120, 200), 1.5f);
				dl->AddCircleFilled(hp, kTanR, col(255, 200, 120, 255));
			}
		}

		// Key points.
		for (std::size_t i = 0; i < keys.size(); ++i) {
			const ImVec2 sp = toScreen(keys[i].Pos);
			const bool sel = enabled && (static_cast<int>(i) == s_Selected);
			dl->AddCircleFilled(sp, kKeyR, sel ? col(255, 230, 120, 255) : col(120, 200, 255, 255));
		}

		ImGui::PopID();
		return changed;
	}

	float GetInspectorLabelColumnWidth()
	{
		return 160.0f;
	}

	void DrawInspectorLabel(const char* label)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float labelColumnWidth = GetInspectorLabelColumnWidth();
		const float availableLabelWidth = std::max(1.0f, labelColumnWidth - style.ItemSpacing.x);

		ImGui::AlignTextToFramePadding();

		bool truncated = false;
		const std::string clippedLabel = Ellipsize(label ? label : "", availableLabelWidth, &truncated);
		ImGui::TextUnformatted(clippedLabel.c_str());
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", label);
		}
	}

	void BeginInspectorFieldRow(const char* label)
	{
		DrawInspectorLabel(label);
		ImGui::SameLine(GetInspectorLabelColumnWidth());
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	}

	std::string Ellipsize(const std::string& text, float maxWidth, bool* outTruncated)
	{
		if (outTruncated) {
			*outTruncated = false;
		}

		if (text.empty() || maxWidth <= 0.0f) {
			return text;
		}

		if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) {
			return text;
		}

		constexpr const char* ellipsis = "...";
		const float ellipsisWidth = ImGui::CalcTextSize(ellipsis).x;
		if (ellipsisWidth >= maxWidth) {
			if (outTruncated) {
				*outTruncated = true;
			}
			return ellipsis;
		}

		const float availableWidth = maxWidth - ellipsisWidth;
		int low = 0;
		int high = static_cast<int>(text.size());
		int bestFit = 0;

		while (low <= high) {
			const int mid = low + ((high - low) / 2);
			const float currentWidth = ImGui::CalcTextSize(text.c_str(), text.c_str() + mid).x;
			if (currentWidth <= availableWidth) {
				bestFit = mid;
				low = mid + 1;
			}
			else {
				high = mid - 1;
			}
		}

		if (outTruncated) {
			*outTruncated = true;
		}

		return text.substr(0, static_cast<std::size_t>(bestFit)) + ellipsis;
	}

	void TextEllipsis(const std::string& text, float maxWidth)
	{
		if (maxWidth < 0.0f) {
			maxWidth = ImGui::GetContentRegionAvail().x;
		}

		bool truncated = false;
		const std::string displayText = Ellipsize(text, maxWidth, &truncated);
		ImGui::TextUnformatted(displayText.c_str());
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", text.c_str());
		}
	}

	void TextDisabledEllipsis(const std::string& text, float maxWidth)
	{
		if (maxWidth < 0.0f) {
			maxWidth = ImGui::GetContentRegionAvail().x;
		}

		bool truncated = false;
		const std::string displayText = Ellipsize(text, maxWidth, &truncated);
		ImGui::TextDisabled("%s", displayText.c_str());
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", text.c_str());
		}
	}

	bool SelectableEllipsis(const std::string& text, const char* id, bool selected,
		ImGuiSelectableFlags flags, const ImVec2& size, float maxWidth)
	{
		if (maxWidth < 0.0f) {
			maxWidth = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
		}

		bool truncated = false;
		const std::string displayText = Ellipsize(text, maxWidth, &truncated);
		const std::string label = displayText + "##" + (id ? std::string(id) : text);
		const bool activated = ImGui::Selectable(label.c_str(), selected, flags, size);
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", text.c_str());
		}
		return activated;
	}

	bool MenuItemEllipsis(const std::string& text, const char* id,
		const char* shortcut, bool selected, bool enabled, float maxWidth)
	{
		if (maxWidth < 0.0f) {
			maxWidth = ImGui::GetContentRegionAvail().x;
		}

		bool truncated = false;
		const std::string displayText = Ellipsize(text, maxWidth, &truncated);
		const std::string label = displayText + "##" + (id ? std::string(id) : text);
		const bool activated = ImGui::MenuItem(label.c_str(), shortcut, selected, enabled);
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", text.c_str());
		}
		return activated;
	}

	namespace {
		// Dark rounded panel + transparency checkerboard — shared by the texture
		// preview and the empty "No Texture" placeholder so both read identically.
		void DrawPreviewBackground(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
		{
			drawList->AddRectFilled(min, max, IM_COL32(35, 35, 35, 255), 6.0f);

			const float checkerSize = 8.0f;
			for (float y = min.y; y < max.y; y += checkerSize) {
				for (float x = min.x; x < max.x; x += checkerSize) {
					const int ix = static_cast<int>((x - min.x) / checkerSize);
					const int iy = static_cast<int>((y - min.y) / checkerSize);
					const bool even = ((ix + iy) % 2) == 0;

					drawList->AddRectFilled(
						ImVec2(x, y),
						ImVec2(
							(x + checkerSize < max.x) ? x + checkerSize : max.x,
							(y + checkerSize < max.y) ? y + checkerSize : max.y
						),
						even ? IM_COL32(70, 70, 70, 255) : IM_COL32(100, 100, 100, 255)
					);
				}
			}
		}

		// Square 1.5px frame around the preview box. Square (not rounded) so it
		// traces the checkerboard's sharp corners exactly. Drawn on top of the
		// content so it frames the placeholder and an assigned texture alike.
		void DrawPreviewBorder(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
		{
			drawList->AddRect(min, max, IM_COL32(130, 130, 130, 220), 0.0f, 0, 1.5f);
		}

		void DrawTexturePreviewImpl(uint64_t rendererId, float texWidth, float texHeight,
			float previewSize, bool flippedY)
		{
			const ImVec2 previewMin = ImGui::GetCursorScreenPos();
			const ImVec2 previewMax = ImVec2(previewMin.x + previewSize, previewMin.y + previewSize);

			ImDrawList* drawList = ImGui::GetWindowDrawList();

			DrawPreviewBackground(drawList, previewMin, previewMax);

			float drawWidth = previewSize;
			float drawHeight = previewSize;

			if (texWidth > 0.0f && texHeight > 0.0f) {
				const float aspect = texWidth / texHeight;
				if (aspect > 1.0f) {
					drawHeight = previewSize / aspect;
				}
				else {
					drawWidth = previewSize * aspect;
				}
			}

			const ImVec2 imageMin = ImVec2(
				previewMin.x + (previewSize - drawWidth) * 0.5f,
				previewMin.y + (previewSize - drawHeight) * 0.5f
			);
			const ImVec2 imageMax = ImVec2(imageMin.x + drawWidth, imageMin.y + drawHeight);

			// stb uploads bottom-row-first when flipVertical=true; compensate with (0,1)→(1,0) UVs so the preview isn't upside-down.
			const ImVec2 uv0 = flippedY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
			const ImVec2 uv1 = flippedY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
			drawList->AddImage((ImTextureID)(intptr_t)rendererId,
				imageMin, imageMax, uv0, uv1);

			DrawPreviewBorder(drawList, previewMin, previewMax);

			ImGui::Dummy(ImVec2(previewSize, previewSize));
		}
	}

	void DrawTexturePreview(uint64_t rendererId, float texWidth, float texHeight, float previewSize)
	{
		// Raw-handle overload: assumes the natural top-down sprite/UI load
		// path. New call sites should prefer the Texture2D overload below
		// so the canonical flip rule applies automatically.
		DrawTexturePreviewImpl(rendererId, texWidth, texHeight, previewSize, /*flippedY=*/false);
	}

	void DrawTexturePreview(const Texture2D& tex, float previewSize)
	{
		DrawTexturePreviewImpl(tex.GetHandle(), tex.GetWidth(), tex.GetHeight(),
			previewSize, tex.IsFlippedY());
	}

	void DrawTexturePlaceholder(float previewSize)
	{
		const ImVec2 previewMin = ImGui::GetCursorScreenPos();
		const ImVec2 previewMax = ImVec2(previewMin.x + previewSize, previewMin.y + previewSize);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		DrawPreviewBackground(drawList, previewMin, previewMax);
		DrawPreviewBorder(drawList, previewMin, previewMax);

		const char* label = "No Texture";
		const ImVec2 textSize = ImGui::CalcTextSize(label);
		const ImVec2 textPos(
			previewMin.x + (previewSize - textSize.x) * 0.5f,
			previewMin.y + (previewSize - textSize.y) * 0.5f
		);
		// Shadow first so the label stays legible over the checker pattern.
		drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 200), label);
		drawList->AddText(textPos, IM_COL32(225, 225, 225, 255), label);

		ImGui::Dummy(ImVec2(previewSize, previewSize));
	}

	void CenterNextModal() {
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		// ImGuiCond_Always, not Appearing: every caller pairs this with SetNextWindowAsNativeDialog(),
		// and multi-viewport + NoAutoMerge makes the Appearing-cond Pos hint unreliable, pinning the
		// dialog off-center on first show.
		ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	}

	bool BeginComponentSection(const char* label, bool& removeRequested, const std::function<void()>& contextMenu)
	{
		removeRequested = false;

		ImGui::PushID(label);

		bool truncated = false;
		const float headerWidth = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2.0f;
		const std::string displayLabel = Ellipsize(label, headerWidth, &truncated);
		const std::string headerLabel = displayLabel + "##" + label;
		bool open = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (truncated && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", label);
		}

		if (ImGui::BeginPopupContextItem("ComponentContext")) {
			if (contextMenu) {
				contextMenu();
				ImGui::Separator();
			}
			if (ImGui::MenuItem("Remove Component")) {
				removeRequested = true;
			}
			ImGui::EndPopup();
		}

		if (open) {
			ImGui::Indent(8.0f);
		}
		else {
			ImGui::PopID();
		}

		return open;
	}

	void EndComponentSection()
	{
		ImGui::Unindent(8.0f);
		ImGui::Spacing();
		ImGui::PopID();
	}
}
