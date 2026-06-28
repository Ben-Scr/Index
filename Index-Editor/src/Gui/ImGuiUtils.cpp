#include <pch.hpp>
#include "Gui/ImGuiUtils.hpp"
#include "Graphics/Texture2D.hpp"
#include "Scene/Scene.hpp"
#include "Collections/Curve.hpp"
#include "Collections/Gradient.hpp"
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
		constexpr float kSnap = 0.1f;   // Left-Ctrl drag-snap increment
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

		// Tangent handles draw at a fixed, short screen length along the tangent direction.
		// They are NOT clamped to the bounds — a handle runs its full length in its true
		// direction and the graph square (p0..p1) just clips the drawing, acting as a mask.
		// Drag sets the tangent's angle; the handle keeps this constant compact length.
		const float kHandleLen = 28.0f;
		auto handleDot = [&](const ImVec2& keyS, const Vec2& off) -> ImVec2 {
			float dx = off.x * size.x;
			float dy = -off.y / (kYMax - kYMin) * size.y;
			float len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-4f) { dx = (off.x < 0.0f) ? -1.0f : 1.0f; dy = 0.0f; len = 1.0f; }
			return ImVec2(keyS.x + dx / len * kHandleLen, keyS.y + dy / len * kHandleLen);
		};
		// The same handle ray, but stopped where it exits the square. Used for hit-testing so
		// a handle stays grabbable at its visible edge even when its dot is masked off-graph.
		auto handleGrab = [&](const ImVec2& keyS, const Vec2& off) -> ImVec2 {
			const ImVec2 full = handleDot(keyS, off);
			const float dx = full.x - keyS.x, dy = full.y - keyS.y;
			float t = 1.0f;
			if (dx >  1e-4f)      t = std::min(t, (p1.x - keyS.x) / dx);
			else if (dx < -1e-4f) t = std::min(t, (p0.x - keyS.x) / dx);
			if (dy >  1e-4f)      t = std::min(t, (p1.y - keyS.y) / dy);
			else if (dy < -1e-4f) t = std::min(t, (p0.y - keyS.y) / dy);
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
		// Curve-space position the context menu was opened at, so "Add Point" can
		// insert where the user right-clicked rather than where the menu item sits.
		static Vec2 s_CtxPos{ 0.5f, 1.0f };
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

			// Insert a key at curve-x `mx` WITHOUT changing the curve shape: find the bezier
			// segment spanning mx, solve t for x(t)=mx, then De Casteljau-split it so the new
			// key lands on the curve and the rewritten neighbour handles reproduce the original
			// two halves exactly. The user only alters the shape by dragging afterwards.
			// Returns the new key's index, or -1 if mx isn't strictly between the endpoints.
			auto insertKeyPreservingShape = [&](float mx) -> int {
				if (keys.size() < 2) return -1;
				if (mx <= keys.front().Pos.x + 1e-4f || mx >= keys.back().Pos.x - 1e-4f) return -1;
				// Segment [i-1, i] with keys[i-1].Pos.x < mx <= keys[i].Pos.x.
				std::size_t i = 1;
				while (i < keys.size() - 1 && keys[i].Pos.x < mx) ++i;
				Key& a = keys[i - 1];
				Key& b = keys[i];
				const Vec2 p0 = a.Pos;
				const Vec2 p1{ a.Pos.x + a.OutTangent.x, a.Pos.y + a.OutTangent.y };
				const Vec2 p2{ b.Pos.x + b.InTangent.x,  b.Pos.y + b.InTangent.y };
				const Vec2 p3 = b.Pos;
				// x is monotonic in t (handle x is clamped on drag): bisect for t(mx).
				float lo = 0.0f, hi = 1.0f;
				for (int iter = 0; iter < 24; ++iter) {
					const float mid = 0.5f * (lo + hi);
					if (Curve::BezierPoint(p0, p1, p2, p3, mid).x < mx) lo = mid; else hi = mid;
				}
				const float t = 0.5f * (lo + hi);
				auto lerp2 = [](const Vec2& u, const Vec2& v, float s) {
					return Vec2{ u.x + (v.x - u.x) * s, u.y + (v.y - u.y) * s };
				};
				const Vec2 A = lerp2(p0, p1, t);
				const Vec2 B = lerp2(p1, p2, t);
				const Vec2 C = lerp2(p2, p3, t);
				const Vec2 D = lerp2(A, B, t);
				const Vec2 E = lerp2(B, C, t);
				const Vec2 F = lerp2(D, E, t);   // split point — lies exactly on the curve
				// Rewrite handles so the two sub-segments equal the original halves.
				a.OutTangent = Vec2{ A.x - p0.x, A.y - p0.y };
				b.InTangent  = Vec2{ C.x - p3.x, C.y - p3.y };
				Key nk;
				nk.Pos        = F;
				nk.InTangent  = Vec2{ D.x - F.x, D.y - F.y };
				nk.OutTangent = Vec2{ E.x - F.x, E.y - F.y };
				keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(i), nk);
				return static_cast<int>(i);
			};

			int hitKey = -1, hitPart = -1;
			if (s_Selected >= 0 && s_Selected < static_cast<int>(keys.size())) {
				const Key& k = keys[s_Selected];
				const ImVec2 keyS = toScreen(k.Pos);
				if (s_Selected > 0 && (withinR(handleDot(keyS, k.InTangent), kTanR) || withinR(handleGrab(keyS, k.InTangent), kTanR))) { hitKey = s_Selected; hitPart = 1; }
				else if (s_Selected < static_cast<int>(keys.size()) - 1 && (withinR(handleDot(keyS, k.OutTangent), kTanR) || withinR(handleGrab(keyS, k.OutTangent), kTanR))) { hitKey = s_Selected; hitPart = 2; }
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
					// Build a tangent offset for the given side from a unit direction: magnitude
					// scales with that side's segment, x clamped to the side's half-plane.
					auto makeTangent = [&](float ux, float uy, bool in) -> Vec2 {
						const float segW = in
							? (s_DragKey > 0 ? k.Pos.x - keys[s_DragKey - 1].Pos.x : 0.5f)
							: (s_DragKey < static_cast<int>(keys.size()) - 1 ? keys[s_DragKey + 1].Pos.x - k.Pos.x : 0.5f);
						const float mag = 0.45f * std::max(0.05f, segW);
						Vec2 off{ ux * mag, uy * mag };
						off.x = in ? std::clamp(off.x, -segW, 0.0f) : std::clamp(off.x, 0.0f, segW);
						return off;
					};
					// Dragged handle follows the mouse (snapped under Ctrl).
					Vec2 dragged = makeTangent(dx, dy, isIn);
					if (ctrlSnap) { dragged.x = snap(dragged.x); dragged.y = snap(dragged.y); }
					// Opposite handle stays collinear (antiparallel) so the tangent line through
					// the key remains smooth/aligned — move one side, the other mirrors it.
					float ndx = dragged.x, ndy = dragged.y;
					const float nl = std::sqrt(ndx * ndx + ndy * ndy);
					if (nl > 1e-4f) { ndx /= nl; ndy /= nl; }
					else { ndx = isIn ? -1.0f : 1.0f; ndy = 0.0f; }
					const Vec2 mirrored = makeTangent(-ndx, -ndy, !isIn);
					if (isIn) { k.InTangent = dragged; k.OutTangent = mirrored; }
					else      { k.OutTangent = dragged; k.InTangent = mirrored; }
					changed = true;
				}
			}

			if (hovered && hitPart < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				Vec2 m = fromScreen(mouse);
				if (ctrlSnap) m.x = snap(m.x);   // y comes from the curve so the shape is preserved
				const int newIdx = insertKeyPreservingShape(m.x);
				if (newIdx >= 0) {
					s_Selected = newIdx;
					changed = true;
				}
			}
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
				s_CtxKey = (hitPart == 0) ? hitKey : -1;
				if (hitPart == 0) s_Selected = hitKey;
				Vec2 ctx = fromScreen(mouse);
				if (ctrlSnap) { ctx.x = snap(ctx.x); ctx.y = snap(ctx.y); }
				s_CtxPos = ctx;
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
				ImGui::Separator();
				// Endpoints are pinned at x=0/1; only the open interval can take a new point.
				const bool canAddPoint = s_CtxPos.x > 1e-3f && s_CtxPos.x < 1.0f - 1e-3f;
				if (ImGui::MenuItem("Add Point", nullptr, false, canAddPoint)) {
					const int newIdx = insertKeyPreservingShape(s_CtxPos.x);
					if (newIdx >= 0) s_Selected = newIdx;
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

		// Tangent handles for the selected key (drawn only while enabled). Clipped to the
		// graph square so a handle that runs past an edge is masked off rather than clamped.
		if (enabled && s_Selected >= 0 && s_Selected < static_cast<int>(keys.size())) {
			const Key& k = keys[s_Selected];
			const ImVec2 kp = toScreen(k.Pos);
			dl->PushClipRect(p0, p1, true);
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
			dl->PopClipRect();
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

	bool DrawGradientEditor(const char* id, Gradient& gradient, bool enabled)
	{
		using ColorKey = Gradient::ColorKey;
		using AlphaKey = Gradient::AlphaKey;
		auto& colorKeys = gradient.ColorKeys;
		auto& alphaKeys = gradient.AlphaKeys;
		if (colorKeys.empty()) colorKeys = Gradient::DefaultColorKeys();
		if (alphaKeys.empty()) alphaKeys = Gradient::DefaultAlphaKeys();

		constexpr float kStripH = 14.0f;     // marker strips above (alpha) + below (colour) the bar
		constexpr float kBarH = 26.0f;       // gradient preview bar
		constexpr float kMarkerHalf = 6.0f;  // marker triangle half-width
		constexpr float kHitR = 8.0f;        // marker grab radius (x)

		ImGui::PushID(id);
		bool changed = false;

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float fullW = std::max(120.0f, ImGui::GetContentRegionAvail().x);
		const float totalH = kStripH + kBarH + kStripH;
		ImGui::InvisibleButton("##gradient", ImVec2(fullW, totalH),
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = enabled && ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();

		const float barX0 = origin.x;
		const float barX1 = origin.x + fullW;
		const float barY0 = origin.y + kStripH;
		const float barY1 = barY0 + kBarH;
		const float barW = barX1 - barX0;

		// Dim every drawn element when disabled so the widget reads as inactive (greyed).
		auto col = [&](int r, int g, int b, int a) -> ImU32 {
			return IM_COL32(r, g, b, enabled ? a : a * 35 / 100);
		};
		auto tToX = [&](float t) { return barX0 + std::clamp(t, 0.0f, 1.0f) * barW; };
		auto xToT = [&](float x) { return std::clamp((x - barX0) / std::max(1.0f, barW), 0.0f, 1.0f); };
		auto u8 = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };

		// Checkerboard behind the bar so partial alpha is visible.
		{
			constexpr float kCell = 6.0f;
			dl->PushClipRect(ImVec2(barX0, barY0), ImVec2(barX1, barY1), true);
			int row = 0;
			for (float y = barY0; y < barY1; y += kCell, ++row) {
				int cell = row;
				for (float x = barX0; x < barX1; x += kCell, ++cell) {
					const ImU32 c = (cell & 1) ? col(120, 120, 120, 255) : col(80, 80, 80, 255);
					dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + kCell, barX1), std::min(y + kCell, barY1)), c);
				}
			}
			dl->PopClipRect();
		}

		// Gradient fill: draw a horizontal multi-colour rect between each pair of adjacent
		// breakpoints (the union of colour + alpha key positions). Between two breakpoints
		// neither RGB nor alpha has a key, so both interpolate linearly — exactly what
		// AddRectFilledMultiColor produces.
		{
			const int dimA = enabled ? 255 : 89;
			auto sampleU32 = [&](float t) -> ImU32 {
				const Color c = gradient.Evaluate(t);
				const int a = static_cast<int>(std::lround(std::clamp(c.a, 0.0f, 1.0f) * dimA));
				return IM_COL32(u8(c.r), u8(c.g), u8(c.b), a);
			};
			std::vector<float> stops;
			stops.reserve(colorKeys.size() + alphaKeys.size() + 2);
			stops.push_back(0.0f);
			stops.push_back(1.0f);
			for (const auto& k : colorKeys) stops.push_back(std::clamp(k.Position, 0.0f, 1.0f));
			for (const auto& k : alphaKeys) stops.push_back(std::clamp(k.Position, 0.0f, 1.0f));
			std::sort(stops.begin(), stops.end());
			stops.erase(std::unique(stops.begin(), stops.end(),
				[](float a, float b) { return std::abs(a - b) < 1e-5f; }), stops.end());
			for (std::size_t i = 1; i < stops.size(); ++i) {
				const float tL = stops[i - 1], tR = stops[i];
				const ImU32 cL = sampleU32(tL), cR = sampleU32(tR);
				dl->AddRectFilledMultiColor(ImVec2(tToX(tL), barY0), ImVec2(tToX(tR), barY1), cL, cR, cR, cL);
			}
			dl->AddRect(ImVec2(barX0, barY0), ImVec2(barX1, barY1), col(70, 70, 78, 255));
		}

		// Persistent selection + drag target, keyed to this gradient's address so it resets
		// when a different gradient is shown. List: 0 = colour key, 1 = alpha key.
		static const void* s_Owner = nullptr;
		static int s_SelList = -1, s_SelIdx = -1;
		static int s_DragList = -1, s_DragIdx = -1;
		// t at which the context menu was opened, so "Add ... Stop" inserts where the user
		// right-clicked rather than wherever the menu item happens to sit.
		static float s_CtxT = 0.5f;
		if (s_Owner != static_cast<const void*>(&gradient)) {
			s_Owner = static_cast<const void*>(&gradient);
			s_SelList = s_SelIdx = s_DragList = s_DragIdx = -1;
		}
		if (s_SelList == 0 && s_SelIdx >= static_cast<int>(colorKeys.size())) { s_SelList = -1; s_SelIdx = -1; }
		if (s_SelList == 1 && s_SelIdx >= static_cast<int>(alphaKeys.size())) { s_SelList = -1; s_SelIdx = -1; }

		const ImVec2 mouse = ImGui::GetMousePos();
		const float colTipY = barY1;            // colour markers: tip at bar bottom, base below
		const float colBaseY = barY1 + kStripH;
		const float alphaTipY = barY0;          // alpha markers: tip at bar top, base above
		const float alphaBaseY = barY0 - kStripH;

		auto hitColorMarker = [&](int i) {
			const float x = tToX(colorKeys[i].Position);
			return std::abs(mouse.x - x) <= kHitR && mouse.y >= colTipY - 2.0f && mouse.y <= colBaseY + 2.0f;
		};
		auto hitAlphaMarker = [&](int i) {
			const float x = tToX(alphaKeys[i].Position);
			return std::abs(mouse.x - x) <= kHitR && mouse.y >= alphaBaseY - 2.0f && mouse.y <= alphaTipY + 2.0f;
		};
		auto addColorKey = [&](float t) {
			const Color c = gradient.EvaluateRGB(t);
			ColorKey nk; nk.Position = std::clamp(t, 0.0f, 1.0f); nk.R = c.r; nk.G = c.g; nk.B = c.b;
			int idx = 0;
			while (idx < static_cast<int>(colorKeys.size()) && colorKeys[idx].Position < nk.Position) ++idx;
			colorKeys.insert(colorKeys.begin() + idx, nk);
			s_SelList = 0; s_SelIdx = idx;
		};
		auto addAlphaKey = [&](float t) {
			AlphaKey nk; nk.Position = std::clamp(t, 0.0f, 1.0f); nk.Alpha = gradient.EvaluateAlpha(t);
			int idx = 0;
			while (idx < static_cast<int>(alphaKeys.size()) && alphaKeys[idx].Position < nk.Position) ++idx;
			alphaKeys.insert(alphaKeys.begin() + idx, nk);
			s_SelList = 1; s_SelIdx = idx;
		};

		if (enabled) {
			// Resolve the marker under the cursor (prefer the selected one for stable grabbing).
			int hitList = -1, hitIdx = -1;
			if (s_SelList == 1 && s_SelIdx >= 0 && s_SelIdx < static_cast<int>(alphaKeys.size()) && hitAlphaMarker(s_SelIdx)) { hitList = 1; hitIdx = s_SelIdx; }
			else if (s_SelList == 0 && s_SelIdx >= 0 && s_SelIdx < static_cast<int>(colorKeys.size()) && hitColorMarker(s_SelIdx)) { hitList = 0; hitIdx = s_SelIdx; }
			if (hitList < 0) {
				for (int i = 0; i < static_cast<int>(alphaKeys.size()); ++i) { if (hitAlphaMarker(i)) { hitList = 1; hitIdx = i; break; } }
			}
			if (hitList < 0) {
				for (int i = 0; i < static_cast<int>(colorKeys.size()); ++i) { if (hitColorMarker(i)) { hitList = 0; hitIdx = i; break; } }
			}

			if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hitList >= 0) {
				s_SelList = hitList; s_SelIdx = hitIdx;
				s_DragList = hitList; s_DragIdx = hitIdx;
			}
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { s_DragList = -1; s_DragIdx = -1; }

			// Drag the grabbed marker along x, clamped between its neighbours so the list stays
			// sorted and the index stays stable.
			if (s_DragList == 0 && s_DragIdx >= 0 && s_DragIdx < static_cast<int>(colorKeys.size())
				&& ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
				float t = xToT(mouse.x);
				const float lo = s_DragIdx > 0 ? colorKeys[s_DragIdx - 1].Position + 1e-4f : 0.0f;
				const float hi = s_DragIdx < static_cast<int>(colorKeys.size()) - 1 ? colorKeys[s_DragIdx + 1].Position - 1e-4f : 1.0f;
				t = std::clamp(t, std::min(lo, hi), std::max(lo, hi));
				if (t != colorKeys[s_DragIdx].Position) { colorKeys[s_DragIdx].Position = t; changed = true; }
			}
			else if (s_DragList == 1 && s_DragIdx >= 0 && s_DragIdx < static_cast<int>(alphaKeys.size())
				&& ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
				float t = xToT(mouse.x);
				const float lo = s_DragIdx > 0 ? alphaKeys[s_DragIdx - 1].Position + 1e-4f : 0.0f;
				const float hi = s_DragIdx < static_cast<int>(alphaKeys.size()) - 1 ? alphaKeys[s_DragIdx + 1].Position - 1e-4f : 1.0f;
				t = std::clamp(t, std::min(lo, hi), std::max(lo, hi));
				if (t != alphaKeys[s_DragIdx].Position) { alphaKeys[s_DragIdx].Position = t; changed = true; }
			}

			// Double-click an empty strip to add a stop: below the bar => colour, above => alpha.
			if (hovered && hitList < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				const float t = xToT(mouse.x);
				if (mouse.y >= barY1) { addColorKey(t); changed = true; }
				else if (mouse.y <= barY0) { addAlphaKey(t); changed = true; }
				else { addColorKey(t); changed = true; } // double-click on the bar => colour stop
			}

			// Right-click: remember the cursor's t for "Add ... Stop", and select the marker
			// under the cursor (if any) so "Delete stop" targets it.
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
				s_CtxT = xToT(mouse.x);
				if (hitList >= 0) { s_SelList = hitList; s_SelIdx = hitIdx; }
			}
			// Delete the selected stop (keep at least one per list). Suppressed while a
			// text/drag field is being edited so Backspace there doesn't erase a stop.
			const bool deletePressed = hovered && !ImGui::GetIO().WantTextInput
				&& (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace));
			if (deletePressed && s_SelList == 0 && colorKeys.size() > 1) {
				colorKeys.erase(colorKeys.begin() + s_SelIdx); s_SelList = -1; s_SelIdx = -1; changed = true;
			}
			else if (deletePressed && s_SelList == 1 && alphaKeys.size() > 1) {
				alphaKeys.erase(alphaKeys.begin() + s_SelIdx); s_SelList = -1; s_SelIdx = -1; changed = true;
			}

			if (ImGui::BeginPopupContextItem("##gradCtx")) {
				if (ImGui::MenuItem("Add Color Stop")) {
					addColorKey(s_CtxT); changed = true;
				}
				if (ImGui::MenuItem("Add Alpha Stop")) {
					addAlphaKey(s_CtxT); changed = true;
				}
				ImGui::Separator();
				const bool canDelete = (s_SelList == 0 && colorKeys.size() > 1) || (s_SelList == 1 && alphaKeys.size() > 1);
				if (ImGui::MenuItem("Delete stop", nullptr, false, canDelete)) {
					if (s_SelList == 0) colorKeys.erase(colorKeys.begin() + s_SelIdx);
					else if (s_SelList == 1) alphaKeys.erase(alphaKeys.begin() + s_SelIdx);
					s_SelList = -1; s_SelIdx = -1; changed = true;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset to default")) {
					colorKeys = Gradient::DefaultColorKeys();
					alphaKeys = Gradient::DefaultAlphaKeys();
					s_SelList = -1; s_SelIdx = -1; changed = true;
				}
				ImGui::EndPopup();
			}
		}

		// Alpha markers (top), then colour markers (bottom), so the selected one reads clearly.
		for (int i = 0; i < static_cast<int>(alphaKeys.size()); ++i) {
			const float x = tToX(alphaKeys[i].Position);
			const bool sel = enabled && s_SelList == 1 && i == s_SelIdx;
			const int g = u8(alphaKeys[i].Alpha);
			dl->AddTriangleFilled(ImVec2(x, alphaTipY), ImVec2(x - kMarkerHalf, alphaBaseY), ImVec2(x + kMarkerHalf, alphaBaseY), col(g, g, g, 255));
			dl->AddTriangle(ImVec2(x, alphaTipY), ImVec2(x - kMarkerHalf, alphaBaseY), ImVec2(x + kMarkerHalf, alphaBaseY),
				sel ? col(255, 230, 120, 255) : col(20, 20, 20, 255), sel ? 2.0f : 1.0f);
		}
		for (int i = 0; i < static_cast<int>(colorKeys.size()); ++i) {
			const float x = tToX(colorKeys[i].Position);
			const bool sel = enabled && s_SelList == 0 && i == s_SelIdx;
			const ColorKey& k = colorKeys[i];
			dl->AddTriangleFilled(ImVec2(x, colTipY), ImVec2(x - kMarkerHalf, colBaseY), ImVec2(x + kMarkerHalf, colBaseY), col(u8(k.R), u8(k.G), u8(k.B), 255));
			dl->AddTriangle(ImVec2(x, colTipY), ImVec2(x - kMarkerHalf, colBaseY), ImVec2(x + kMarkerHalf, colBaseY),
				sel ? col(255, 230, 120, 255) : col(20, 20, 20, 255), sel ? 2.0f : 1.0f);
		}

		// Editor row for the selected stop (greyed-out with the widget when disabled).
		auto dragLocation = [&](float& position, int idx, const std::vector<float>& neighbourPositions) {
			float locPct = position * 100.0f;
			ImGui::SetNextItemWidth(110.0f);
			if (ImGui::DragFloat("##gradLoc", &locPct, 0.5f, 0.0f, 100.0f, "Loc %.0f%%")) {
				float t = std::clamp(locPct / 100.0f, 0.0f, 1.0f);
				const float lo = idx > 0 ? neighbourPositions[idx - 1] + 1e-4f : 0.0f;
				const float hi = idx < static_cast<int>(neighbourPositions.size()) - 1 ? neighbourPositions[idx + 1] - 1e-4f : 1.0f;
				t = std::clamp(t, std::min(lo, hi), std::max(lo, hi));
				position = t; changed = true;
			}
		};
		ImGui::BeginDisabled(!enabled);
		if (s_SelList == 0 && s_SelIdx >= 0 && s_SelIdx < static_cast<int>(colorKeys.size())) {
			ColorKey& k = colorKeys[s_SelIdx];
			float rgb[3] = { k.R, k.G, k.B };
			if (ImGui::ColorEdit3("##gradColorEdit", rgb, ImGuiColorEditFlags_NoInputs)) { k.R = rgb[0]; k.G = rgb[1]; k.B = rgb[2]; changed = true; }
			ImGui::SameLine();
			std::vector<float> positions; positions.reserve(colorKeys.size());
			for (const auto& c : colorKeys) positions.push_back(c.Position);
			dragLocation(k.Position, s_SelIdx, positions);
			ImGui::SameLine();
			ImGui::BeginDisabled(colorKeys.size() <= 1);
			if (ImGui::Button("Delete##gradDelColor")) { colorKeys.erase(colorKeys.begin() + s_SelIdx); s_SelList = -1; s_SelIdx = -1; changed = true; }
			ImGui::EndDisabled();
		}
		else if (s_SelList == 1 && s_SelIdx >= 0 && s_SelIdx < static_cast<int>(alphaKeys.size())) {
			AlphaKey& k = alphaKeys[s_SelIdx];
			float a = k.Alpha;
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::SliderFloat("##gradAlphaEdit", &a, 0.0f, 1.0f, "A %.2f")) { k.Alpha = std::clamp(a, 0.0f, 1.0f); changed = true; }
			ImGui::SameLine();
			std::vector<float> positions; positions.reserve(alphaKeys.size());
			for (const auto& c : alphaKeys) positions.push_back(c.Position);
			dragLocation(k.Position, s_SelIdx, positions);
			ImGui::SameLine();
			ImGui::BeginDisabled(alphaKeys.size() <= 1);
			if (ImGui::Button("Delete##gradDelAlpha")) { alphaKeys.erase(alphaKeys.begin() + s_SelIdx); s_SelList = -1; s_SelIdx = -1; changed = true; }
			ImGui::EndDisabled();
		}

		ImGui::EndDisabled();

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

		// The WGPU ImGui backend binds one shared sampler (Linear by default) for
		// every image; per-texture samplers are ignored. These emit the backend's
		// platform_io sampler callbacks to switch group-0 sampling for the next
		// draw, so a Point texture can render crisp. Push must be paired with Pop
		// (restore Linear) — the state persists across draws until reset.
		void PushPointSampler(ImDrawList* drawList)
		{
			if (ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest)
				drawList->AddCallback(cb, nullptr);
		}
		void PopPointSampler(ImDrawList* drawList)
		{
			if (ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear)
				drawList->AddCallback(cb, nullptr);
		}

		void DrawTexturePreviewImpl(uint64_t rendererId, float texWidth, float texHeight,
			float previewSize, bool flippedY, Filter filter)
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
			const bool point = (filter == Filter::Point);
			if (point) PushPointSampler(drawList);
			drawList->AddImage((ImTextureID)(intptr_t)rendererId,
				imageMin, imageMax, uv0, uv1);
			if (point) PopPointSampler(drawList);

			DrawPreviewBorder(drawList, previewMin, previewMax);

			ImGui::Dummy(ImVec2(previewSize, previewSize));
		}
	}

	void DrawTexturePreview(uint64_t rendererId, float texWidth, float texHeight, float previewSize, Filter filter)
	{
		// Raw-handle overload: assumes the natural top-down sprite/UI load
		// path. New call sites should prefer the Texture2D overload below
		// so the canonical flip rule applies automatically.
		DrawTexturePreviewImpl(rendererId, texWidth, texHeight, previewSize, /*flippedY=*/false, filter);
	}

	void DrawTexturePreview(const Texture2D& tex, float previewSize, std::optional<Filter> filterOverride)
	{
		const Filter filter = filterOverride.value_or(tex.GetFilter());
		DrawTexturePreviewImpl(tex.GetHandle(), tex.GetWidth(), tex.GetHeight(),
			previewSize, tex.IsFlippedY(), filter);
	}

	void ImageFiltered(ImTextureID texId, const ImVec2& size, Filter filter,
		const ImVec2& uv0, const ImVec2& uv1)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const bool point = (filter == Filter::Point);
		if (point) PushPointSampler(drawList);
		ImGui::Image(texId, size, uv0, uv1);
		if (point) PopPointSampler(drawList);
	}

	void AddImageFiltered(ImDrawList* drawList, ImTextureID texId,
		const ImVec2& pMin, const ImVec2& pMax, Filter filter,
		const ImVec2& uv0, const ImVec2& uv1, ImU32 tintCol)
	{
		const bool point = (filter == Filter::Point);
		if (point) PushPointSampler(drawList);
		drawList->AddImage(texId, pMin, pMax, uv0, uv1, tintCol);
		if (point) PopPointSampler(drawList);
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
