# Assets

This page explains the **whole asset system** of Index: what an asset is, how Index keeps track of them, how you reference them from components and scripts, and how they get loaded while your game runs.

If you just want the short version: every file in your project's `Assets` folder is given a permanent ID. Components and scripts store that ID (not the file path), so you can freely move or rename files without breaking anything. The engine loads the actual data on demand and caches it.

---

## What is an asset?

An **asset** is any content file your game uses — a sprite, a sound, a font, a scene, and so on. In Index you don't "import" assets through a wizard; you simply **drop files into your project's `Assets` folder** and the engine discovers them automatically.

Index recognizes these asset kinds (from the `AssetKind` list):

| Kind | Typical file types | Used for |
|------|--------------------|----------|
| **Texture** | `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga` | Sprites and images |
| **Audio** | `.wav`, `.mp3`, `.ogg`, `.flac` | Sound effects and music |
| **Font** | `.ttf`, `.otf` | Text rendering |
| **Scene** | `.scene` | A level / screen of your game |
| **Prefab** | `.prefab` | A reusable, saved entity (with children) |
| **Script** | `.cs` (and C/C++ source) | Game logic |
| **Shader** | `.wgsl`, `.vert`, `.frag`, … | Custom rendering |
| **Other** | anything else | Tracked but not specially handled |

Anything with an unknown extension is still tracked as **Other**, so nothing in your project is invisible to the engine.

---

## How assets are identified: GUIDs and `.meta` files

This is the most important idea in the whole system.

Every asset gets a permanent **GUID** (a 64-bit unique ID, also called an Asset ID). Once a file has a GUID, *that* is how everything refers to it — your scenes, prefabs, components, and scripts all store the GUID, **never the file path**.

Why? Because paths change. If references used paths, renaming `player.png` to `hero.png` would break every sprite using it. With GUIDs, you can rename and move files freely and references keep working.

### The `.meta` file

To make a GUID *stick* to a file, Index writes a small companion file next to it with a `.meta` extension:

```
Assets/
  player.png
  player.png.meta     ← created automatically
```

The `.meta` is a small JSON file that stores:

- the asset's **GUID** (so it stays the same forever), and
- **import settings** for that asset (for a texture: filter mode, wrap mode, and any sprite slices you've defined).

> **Keep `.meta` files.** When you move or copy an asset, move its `.meta` too, and commit them to version control. The `.meta` is what preserves the asset's identity and its import settings. If a `.meta` goes missing, the engine generates a new one — but the new GUID won't match old references.

### Two kinds of GUID

- **Project assets** (your files) get a GUID derived from the file. These live alongside your `.meta` files.
- **Built-in engine assets** (things shipped with Index, like the default font and the default white texture) get fixed, stable GUIDs so they're identical on every machine. The default font, for example, always has the same ID.

---

## How assets are discovered (the "import" step)

There is no manual import button. Discovery is automatic:

1. The engine **scans** your project's `Assets` folder.
2. Each file is **classified** into an `AssetKind` by its extension.
3. If the file already has a `.meta` with a valid GUID, that GUID is used. If not, the engine **generates a GUID and writes a new `.meta`**.

This is handled by the **Asset Registry**, the engine's central index of all known assets. It keeps two lookups — *GUID → file* and *file → GUID* — so it can answer "where is asset X?" and "what is the ID of this file?" instantly.

In the **editor**, the registry watches the `Assets` folder. When you add, delete, or change a file, the registry marks itself dirty and re-scans, so newly added assets show up without restarting.

---

## Referencing assets from components

Built-in components that use an asset store its **GUID**, plus a transient runtime handle that is *not* saved. For example, a sprite component stores:

- `TextureAssetId` — the GUID, written to the `.scene` file. **This is the real reference.**
- a `TextureHandle` — a lightweight runtime handle to the loaded GPU texture. Rebuilt each run, never saved.

The same pattern is used across the engine: audio sources store an audio GUID, text components store a font GUID, and so on.

**When a scene loads**, the deserializer reads the GUID and asks the right manager to load it — for example, "load the texture with this GUID." It resolves the GUID to a file, applies the import settings from the `.meta`, and hands back a handle. (If a GUID can't be found, the engine falls back to an older saved path if one exists, so projects keep working across changes.)

---

## Referencing assets from C# scripts

Scripts use small **reference types** that wrap a GUID, so you can expose an asset slot in the Inspector and assign it by drag-and-drop:

- `TextureRef`, `AudioRef`, `FontRef`, `SceneRef`, … — each holds an asset GUID and knows how to resolve it.

You can also load assets in code through the script `AssetManager`:

```csharp
// Load by path (resolved to a GUID internally)…
Texture tex = AssetManager.Load<Texture>("Sprites/player.png");

// …or directly by GUID.
Audio sfx = Audio.FromAssetUUID(myAudioGuid);
sfx.PlayOneShot();
```

Under the hood these always end up working with GUIDs — the path form is just a convenience that looks up the GUID for you. See [Scripting](../Scripting/Scripting.md) for the full API and the Inspector attributes that create asset slots.

---

## Loading, caching, and handles

You don't load the same file twice. Each asset type has a **manager** (Texture Manager, Audio Manager, Font Manager, Shader Manager) that:

1. **Loads** the data the first time it's requested (decode the PNG, upload it to the GPU, etc.).
2. **Caches** it and returns a small **handle** (a cheap value you pass around instead of the heavy data).
3. **Reuses** the cached copy on every later request for the same asset.

Handles are designed to be safe: if an asset is unloaded and its slot later reused, old handles won't accidentally point at the new asset (each slot carries a generation counter).

Fonts are cached per **pixel size** — text at 16px and 48px are baked into separate GPU textures, because each size needs its own glyphs.

### Hot-reload (editor)

In the editor, changing an asset file on disk updates it live. For example, editing `player.png` in an image editor and saving triggers the Texture Manager to re-decode it and re-upload to the GPU, so the change appears in the editor without a restart. Thumbnails in the Asset Browser refresh the same way.

### Unloading and purging

Managers can free assets that nothing is using anymore. Systems that hold references (scenes, components, plugins) can register as "reference providers," and a `PurgeUnreferenced()` pass unloads anything no provider still needs — keeping memory in check during long sessions.

---

## The Asset Browser (editor)

The **Asset Browser** is the editor panel where you work with assets directly. From it you can:

- Browse folders with a breadcrumb trail and a thumbnail grid.
- **Create** new scenes, prefabs, scripts, and folders.
- **Drag and drop** assets onto components (e.g. drop a texture onto a sprite) or onto the scene.
- Rename, duplicate, delete, copy/paste, and open files in an external editor.
- **Slice sprite sheets** — define multiple sprites inside one texture; the slices are stored in that texture's `.meta`.
- Drop external files into the window to bring them into the project.

See [Editor](../Editor/Editor.md) for how the Asset Browser fits with the other panels.

---

## Assets at runtime vs. in the editor

| | Editor | Runtime (shipped game) |
|---|--------|------------------------|
| Where assets come from | The project's `Assets` folder | The `Assets` folder shipped next to the game |
| Discovery | Scanned + watched live | Loaded on demand as scenes deserialize |
| Hot-reload | Yes — edits apply live | No — files are read once |
| Form on disk | Loose files | **Loose files** (no archive/pack step) |

Index ships assets as **loose files** — there is no "cook" or archive step that bundles everything into one package file. When you build a game, the project's assets are copied alongside the runtime executable, and the game reads them from disk as scenes load. (See the Build flow in [Editor](../Editor/Editor.md).)

---

## Summary

- Drop files into `Assets/` — no import wizard.
- Each file gets a permanent **GUID**, stored in a `.meta` file beside it.
- Everything references assets **by GUID**, so you can move/rename freely. Keep the `.meta` files.
- The **Asset Registry** indexes everything; per-type **managers** load, cache, and hand out **handles**.
- The editor hot-reloads changed assets; the runtime reads loose files on demand.

## Related pages

- [Scenes](../Engine-Core/Scenes.md) — scenes and prefabs are assets too, and they store the GUIDs of the assets they use.
- [Scripting](../Scripting/Scripting.md) — loading and referencing assets from C#.
- [Editor](../Editor/Editor.md) — the Asset Browser and the build process.
