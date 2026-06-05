#pragma once

#include "Assets/TextureMeta.hpp"
#include "Graphics/TextureHandle.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

namespace Index {

	class SpriteEditorPanel {
	public:
		void Initialize();
		void Render(bool* pOpen);
		void Shutdown();

		void OpenTexture(const std::string& assetPath, uint64_t assetId);

		bool HasUnsavedChanges() const { return m_Dirty; }
		const std::string& GetOpenAssetPath() const { return m_AssetPath; }

	private:
		enum class SliceMode { Manual, GridBySize, GridByCount, Auto };
		enum class DragMode  { None, MoveBody, ResizeEdge, ResizeCorner };

		// Slices = cut the sheet into named sprites; Crop = trim the lone
		// "whole texture" sprite (applies when a renderer leaves SpriteName empty).
		enum class EditMode  { Slices, Crop };

		// One screen-space handle position around a selected slice — 4
		// corners + 4 edge midpoints.
		enum class Handle : int {
			None       = 0,
			TopLeft    = 1, TopRight    = 2,
			BottomLeft = 3, BottomRight = 4,
			Top        = 5, Bottom      = 6,
			Left       = 7, Right       = 8,
		};

		// ── Render passes (called from Render) ─────────────────────────
		void RenderToolbar();
		void RenderCanvas();
		void RenderSliceList();
		void RenderCropPanel();
		void RenderSliceModePopup();
		void RenderUnsavedPrompt();

		// ── Slice algorithms ───────────────────────────────────────────
		// `keepExisting=false` clears the staged slice list before emitting.
		void SliceByGridSize(int cellW, int cellH, int padX, int padY,
		                     int offsetX, int offsetY, bool keepExisting);
		void SliceByGridCount(int cols, int rows, int padX, int padY,
		                      bool keepExisting);
		// Flood-fill BFS over the CPU bitmap. `alphaThreshold` of 0 treats
		// any non-zero alpha as opaque; raise to ignore anti-alias halos.
		void SliceAuto(uint8_t alphaThreshold, int minSize, bool keepExisting);

		// ── Canvas helpers ─────────────────────────────────────────────
		ImVec2 TextureToScreen(ImVec2 texPx) const;
		ImVec2 ScreenToTexture(ImVec2 screenPx) const;
		void   FitCanvasToSlice(const SpriteSlice& slice);
		void   ZoomToFitTexture();
		int    HitTestSlice(ImVec2 mouseTexPx, Handle& outHandle) const;
		bool   PointInRect(ImVec2 pt, ImVec2 rectMin, ImVec2 rectMax) const;
		// Which screen-space handle (corner/edge) of a tex-space rect the
		// cursor is over, or Handle::None. Shared by slice + crop hit-tests.
		Handle HitRectHandle(int x, int y, int w, int h, ImVec2 mouseTexPx) const;
		// Move/resize a rect by a pixel delta per the active drag handle; clamps to texture bounds.
		void   ApplyHandleDrag(int& X, int& Y, int& W, int& H,
		                       int startX, int startY, int startW, int startH,
		                       int dx, int dy) const;

		// ── Crop mode ──────────────────────────────────────────────────
		bool HitTestCropRect(ImVec2 mouseTexPx, Handle& outHandle) const;
		void DrawCropOverlay(ImDrawList* drawList);
		void HandleCropInteraction(bool hovered, const ImGuiIO& io, ImVec2 mouseTexPx);

		// ── Staged-list helpers ────────────────────────────────────────
		std::string MakeUniqueName(std::string baseName) const;
		void        AppendUniqueSlice(SpriteSlice slice);
		void        DeleteSelected();
		void        DuplicateSelected();
		void        SelectOnly(int index);
		void        ToggleSelected(int index);
		bool        IsSelected(int index) const;

		// ── Persistence ────────────────────────────────────────────────
		void Apply();   // commit m_Slices to disk via AssetRegistry::WriteTextureMeta
		void Revert();  // re-read m_Slices from disk, drop staged edits

		// ── CPU bitmap (lazy via stb_image, cached on the panel) ───────
		void EnsureDecodedBitmap();
		bool BitmapAvailable() const { return !m_DecodedPixels.empty(); }

		// ── State ──────────────────────────────────────────────────────
		std::string   m_AssetPath;
		uint64_t      m_AssetId      = 0;
		TextureHandle m_PreviewHandle;
		int           m_TexWidth     = 0;
		int           m_TexHeight    = 0;

		std::vector<SpriteSlice> m_Slices;       // staged edits (not on disk)
		std::vector<int>         m_Selection;    // indices into m_Slices
		bool                     m_Dirty = false;

		EditMode    m_EditMode = EditMode::Slices;
		TextureCrop m_Crop;                      // staged per-texture crop

		ImVec2 m_PanOffset{ 0.0f, 0.0f };
		float  m_Zoom            = 1.0f;
		bool   m_PanInitialized  = false;
		ImVec2 m_LastCanvasSize{ 0.0f, 0.0f };

		// Manual draw-out state.
		bool   m_IsDrawing       = false;
		ImVec2 m_DrawStartTexPx{ 0.0f, 0.0f };

		// Drag-existing-slice state.
		DragMode    m_DragMode        = DragMode::None;
		int         m_DragSliceIndex  = -1;
		Handle      m_DragHandle      = Handle::None;
		ImVec2      m_DragStartMouseTexPx{ 0.0f, 0.0f };
		SpriteSlice m_DragStartSliceState{};

		// Drag-crop state (Crop mode).
		bool        m_CropDragActive  = false;
		TextureCrop m_DragStartCrop{};

		// Slice-mode form values (live across sessions for the lifetime of
		// the editor process; not persisted to project settings yet).
		SliceMode m_SliceMode = SliceMode::GridBySize;
		int   m_CellW          = 32;
		int   m_CellH          = 32;
		int   m_GridCols       = 8;
		int   m_GridRows       = 8;
		int   m_PaddingX       = 0;
		int   m_PaddingY       = 0;
		int   m_OffsetX        = 0;
		int   m_OffsetY        = 0;
		int   m_AutoSliceAlphaThreshold = 1;   // 0..255
		int   m_AutoSliceMinSize        = 2;   // px; reject sub-2x2 noise
		bool  m_KeepExistingOnSlice     = false;
		bool  m_OpenSlicePopup          = false;

		std::string m_PendingAssetPath;
		uint64_t    m_PendingAssetId = 0;
		bool        m_ShowUnsavedPrompt = false;

		// Re-decoded CPU bitmap (RGBA8, w*h*4 bytes). Lifetime = current
		// OpenTexture; freed and re-populated on each swap.
		std::vector<uint8_t>      m_DecodedPixels;
		std::filesystem::file_time_type m_DecodedMtime{};
	};

} // namespace Index
