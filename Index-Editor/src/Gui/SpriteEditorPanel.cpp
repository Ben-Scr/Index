#include <pch.hpp>
#include "Gui/SpriteEditorPanel.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Graphics/SpriteUVResolver.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"

#include "Graphics/ImageData.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <queue>
#include <string>

namespace Index {

	namespace {
		// Renamed to RGBA to dodge ambiguity with Index::Color (pulled in transitively via AssetRegistry).
		ImU32 RGBA(int r, int g, int b, int a = 255) {
			return IM_COL32(r, g, b, a);
		}

		constexpr float k_MinZoom        = 0.05f;   // texture must stay visible
		constexpr float k_MaxZoom        = 64.0f;   // pixel-art friendly
		constexpr float k_HandleSizePx   = 8.0f;    // screen-space, zoom-independent
		constexpr float k_HandleHitPad   = 2.0f;
	}

	void SpriteEditorPanel::Initialize() {
	}

	void SpriteEditorPanel::Shutdown() {
		m_AssetPath.clear();
		m_AssetId = 0;
		m_PreviewHandle = TextureHandle{};
		m_Slices.clear();
		m_Selection.clear();
		m_Crop = TextureCrop{};
		m_CropDragActive = false;
		m_Dirty = false;
		m_DecodedPixels.clear();
	}

	void SpriteEditorPanel::OpenTexture(const std::string& assetPath, uint64_t assetId) {
		if (assetPath.empty()) {
			return;
		}
		if (m_AssetPath == assetPath) {
			return;
		}
		if (m_Dirty) {
			m_PendingAssetPath  = assetPath;
			m_PendingAssetId    = assetId;
			m_ShowUnsavedPrompt = true;
			return;
		}

		m_AssetPath   = assetPath;
		m_AssetId     = assetId;

		m_PreviewHandle = TextureManager::LoadTexture(assetPath);
		if (Texture2D* tex = TextureManager::GetTexture(m_PreviewHandle)) {
			m_TexWidth  = static_cast<int>(tex->GetWidth());
			m_TexHeight = static_cast<int>(tex->GetHeight());
		}
		else {
			m_TexWidth  = 0;
			m_TexHeight = 0;
		}

		const TextureMeta meta = AssetRegistry::ReadTextureMeta(assetPath);
		m_Slices    = meta.Sprites;
		m_Crop      = meta.Crop;
		// Guard against a crop authored before the texture was re-imported smaller.
		if (m_Crop.W > 0 && m_Crop.H > 0 && m_TexWidth > 0 && m_TexHeight > 0) {
			m_Crop.X = std::clamp(m_Crop.X, 0, std::max(0, m_TexWidth  - 1));
			m_Crop.Y = std::clamp(m_Crop.Y, 0, std::max(0, m_TexHeight - 1));
			m_Crop.W = std::clamp(m_Crop.W, 1, std::max(1, m_TexWidth  - m_Crop.X));
			m_Crop.H = std::clamp(m_Crop.H, 1, std::max(1, m_TexHeight - m_Crop.Y));
		}
		m_Selection.clear();
		m_Dirty = false;
		// Open straight into Crop mode if this texture is already cropped.
		m_EditMode = m_Crop.Enabled ? EditMode::Crop : EditMode::Slices;

		m_PanInitialized = false;          // ZoomToFitTexture next render
		m_IsDrawing      = false;
		m_DragMode       = DragMode::None;
		m_DragSliceIndex = -1;
		m_CropDragActive = false;

		m_DecodedPixels.clear();           // re-decoded lazily on Auto-slice
	}

	void SpriteEditorPanel::Render(bool* pOpen) {
		// Auto-grow the first time the user opens this panel so the
		// canvas is comfortable on a 1080p display.
		ImGui::SetNextWindowSize(ImVec2(900.0f, 640.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Sprite Editor", pOpen, ImGuiWindowFlags_NoCollapse)) {
			ImGui::End();
			return;
		}

		if (m_AssetPath.empty() || m_TexWidth <= 0 || m_TexHeight <= 0) {
			ImGui::TextDisabled("Select a texture asset and click \"Open Sprite Editor\".");
			ImGui::End();
			return;
		}

		// Header strip: asset path on the left, slice count on the right.
		ImGui::TextDisabled("%s", m_AssetPath.c_str());
		ImGui::SameLine();
		const float countWidth = ImGui::CalcTextSize("Slices: 0000").x;
		ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - countWidth + ImGui::GetCursorPosX());
		ImGui::TextDisabled("Slices: %zu%s", m_Slices.size(), m_Dirty ? " *" : "");

		RenderToolbar();
		ImGui::Separator();

		const float panelHeight    = ImGui::GetContentRegionAvail().y;
		const float sliceListWidth = 260.0f;
		const float canvasWidth    = std::max(100.0f, ImGui::GetContentRegionAvail().x - sliceListWidth - ImGui::GetStyle().ItemSpacing.x);

		ImGui::BeginChild("##SpriteEditorCanvas", ImVec2(canvasWidth, panelHeight),
			ImGuiChildFlags_Borders, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
		RenderCanvas();
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##SpriteEditorList", ImVec2(sliceListWidth, panelHeight),
			ImGuiChildFlags_Borders);
		if (m_EditMode == EditMode::Crop) {
			RenderCropPanel();
		}
		else {
			RenderSliceList();
		}
		ImGui::EndChild();

		// Delete selected slices via Entf or the numpad Del/comma key — works
		// whether the canvas or the slice list is focused. Skipped while a field
		// (e.g. a name InputText) is active so it doesn't eat that keypress.
		if (m_EditMode == EditMode::Slices
			&& !m_Selection.empty()
			&& !ImGui::IsAnyItemActive()
			&& ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
			&& (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)))
		{
			DeleteSelected();
		}

		RenderUnsavedPrompt();
		RenderSliceModePopup();

		ImGui::End();
	}

	// ─────────────────────────────────────────────────────────────────────
	// Toolbar
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::RenderToolbar() {
		// Edit-mode toggle: cut named slices vs. crop the single sprite.
		int editMode = static_cast<int>(m_EditMode);
		if (ImGui::RadioButton("Slices", &editMode, static_cast<int>(EditMode::Slices))) {
			m_EditMode = EditMode::Slices;
			m_IsDrawing = false;
			m_CropDragActive = false;
			m_DragMode = DragMode::None;
		}
		ImGui::SameLine();
		if (ImGui::RadioButton("Crop", &editMode, static_cast<int>(EditMode::Crop))) {
			m_EditMode = EditMode::Crop;
			m_IsDrawing = false;
			m_DragMode = DragMode::None;
			m_DragSliceIndex = -1;
			m_Selection.clear();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		const bool sliceMode = (m_EditMode == EditMode::Slices);
		if (!sliceMode) ImGui::BeginDisabled();
		if (ImGui::Button("Slice")) {
			m_OpenSlicePopup = true;
		}
		if (!sliceMode) ImGui::EndDisabled();
		ImGui::SameLine();
		const bool canApply = m_Dirty;
		if (!canApply) ImGui::BeginDisabled();
		if (ImGui::Button("Apply")) {
			Apply();
		}
		if (!canApply) ImGui::EndDisabled();

		ImGui::SameLine();
		if (!canApply) ImGui::BeginDisabled();
		if (ImGui::Button("Revert")) {
			Revert();
		}
		if (!canApply) ImGui::EndDisabled();

		// Zoom controls, right-aligned.
		ImGui::SameLine();
		const float zoomBlockWidth = 220.0f;
		ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - zoomBlockWidth + ImGui::GetCursorPosX());
		ImGui::Text("Zoom:");
		ImGui::SameLine();
		if (ImGui::Button("-##zoom")) {
			m_Zoom = std::max(k_MinZoom, m_Zoom * 0.8f);
		}
		ImGui::SameLine();
		ImGui::Text("%5.0f%%", m_Zoom * 100.0f);
		ImGui::SameLine();
		if (ImGui::Button("+##zoom")) {
			m_Zoom = std::min(k_MaxZoom, m_Zoom * 1.25f);
		}
		ImGui::SameLine();
		if (ImGui::Button("Fit")) {
			ZoomToFitTexture();
		}
	}

	void SpriteEditorPanel::RenderSliceModePopup() {
		if (m_OpenSlicePopup) {
			ImGui::OpenPopup("##SliceModePopup");
			m_OpenSlicePopup = false;
		}

		ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::BeginPopup("##SliceModePopup")) {
			return;
		}

		ImGui::TextDisabled("Mode");
		int mode = static_cast<int>(m_SliceMode);
		const char* modeNames[] = { "Manual", "Grid By Cell Size", "Grid By Cell Count", "Auto (Transparent Gutters)" };
		if (ImGui::Combo("##Mode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
			m_SliceMode = static_cast<SliceMode>(mode);
		}

		ImGui::Spacing();
		switch (m_SliceMode) {
		case SliceMode::Manual:
			ImGui::TextWrapped("Drag a rectangle on the canvas to add a new slice. "
				"This mode has no Slice button to press — just click and drag.");
			break;

		case SliceMode::GridBySize:
			ImGui::DragInt("Cell W",   &m_CellW,    1, 1, m_TexWidth);
			ImGui::DragInt("Cell H",   &m_CellH,    1, 1, m_TexHeight);
			ImGui::DragInt("Padding X", &m_PaddingX, 1, 0, 256);
			ImGui::DragInt("Padding Y", &m_PaddingY, 1, 0, 256);
			ImGui::DragInt("Offset X",  &m_OffsetX,  1, 0, m_TexWidth);
			ImGui::DragInt("Offset Y",  &m_OffsetY,  1, 0, m_TexHeight);
			break;

		case SliceMode::GridByCount:
			ImGui::DragInt("Columns",   &m_GridCols, 1, 1, 256);
			ImGui::DragInt("Rows",      &m_GridRows, 1, 1, 256);
			ImGui::DragInt("Padding X", &m_PaddingX, 1, 0, 256);
			ImGui::DragInt("Padding Y", &m_PaddingY, 1, 0, 256);
			break;

		case SliceMode::Auto:
			ImGui::DragInt("Alpha Threshold", &m_AutoSliceAlphaThreshold, 1, 0, 255);
			ImGui::DragInt("Min Slice Size",  &m_AutoSliceMinSize,        1, 1, 256);
			if (!BitmapAvailable()) {
				ImGui::TextDisabled("(Pixel data will be re-decoded on first Slice.)");
			}
			break;
		}

		ImGui::Spacing();
		ImGui::Checkbox("Keep existing slices", &m_KeepExistingOnSlice);
		ImGui::Spacing();

		const bool canSlice = (m_SliceMode != SliceMode::Manual);
		if (!canSlice) ImGui::BeginDisabled();
		if (ImGui::Button("Slice", ImVec2(-FLT_MIN, 0.0f))) {
			switch (m_SliceMode) {
			case SliceMode::Manual:        break; // unreachable (disabled)
			case SliceMode::GridBySize:
				SliceByGridSize(m_CellW, m_CellH, m_PaddingX, m_PaddingY,
					m_OffsetX, m_OffsetY, m_KeepExistingOnSlice);
				break;
			case SliceMode::GridByCount:
				SliceByGridCount(m_GridCols, m_GridRows, m_PaddingX, m_PaddingY, m_KeepExistingOnSlice);
				break;
			case SliceMode::Auto:
				EnsureDecodedBitmap();
				if (!BitmapAvailable()) {
					IDX_WARN_TAG("SpriteEditor",
						"Auto-slice unavailable: failed to decode '{}'.", m_AssetPath);
				}
				else {
					SliceAuto(static_cast<uint8_t>(std::clamp(m_AutoSliceAlphaThreshold, 0, 255)),
						std::max(1, m_AutoSliceMinSize),
						m_KeepExistingOnSlice);
				}
				break;
			}
			ImGui::CloseCurrentPopup();
		}
		if (!canSlice) ImGui::EndDisabled();

		ImGui::EndPopup();
	}

	void SpriteEditorPanel::RenderUnsavedPrompt() {
		if (m_ShowUnsavedPrompt) {
			ImGui::OpenPopup("Unsaved Sprite Edits");
			m_ShowUnsavedPrompt = false;
		}
		ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);
		if (!ImGui::BeginPopupModal("Unsaved Sprite Edits", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			return;
		}

		ImGui::TextWrapped("This texture has unsaved slice edits. Save them before switching?");
		ImGui::Spacing();

		const float buttonWidth = 90.0f;
		if (ImGui::Button("Save", ImVec2(buttonWidth, 0.0f))) {
			Apply();
			const std::string newPath = m_PendingAssetPath;
			const uint64_t    newId   = m_PendingAssetId;
			m_PendingAssetPath.clear();
			m_PendingAssetId = 0;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			OpenTexture(newPath, newId);
			return;
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(buttonWidth, 0.0f))) {
			m_Dirty = false; // drop staged edits — Revert would re-read .meta which we're about to do anyway
			const std::string newPath = m_PendingAssetPath;
			const uint64_t    newId   = m_PendingAssetId;
			m_PendingAssetPath.clear();
			m_PendingAssetId = 0;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			OpenTexture(newPath, newId);
			return;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f))) {
			m_PendingAssetPath.clear();
			m_PendingAssetId = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// ─────────────────────────────────────────────────────────────────────
	// Canvas
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::RenderCanvas() {
		const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
		const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
		const ImVec2 canvasMax(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		m_LastCanvasSize = canvasSize;

		drawList->AddRectFilled(canvasMin, canvasMax, RGBA(28, 28, 28));
		const float checker = 16.0f;
		for (float y = canvasMin.y; y < canvasMax.y; y += checker) {
			for (float x = canvasMin.x; x < canvasMax.x; x += checker) {
				const int ix = static_cast<int>((x - canvasMin.x) / checker);
				const int iy = static_cast<int>((y - canvasMin.y) / checker);
				const bool even = ((ix + iy) % 2) == 0;
				drawList->AddRectFilled(
					ImVec2(x, y),
					ImVec2(std::min(x + checker, canvasMax.x), std::min(y + checker, canvasMax.y)),
					even ? RGBA(40, 40, 40) : RGBA(55, 55, 55));
			}
		}

		if (!m_PanInitialized) {
			ZoomToFitTexture();
			m_PanInitialized = true;
		}

		const ImVec2 imageMin(canvasMin.x + m_PanOffset.x, canvasMin.y + m_PanOffset.y);
		const ImVec2 imageMax(imageMin.x + m_TexWidth * m_Zoom, imageMin.y + m_TexHeight * m_Zoom);
		drawList->PushClipRect(canvasMin, canvasMax, true);
		if (Texture2D* tex = TextureManager::GetTexture(m_PreviewHandle); tex && tex->IsValid()) {
			drawList->AddImage((ImTextureID)(intptr_t)tex->GetHandle(),
				imageMin, imageMax, ImVec2(0, 0), ImVec2(1, 1));
		}
		// Border around the texture so the user sees its extents.
		drawList->AddRect(imageMin, imageMax, RGBA(180, 180, 180, 200));

		if (m_EditMode == EditMode::Crop) {
			DrawCropOverlay(drawList);
		}

		// Slice rects (Slices mode only).
		for (size_t i = 0; m_EditMode == EditMode::Slices && i < m_Slices.size(); ++i) {
			const SpriteSlice& slice = m_Slices[i];
			const ImVec2 minPx = TextureToScreen(ImVec2(static_cast<float>(slice.X), static_cast<float>(slice.Y)));
			const ImVec2 maxPx = TextureToScreen(ImVec2(static_cast<float>(slice.X + slice.W), static_cast<float>(slice.Y + slice.H)));
			const bool selected = IsSelected(static_cast<int>(i));
			const ImU32 fillCol = selected ? RGBA(80, 160, 240, 60) : RGBA(240, 200, 60, 30);
			const ImU32 borderCol = selected ? RGBA(80, 160, 240, 255) : RGBA(240, 200, 60, 255);
			drawList->AddRectFilled(minPx, maxPx, fillCol);
			drawList->AddRect(minPx, maxPx, borderCol, 0.0f, 0, selected ? 2.0f : 1.0f);

			// Selection handles for the primary-selected slice only —
			// drawing 8 handles for every selected slice gets noisy in a
			// large multi-select.
			if (selected && i == static_cast<size_t>(m_Selection.front())) {
				const float h = k_HandleSizePx * 0.5f;
				auto handle = [&](ImVec2 centre) {
					drawList->AddRectFilled(
						ImVec2(centre.x - h, centre.y - h),
						ImVec2(centre.x + h, centre.y + h),
						RGBA(255, 255, 255), 0.0f);
					drawList->AddRect(
						ImVec2(centre.x - h, centre.y - h),
						ImVec2(centre.x + h, centre.y + h),
						RGBA(0, 0, 0), 0.0f);
				};
				const ImVec2 mid((minPx.x + maxPx.x) * 0.5f, (minPx.y + maxPx.y) * 0.5f);
				handle(minPx);
				handle(ImVec2(maxPx.x, minPx.y));
				handle(ImVec2(minPx.x, maxPx.y));
				handle(maxPx);
				handle(ImVec2(mid.x, minPx.y));
				handle(ImVec2(mid.x, maxPx.y));
				handle(ImVec2(minPx.x, mid.y));
				handle(ImVec2(maxPx.x, mid.y));
			}
		}

		// In-progress manual drag-out preview (Crop mode draws its own).
		if (m_IsDrawing && m_EditMode == EditMode::Slices) {
			const ImVec2 mouseTexPx = ScreenToTexture(ImGui::GetIO().MousePos);
			const ImVec2 a = TextureToScreen(m_DrawStartTexPx);
			const ImVec2 b = TextureToScreen(mouseTexPx);
			drawList->AddRect(
				ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
				ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)),
				RGBA(255, 255, 255, 230), 0.0f, 0, 1.5f);
		}

		drawList->PopClipRect();

		ImGui::SetCursorScreenPos(canvasMin);
		ImGui::InvisibleButton("##SpriteEditorCanvasInteract", canvasSize,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = ImGui::IsItemHovered();

		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mouseTexPx = ScreenToTexture(io.MousePos);

		if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
			m_PanOffset.x += io.MouseDelta.x;
			m_PanOffset.y += io.MouseDelta.y;
		}

		// Zoom anchored on cursor so the view stays centred on what the user is looking at.
		if (hovered && io.MouseWheel != 0.0f) {
			const float oldZoom = m_Zoom;
			const float factor  = (io.MouseWheel > 0) ? 1.15f : (1.0f / 1.15f);
			const float newZoom = std::clamp(oldZoom * factor, k_MinZoom, k_MaxZoom);
			if (newZoom != oldZoom) {
				const ImVec2 anchorTexPx = ScreenToTexture(io.MousePos);
				m_Zoom = newZoom;
				const ImVec2 anchorScreenAfter = TextureToScreen(anchorTexPx);
				m_PanOffset.x += io.MousePos.x - anchorScreenAfter.x;
				m_PanOffset.y += io.MousePos.y - anchorScreenAfter.y;
			}
		}

		if (m_EditMode == EditMode::Crop) {
			HandleCropInteraction(hovered, io, mouseTexPx);
			return;
		}

		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			Handle hit = Handle::None;
			const int hitSlice = HitTestSlice(mouseTexPx, hit);
			if (hitSlice >= 0) {
				// Click on an existing slice → select (with Ctrl/Shift
				// behaviour) and start drag.
				if (io.KeyCtrl) {
					ToggleSelected(hitSlice);
				}
				else if (!IsSelected(hitSlice)) {
					SelectOnly(hitSlice);
				}
				m_DragSliceIndex     = hitSlice;
				m_DragHandle         = hit;
				m_DragStartMouseTexPx = mouseTexPx;
				m_DragStartSliceState = m_Slices[hitSlice];
				if (hit == Handle::None) {
					m_DragMode = DragMode::MoveBody;
				}
				else if (hit == Handle::TopLeft || hit == Handle::TopRight
					|| hit == Handle::BottomLeft || hit == Handle::BottomRight) {
					m_DragMode = DragMode::ResizeCorner;
				}
				else {
					m_DragMode = DragMode::ResizeEdge;
				}
			}
			else if (m_SliceMode == SliceMode::Manual
				&& mouseTexPx.x >= 0 && mouseTexPx.y >= 0
				&& mouseTexPx.x <= static_cast<float>(m_TexWidth)
				&& mouseTexPx.y <= static_cast<float>(m_TexHeight))
			{
				// Manual mode: click on empty texture → start draw-out.
				m_IsDrawing      = true;
				m_DrawStartTexPx = mouseTexPx;
			}
			else if (!io.KeyCtrl) {
				m_Selection.clear();
			}
		}

		// Live drag updates.
		if (m_DragMode != DragMode::None && m_DragSliceIndex >= 0
			&& m_DragSliceIndex < static_cast<int>(m_Slices.size()))
		{
			SpriteSlice& slice = m_Slices[m_DragSliceIndex];
			const int dx = static_cast<int>(std::round(mouseTexPx.x - m_DragStartMouseTexPx.x));
			const int dy = static_cast<int>(std::round(mouseTexPx.y - m_DragStartMouseTexPx.y));
			ApplyHandleDrag(slice.X, slice.Y, slice.W, slice.H,
				m_DragStartSliceState.X, m_DragStartSliceState.Y,
				m_DragStartSliceState.W, m_DragStartSliceState.H, dx, dy);
			m_Dirty = true;
		}

		// Esc cancels the in-progress drag (snap back to pre-drag state).
		if (m_DragMode != DragMode::None && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			if (m_DragSliceIndex >= 0 && m_DragSliceIndex < static_cast<int>(m_Slices.size())) {
				m_Slices[m_DragSliceIndex] = m_DragStartSliceState;
			}
			m_DragMode       = DragMode::None;
			m_DragSliceIndex = -1;
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			m_DragMode       = DragMode::None;
			m_DragSliceIndex = -1;
			m_DragHandle     = Handle::None;
		}

		// Commit the manual draw-out on mouse release.
		if (m_IsDrawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			const ImVec2 end = mouseTexPx;
			int x0 = static_cast<int>(std::round(std::min(m_DrawStartTexPx.x, end.x)));
			int y0 = static_cast<int>(std::round(std::min(m_DrawStartTexPx.y, end.y)));
			int x1 = static_cast<int>(std::round(std::max(m_DrawStartTexPx.x, end.x)));
			int y1 = static_cast<int>(std::round(std::max(m_DrawStartTexPx.y, end.y)));
			x0 = std::clamp(x0, 0, m_TexWidth);
			y0 = std::clamp(y0, 0, m_TexHeight);
			x1 = std::clamp(x1, 0, m_TexWidth);
			y1 = std::clamp(y1, 0, m_TexHeight);
			const int w = x1 - x0;
			const int h = y1 - y0;
			if (w > 0 && h > 0) {
				SpriteSlice slice;
				slice.X = x0; slice.Y = y0;
				slice.W = w;  slice.H = h;
				slice.PivotX = 0.5f; slice.PivotY = 0.5f;
				slice.Name = MakeUniqueName("slice_1");
				AppendUniqueSlice(std::move(slice));
				SelectOnly(static_cast<int>(m_Slices.size() - 1));
				m_Dirty = true;
			}
			m_IsDrawing = false;
		}

		if (hovered) {
			// Delete is handled at panel scope (see Render) so it also fires
			// when the slice list — not the canvas — has focus.
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && !m_Slices.empty()) {
				m_Selection.clear();
				m_Selection.reserve(m_Slices.size());
				for (int i = 0; i < static_cast<int>(m_Slices.size()); ++i) m_Selection.push_back(i);
			}
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !m_Selection.empty()) {
				DuplicateSelected();
			}
		}
	}

	// ─────────────────────────────────────────────────────────────────────
	// Crop mode (single-sprite trim)
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::HandleCropInteraction(bool hovered, const ImGuiIO& io, ImVec2 mouseTexPx) {
		(void)io;
		// Click a handle/body to drag the crop, or click empty texture to draw a fresh box.
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			Handle hit = Handle::None;
			if (HitTestCropRect(mouseTexPx, hit)) {
				m_DragHandle          = hit;
				m_DragStartMouseTexPx = mouseTexPx;
				m_DragStartCrop       = m_Crop;
				m_CropDragActive      = true;
				if (hit == Handle::None) {
					m_DragMode = DragMode::MoveBody;
				}
				else if (hit == Handle::TopLeft || hit == Handle::TopRight
					|| hit == Handle::BottomLeft || hit == Handle::BottomRight) {
					m_DragMode = DragMode::ResizeCorner;
				}
				else {
					m_DragMode = DragMode::ResizeEdge;
				}
			}
			else if (mouseTexPx.x >= 0 && mouseTexPx.y >= 0
				&& mouseTexPx.x <= static_cast<float>(m_TexWidth)
				&& mouseTexPx.y <= static_cast<float>(m_TexHeight))
			{
				m_IsDrawing      = true;
				m_DrawStartTexPx = mouseTexPx;
			}
		}

		// Live drag of the crop rect.
		if (m_CropDragActive && m_DragMode != DragMode::None) {
			const int dx = static_cast<int>(std::round(mouseTexPx.x - m_DragStartMouseTexPx.x));
			const int dy = static_cast<int>(std::round(mouseTexPx.y - m_DragStartMouseTexPx.y));
			ApplyHandleDrag(m_Crop.X, m_Crop.Y, m_Crop.W, m_Crop.H,
				m_DragStartCrop.X, m_DragStartCrop.Y, m_DragStartCrop.W, m_DragStartCrop.H, dx, dy);
			m_Dirty = true;
		}

		// Esc cancels the in-progress crop drag (snap back to pre-drag state).
		if (m_CropDragActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			m_Crop           = m_DragStartCrop;
			m_DragMode       = DragMode::None;
			m_CropDragActive = false;
		}
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			m_DragMode       = DragMode::None;
			m_CropDragActive = false;
			m_DragHandle     = Handle::None;
		}

		// Commit a freshly drawn crop box on mouse release.
		if (m_IsDrawing && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			int x0 = static_cast<int>(std::round(std::min(m_DrawStartTexPx.x, mouseTexPx.x)));
			int y0 = static_cast<int>(std::round(std::min(m_DrawStartTexPx.y, mouseTexPx.y)));
			int x1 = static_cast<int>(std::round(std::max(m_DrawStartTexPx.x, mouseTexPx.x)));
			int y1 = static_cast<int>(std::round(std::max(m_DrawStartTexPx.y, mouseTexPx.y)));
			x0 = std::clamp(x0, 0, m_TexWidth);
			y0 = std::clamp(y0, 0, m_TexHeight);
			x1 = std::clamp(x1, 0, m_TexWidth);
			y1 = std::clamp(y1, 0, m_TexHeight);
			const int w = x1 - x0;
			const int h = y1 - y0;
			if (w > 0 && h > 0) {
				m_Crop.X = x0; m_Crop.Y = y0;
				m_Crop.W = w;  m_Crop.H = h;
				m_Crop.Enabled = true;
				m_Dirty = true;
			}
			m_IsDrawing = false;
		}
	}

	void SpriteEditorPanel::DrawCropOverlay(ImDrawList* drawList) {
		const ImVec2 imgMin = TextureToScreen(ImVec2(0.0f, 0.0f));
		const ImVec2 imgMax = TextureToScreen(ImVec2(static_cast<float>(m_TexWidth), static_cast<float>(m_TexHeight)));

		if (m_Crop.Enabled && m_Crop.W > 0 && m_Crop.H > 0) {
			const ImVec2 cMin = TextureToScreen(ImVec2(static_cast<float>(m_Crop.X), static_cast<float>(m_Crop.Y)));
			const ImVec2 cMax = TextureToScreen(ImVec2(static_cast<float>(m_Crop.X + m_Crop.W), static_cast<float>(m_Crop.Y + m_Crop.H)));

			// Dim the four texture bands outside the crop so the kept region reads clearly.
			const ImU32 dim = RGBA(0, 0, 0, 150);
			drawList->AddRectFilled(imgMin, ImVec2(imgMax.x, cMin.y), dim);
			drawList->AddRectFilled(ImVec2(imgMin.x, cMax.y), imgMax, dim);
			drawList->AddRectFilled(ImVec2(imgMin.x, cMin.y), ImVec2(cMin.x, cMax.y), dim);
			drawList->AddRectFilled(ImVec2(cMax.x, cMin.y), ImVec2(imgMax.x, cMax.y), dim);

			drawList->AddRect(cMin, cMax, RGBA(90, 230, 150, 255), 0.0f, 0, 2.0f);

			const float hs = k_HandleSizePx * 0.5f;
			auto handle = [&](ImVec2 centre) {
				drawList->AddRectFilled(ImVec2(centre.x - hs, centre.y - hs), ImVec2(centre.x + hs, centre.y + hs), RGBA(255, 255, 255), 0.0f);
				drawList->AddRect(ImVec2(centre.x - hs, centre.y - hs), ImVec2(centre.x + hs, centre.y + hs), RGBA(0, 0, 0), 0.0f);
			};
			const ImVec2 mid((cMin.x + cMax.x) * 0.5f, (cMin.y + cMax.y) * 0.5f);
			handle(cMin);
			handle(ImVec2(cMax.x, cMin.y));
			handle(ImVec2(cMin.x, cMax.y));
			handle(cMax);
			handle(ImVec2(mid.x, cMin.y));
			handle(ImVec2(mid.x, cMax.y));
			handle(ImVec2(cMin.x, mid.y));
			handle(ImVec2(cMax.x, mid.y));
		}

		// Draw-out preview for a fresh crop box.
		if (m_IsDrawing) {
			const ImVec2 cur = ScreenToTexture(ImGui::GetIO().MousePos);
			const ImVec2 a = TextureToScreen(m_DrawStartTexPx);
			const ImVec2 b = TextureToScreen(cur);
			drawList->AddRect(
				ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
				ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)),
				RGBA(90, 230, 150, 230), 0.0f, 0, 1.5f);
		}
	}

	bool SpriteEditorPanel::HitTestCropRect(ImVec2 mouseTexPx, Handle& outHandle) const {
		outHandle = Handle::None;
		if (!m_Crop.Enabled || m_Crop.W <= 0 || m_Crop.H <= 0) return false;
		outHandle = HitRectHandle(m_Crop.X, m_Crop.Y, m_Crop.W, m_Crop.H, mouseTexPx);
		if (outHandle != Handle::None) return true;
		return mouseTexPx.x >= m_Crop.X && mouseTexPx.x <= m_Crop.X + m_Crop.W
			&& mouseTexPx.y >= m_Crop.Y && mouseTexPx.y <= m_Crop.Y + m_Crop.H;
	}

	SpriteEditorPanel::Handle SpriteEditorPanel::HitRectHandle(int x, int y, int w, int h, ImVec2 mouseTexPx) const {
		// Handle hit zones are in SCREEN space (zoom-independent), so convert
		// the screen radius to a tex-space radius via 1/zoom.
		const float radTex = (k_HandleSizePx * 0.5f + k_HandleHitPad) / std::max(0.01f, m_Zoom);
		const float xL = static_cast<float>(x);
		const float yT = static_cast<float>(y);
		const float xR = static_cast<float>(x + w);
		const float yB = static_cast<float>(y + h);
		const float xM = xL + w * 0.5f;
		const float yM = yT + h * 0.5f;
		struct H { Handle h; ImVec2 c; };
		const H handles[] = {
			{ Handle::TopLeft,     ImVec2(xL, yT) },
			{ Handle::TopRight,    ImVec2(xR, yT) },
			{ Handle::BottomLeft,  ImVec2(xL, yB) },
			{ Handle::BottomRight, ImVec2(xR, yB) },
			{ Handle::Top,         ImVec2(xM, yT) },
			{ Handle::Bottom,      ImVec2(xM, yB) },
			{ Handle::Left,        ImVec2(xL, yM) },
			{ Handle::Right,       ImVec2(xR, yM) },
		};
		for (const H& hnd : handles) {
			if (PointInRect(mouseTexPx,
				ImVec2(hnd.c.x - radTex, hnd.c.y - radTex),
				ImVec2(hnd.c.x + radTex, hnd.c.y + radTex)))
			{
				return hnd.h;
			}
		}
		return Handle::None;
	}

	void SpriteEditorPanel::ApplyHandleDrag(int& X, int& Y, int& W, int& H,
		int startX, int startY, int startW, int startH, int dx, int dy) const
	{
		if (m_DragMode == DragMode::MoveBody) {
			X = startX + dx;
			Y = startY + dy;
		}
		else {
			int x0 = startX;
			int y0 = startY;
			int x1 = startX + startW;
			int y1 = startY + startH;
			switch (m_DragHandle) {
			case Handle::TopLeft:     x0 += dx; y0 += dy; break;
			case Handle::TopRight:    x1 += dx; y0 += dy; break;
			case Handle::BottomLeft:  x0 += dx; y1 += dy; break;
			case Handle::BottomRight: x1 += dx; y1 += dy; break;
			case Handle::Top:         y0 += dy; break;
			case Handle::Bottom:      y1 += dy; break;
			case Handle::Left:        x0 += dx; break;
			case Handle::Right:       x1 += dx; break;
			case Handle::None:        break;
			}
			if (x1 < x0) std::swap(x0, x1);
			if (y1 < y0) std::swap(y0, y1);
			X = x0; Y = y0;
			W = std::max(1, x1 - x0);
			H = std::max(1, y1 - y0);
		}
		X = std::clamp(X, 0, std::max(0, m_TexWidth - 1));
		Y = std::clamp(Y, 0, std::max(0, m_TexHeight - 1));
		W = std::clamp(W, 1, std::max(1, m_TexWidth  - X));
		H = std::clamp(H, 1, std::max(1, m_TexHeight - Y));
	}

	// ─────────────────────────────────────────────────────────────────────
	// Slice list
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::RenderSliceList() {
		ImGui::TextDisabled("Slices");
		ImGui::SameLine();
		const float buttonW = 24.0f;
		ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - buttonW + ImGui::GetCursorPosX());
		if (ImGui::SmallButton("All##SelectAll") && !m_Slices.empty()) {
			m_Selection.clear();
			m_Selection.reserve(m_Slices.size());
			for (int i = 0; i < static_cast<int>(m_Slices.size()); ++i) m_Selection.push_back(i);
		}
		ImGui::Separator();

		ImGui::BeginChild("##SpriteEditorListScroll", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 4.0f));
		for (int i = 0; i < static_cast<int>(m_Slices.size()); ++i) {
			SpriteSlice& slice = m_Slices[i];
			ImGui::PushID(i);
			const bool selected = IsSelected(i);
			if (ImGui::Selectable("##sel", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
				ImVec2(0.0f, ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f)))
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyCtrl) {
					ToggleSelected(i);
				}
				else {
					SelectOnly(i);
					FitCanvasToSlice(slice);
				}
			}
			ImGui::SameLine();

			ImGui::BeginGroup();
			char nameBuf[128];
			std::snprintf(nameBuf, sizeof(nameBuf), "%s", slice.Name.c_str());
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf),
				ImGuiInputTextFlags_EnterReturnsTrue))
			{
				const std::string proposed = nameBuf;
				if (!proposed.empty() && proposed != slice.Name) {
					// Skip uniqueness if the user kept the same name —
					// that's a no-op edit. Otherwise re-uniquify against
					// the rest of the list (excluding this slice).
					const std::string old = std::move(slice.Name);
					slice.Name.clear();
					slice.Name = MakeUniqueName(proposed);
					m_Dirty = true;
					if (slice.Name != proposed && slice.Name != old) {
						IDX_INFO_TAG("SpriteEditor",
							"Slice name '{}' was taken; renamed to '{}'.", proposed, slice.Name);
					}
				}
			}
			int rect[4] = { slice.X, slice.Y, slice.W, slice.H };
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragInt4("##rect", rect, 1, 0, 65535, "%d")) {
				slice.X = std::clamp(rect[0], 0, std::max(0, m_TexWidth - 1));
				slice.Y = std::clamp(rect[1], 0, std::max(0, m_TexHeight - 1));
				slice.W = std::clamp(rect[2], 1, std::max(1, m_TexWidth  - slice.X));
				slice.H = std::clamp(rect[3], 1, std::max(1, m_TexHeight - slice.Y));
				m_Dirty = true;
			}
			float pivot[2] = { slice.PivotX, slice.PivotY };
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::DragFloat2("##pivot", pivot, 0.01f, 0.0f, 1.0f, "%.2f")) {
				slice.PivotX = pivot[0];
				slice.PivotY = pivot[1];
				m_Dirty = true;
			}
			ImGui::EndGroup();

			ImGui::PopID();
			ImGui::Separator();
		}
		ImGui::EndChild();

		const bool canDelete = !m_Selection.empty();
		if (!canDelete) ImGui::BeginDisabled();
		if (ImGui::Button("Delete Selected", ImVec2(-FLT_MIN, 0.0f))) {
			DeleteSelected();
		}
		if (!canDelete) ImGui::EndDisabled();
	}

	// ─────────────────────────────────────────────────────────────────────
	// Crop panel
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::RenderCropPanel() {
		ImGui::TextDisabled("Crop");
		ImGui::Separator();
		ImGui::TextWrapped("Trims the single sprite. Applies when a Sprite Renderer or Image "
			"uses this texture with no sprite selected.");
		ImGui::Spacing();

		bool enabled = m_Crop.Enabled;
		if (ImGui::Checkbox("Enable Crop", &enabled)) {
			// First enable with no rect yet → seed it to the full texture.
			if (enabled && (m_Crop.W <= 0 || m_Crop.H <= 0)) {
				m_Crop.X = 0; m_Crop.Y = 0;
				m_Crop.W = m_TexWidth; m_Crop.H = m_TexHeight;
			}
			m_Crop.Enabled = enabled;
			m_Dirty = true;
		}

		if (!m_Crop.Enabled) ImGui::BeginDisabled();

		ImGui::Spacing();
		ImGui::TextDisabled("Rect (X, Y, W, H)");
		int rect[4] = { m_Crop.X, m_Crop.Y, m_Crop.W, m_Crop.H };
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::DragInt4("##croprect", rect, 1, 0, 65535, "%d")) {
			m_Crop.X = std::clamp(rect[0], 0, std::max(0, m_TexWidth  - 1));
			m_Crop.Y = std::clamp(rect[1], 0, std::max(0, m_TexHeight - 1));
			m_Crop.W = std::clamp(rect[2], 1, std::max(1, m_TexWidth  - m_Crop.X));
			m_Crop.H = std::clamp(rect[3], 1, std::max(1, m_TexHeight - m_Crop.Y));
			m_Dirty = true;
		}

		ImGui::Spacing();
		if (ImGui::Button("Reset to Full Texture", ImVec2(-FLT_MIN, 0.0f))) {
			m_Crop.X = 0; m_Crop.Y = 0;
			m_Crop.W = m_TexWidth; m_Crop.H = m_TexHeight;
			m_Dirty = true;
		}

		if (!m_Crop.Enabled) ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::TextWrapped("Drag a box on the canvas to set the crop, or drag the handles to adjust.");
	}

	// ─────────────────────────────────────────────────────────────────────
	// Slice algorithms
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::SliceByGridSize(int cellW, int cellH, int padX, int padY,
		int offsetX, int offsetY, bool keepExisting)
	{
		if (cellW < 1 || cellH < 1) return;
		if (!keepExisting) m_Slices.clear();
		std::string stem = std::filesystem::path(m_AssetPath).stem().string();
		if (stem.empty()) stem = "slice";

		int nextIndex = 0;
		for (int y = offsetY; y + cellH <= m_TexHeight; y += cellH + padY) {
			for (int x = offsetX; x + cellW <= m_TexWidth; x += cellW + padX) {
				SpriteSlice slice;
				slice.X = x; slice.Y = y;
				slice.W = cellW; slice.H = cellH;
				slice.Name = MakeUniqueName(stem + "_" + std::to_string(nextIndex++));
				m_Slices.push_back(std::move(slice));
			}
		}
		m_Selection.clear();
		m_Dirty = true;
	}

	void SpriteEditorPanel::SliceByGridCount(int cols, int rows, int padX, int padY, bool keepExisting) {
		if (cols < 1 || rows < 1) return;
		const int cellW = (m_TexWidth  - (cols - 1) * padX) / cols;
		const int cellH = (m_TexHeight - (rows - 1) * padY) / rows;
		if (cellW < 1 || cellH < 1) {
			IDX_WARN_TAG("SpriteEditor",
				"Grid-by-count yielded cell {}x{} on {}x{} texture — refusing.",
				cellW, cellH, m_TexWidth, m_TexHeight);
			return;
		}
		SliceByGridSize(cellW, cellH, padX, padY, 0, 0, keepExisting);
	}

	void SpriteEditorPanel::SliceAuto(uint8_t alphaThreshold, int minSize, bool keepExisting) {
		if (!BitmapAvailable() || m_TexWidth <= 0 || m_TexHeight <= 0) return;
		if (!keepExisting) m_Slices.clear();

		const int W = m_TexWidth;
		const int H = m_TexHeight;
		const std::vector<uint8_t>& px = m_DecodedPixels;
		std::vector<uint8_t> visited(static_cast<std::size_t>(W) * H, 0);

		auto alphaAt = [&](int x, int y) -> uint8_t {
			return px[(static_cast<std::size_t>(y) * W + x) * 4 + 3];
		};

		std::string stem = std::filesystem::path(m_AssetPath).stem().string();
		if (stem.empty()) stem = "slice";

		int emitted = 0;
		std::queue<std::pair<int,int>> frontier;
		for (int y = 0; y < H; ++y) {
			for (int x = 0; x < W; ++x) {
				if (visited[static_cast<std::size_t>(y) * W + x]) continue;
				if (alphaAt(x, y) < alphaThreshold) {
					visited[static_cast<std::size_t>(y) * W + x] = 1;
					continue;
				}
				// 4-connected flood. Bounding box collected as we go.
				int minX = x, minY = y, maxX = x, maxY = y;
				frontier.emplace(x, y);
				visited[static_cast<std::size_t>(y) * W + x] = 1;
				while (!frontier.empty()) {
					const auto [cx, cy] = frontier.front();
					frontier.pop();
					minX = std::min(minX, cx); maxX = std::max(maxX, cx);
					minY = std::min(minY, cy); maxY = std::max(maxY, cy);
					constexpr int dx[4] = { -1, 1,  0, 0 };
					constexpr int dy[4] = {  0, 0, -1, 1 };
					for (int k = 0; k < 4; ++k) {
						const int nx = cx + dx[k];
						const int ny = cy + dy[k];
						if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
						const std::size_t idx = static_cast<std::size_t>(ny) * W + nx;
						if (visited[idx]) continue;
						if (alphaAt(nx, ny) < alphaThreshold) {
							visited[idx] = 1;
							continue;
						}
						visited[idx] = 1;
						frontier.emplace(nx, ny);
					}
				}

				const int w = maxX - minX + 1;
				const int h = maxY - minY + 1;
				if (w < minSize || h < minSize) continue;

				SpriteSlice slice;
				slice.X = minX; slice.Y = minY;
				slice.W = w;    slice.H = h;
				slice.Name = MakeUniqueName(stem + "_" + std::to_string(emitted++));
				m_Slices.push_back(std::move(slice));
			}
		}
		m_Selection.clear();
		m_Dirty = true;
	}

	// ─────────────────────────────────────────────────────────────────────
	// Canvas math
	// ─────────────────────────────────────────────────────────────────────

	ImVec2 SpriteEditorPanel::TextureToScreen(ImVec2 texPx) const {
		const ImVec2 canvasMin = ImGui::GetWindowPos();
		const ImVec2 contentMin(canvasMin.x + ImGui::GetWindowContentRegionMin().x,
								 canvasMin.y + ImGui::GetWindowContentRegionMin().y);
		return ImVec2(
			contentMin.x + m_PanOffset.x + texPx.x * m_Zoom,
			contentMin.y + m_PanOffset.y + texPx.y * m_Zoom);
	}

	ImVec2 SpriteEditorPanel::ScreenToTexture(ImVec2 screenPx) const {
		const ImVec2 canvasMin = ImGui::GetWindowPos();
		const ImVec2 contentMin(canvasMin.x + ImGui::GetWindowContentRegionMin().x,
								 canvasMin.y + ImGui::GetWindowContentRegionMin().y);
		return ImVec2(
			(screenPx.x - contentMin.x - m_PanOffset.x) / m_Zoom,
			(screenPx.y - contentMin.y - m_PanOffset.y) / m_Zoom);
	}

	void SpriteEditorPanel::ZoomToFitTexture() {
		if (m_LastCanvasSize.x <= 0.0f || m_LastCanvasSize.y <= 0.0f
			|| m_TexWidth <= 0 || m_TexHeight <= 0)
		{
			m_Zoom = 1.0f;
			m_PanOffset = ImVec2(0.0f, 0.0f);
			return;
		}
		const float padding = 24.0f;
		const float availW  = std::max(1.0f, m_LastCanvasSize.x - padding);
		const float availH  = std::max(1.0f, m_LastCanvasSize.y - padding);
		const float fitZoom = std::min(availW / static_cast<float>(m_TexWidth),
									   availH / static_cast<float>(m_TexHeight));
		m_Zoom = std::clamp(fitZoom, k_MinZoom, k_MaxZoom);
		// Centre inside the canvas.
		m_PanOffset.x = (m_LastCanvasSize.x - m_TexWidth  * m_Zoom) * 0.5f;
		m_PanOffset.y = (m_LastCanvasSize.y - m_TexHeight * m_Zoom) * 0.5f;
	}

	void SpriteEditorPanel::FitCanvasToSlice(const SpriteSlice& slice) {
		if (m_LastCanvasSize.x <= 0.0f || m_LastCanvasSize.y <= 0.0f) return;
		const float padding = 32.0f;
		const float availW  = std::max(1.0f, m_LastCanvasSize.x - padding);
		const float availH  = std::max(1.0f, m_LastCanvasSize.y - padding);
		const float fitZoom = std::min(availW / static_cast<float>(slice.W),
									   availH / static_cast<float>(slice.H));
		m_Zoom = std::clamp(fitZoom, k_MinZoom, k_MaxZoom);
		// Centre the slice (in texture space) inside the canvas.
		const float centreTexX = slice.X + slice.W * 0.5f;
		const float centreTexY = slice.Y + slice.H * 0.5f;
		m_PanOffset.x = m_LastCanvasSize.x * 0.5f - centreTexX * m_Zoom;
		m_PanOffset.y = m_LastCanvasSize.y * 0.5f - centreTexY * m_Zoom;
	}

	bool SpriteEditorPanel::PointInRect(ImVec2 pt, ImVec2 rectMin, ImVec2 rectMax) const {
		return pt.x >= rectMin.x && pt.x <= rectMax.x && pt.y >= rectMin.y && pt.y <= rectMax.y;
	}

	int SpriteEditorPanel::HitTestSlice(ImVec2 mouseTexPx, Handle& outHandle) const {
		outHandle = Handle::None;
		// Handle hit-tests first on the primary-selected slice — its
		// handles take priority over neighbour bodies so the user can
		// shrink one slice into another's bounds.
		if (!m_Selection.empty()) {
			const int idx = m_Selection.front();
			if (idx >= 0 && idx < static_cast<int>(m_Slices.size())) {
				const SpriteSlice& s = m_Slices[idx];
				const Handle h = HitRectHandle(s.X, s.Y, s.W, s.H, mouseTexPx);
				if (h != Handle::None) {
					outHandle = h;
					return idx;
				}
			}
		}

		// Body hits: walk slices in REVERSE order so the visually-topmost
		// one (last drawn) takes the click on overlap.
		for (int i = static_cast<int>(m_Slices.size()) - 1; i >= 0; --i) {
			const SpriteSlice& s = m_Slices[i];
			if (mouseTexPx.x >= s.X && mouseTexPx.x <= s.X + s.W
				&& mouseTexPx.y >= s.Y && mouseTexPx.y <= s.Y + s.H)
			{
				outHandle = Handle::None;
				return i;
			}
		}
		return -1;
	}

	// ─────────────────────────────────────────────────────────────────────
	// Selection + list helpers
	// ─────────────────────────────────────────────────────────────────────

	std::string SpriteEditorPanel::MakeUniqueName(std::string baseName) const {
		if (baseName.empty()) baseName = "slice";
		// Strip a trailing `_N` so "slice_3" rolls into the next collision
		// as "slice_4" rather than "slice_3_2".
		auto findExisting = [&](const std::string& candidate) -> bool {
			for (const SpriteSlice& s : m_Slices) {
				if (s.Name == candidate) return true;
			}
			return false;
		};
		if (!findExisting(baseName)) return baseName;
		for (int n = 2; n < 10000; ++n) {
			std::string candidate = baseName + "_" + std::to_string(n);
			if (!findExisting(candidate)) return candidate;
		}
		return baseName + "_dup";
	}

	void SpriteEditorPanel::AppendUniqueSlice(SpriteSlice slice) {
		if (slice.Name.empty()) slice.Name = "slice";
		slice.Name = MakeUniqueName(std::move(slice.Name));
		m_Slices.push_back(std::move(slice));
	}

	void SpriteEditorPanel::DeleteSelected() {
		if (m_Selection.empty()) return;
		std::vector<int> sorted = m_Selection;
		std::sort(sorted.begin(), sorted.end(), std::greater<int>());
		for (int idx : sorted) {
			if (idx >= 0 && idx < static_cast<int>(m_Slices.size())) {
				m_Slices.erase(m_Slices.begin() + idx);
			}
		}
		m_Selection.clear();
		m_Dirty = true;
	}

	void SpriteEditorPanel::DuplicateSelected() {
		if (m_Selection.empty()) return;
		std::vector<int> newIndices;
		newIndices.reserve(m_Selection.size());
		for (int idx : m_Selection) {
			if (idx < 0 || idx >= static_cast<int>(m_Slices.size())) continue;
			SpriteSlice copy = m_Slices[idx];
			copy.X = std::clamp(copy.X + 1, 0, std::max(0, m_TexWidth  - copy.W));
			copy.Y = std::clamp(copy.Y + 1, 0, std::max(0, m_TexHeight - copy.H));
			AppendUniqueSlice(std::move(copy));
			newIndices.push_back(static_cast<int>(m_Slices.size() - 1));
		}
		m_Selection = std::move(newIndices);
		m_Dirty = true;
	}

	void SpriteEditorPanel::SelectOnly(int index) {
		m_Selection.clear();
		if (index >= 0 && index < static_cast<int>(m_Slices.size())) {
			m_Selection.push_back(index);
		}
	}

	void SpriteEditorPanel::ToggleSelected(int index) {
		auto it = std::find(m_Selection.begin(), m_Selection.end(), index);
		if (it == m_Selection.end()) {
			m_Selection.push_back(index);
		}
		else {
			m_Selection.erase(it);
		}
	}

	bool SpriteEditorPanel::IsSelected(int index) const {
		return std::find(m_Selection.begin(), m_Selection.end(), index) != m_Selection.end();
	}

	// ─────────────────────────────────────────────────────────────────────
	// Persistence
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::Apply() {
		if (m_AssetPath.empty()) return;
		TextureMeta meta = AssetRegistry::ReadTextureMeta(m_AssetPath);
		meta.Sprites = m_Slices;
		meta.Crop    = m_Crop;
		if (!AssetRegistry::WriteTextureMeta(m_AssetPath, meta)) {
			IDX_ERROR_TAG("SpriteEditor", "Failed to write meta for '{}' — slice edits not persisted.", m_AssetPath);
			return;
		}
		m_Dirty = false;
		// Bumping the epoch lazily invalidates cached SpriteUVResolver entries so SpriteRenderers see the new rects next frame.
		NotifySpriteSliceEpochBumped();
	}

	void SpriteEditorPanel::Revert() {
		if (m_AssetPath.empty()) return;
		const TextureMeta meta = AssetRegistry::ReadTextureMeta(m_AssetPath);
		m_Slices = meta.Sprites;
		m_Crop   = meta.Crop;
		m_Selection.clear();
		m_CropDragActive = false;
		m_Dirty = false;
	}

	// ─────────────────────────────────────────────────────────────────────
	// stb_image re-decode for Auto-slice
	// ─────────────────────────────────────────────────────────────────────

	void SpriteEditorPanel::EnsureDecodedBitmap() {
		std::error_code ec;
		std::filesystem::file_time_type mtime{};
		if (std::filesystem::exists(m_AssetPath, ec) && !ec) {
			mtime = std::filesystem::last_write_time(m_AssetPath, ec);
		}
		if (!m_DecodedPixels.empty() && mtime == m_DecodedMtime) {
			return;
		}

		// Route through Texture2D::DecodeFileToCpu — stbi_* symbols are file-static in the engine TU, not linkable directly from the editor DLL.
		auto image = Texture2D::DecodeFileToCpu(m_AssetPath.c_str(), /*flipVertical=*/false);
		if (!image || image->Width <= 0 || image->Height <= 0 || !image->Pixels) {
			IDX_WARN_TAG("SpriteEditor", "Failed to decode '{}' for auto-slice.", m_AssetPath);
			m_DecodedPixels.clear();
			return;
		}
		const std::size_t byteCount = static_cast<std::size_t>(image->Width) * image->Height * 4u;
		m_DecodedPixels.assign(image->Pixels, image->Pixels + byteCount);
		m_DecodedMtime = mtime;
		m_TexWidth  = image->Width;
		m_TexHeight = image->Height;
	}

} // namespace Index
