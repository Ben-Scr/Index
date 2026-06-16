# Localization

This page explains how Index handles **localization** — translating interface text into multiple languages, choosing a language from a JSON file, switching it at runtime, and downloading languages (and the CJK font) on demand. It also covers the file format, the lookup API, and how to add a language of your own.

If you just want the short version: each language is a JSON file of `key → text`. You look a string up by key with `IDX_TR("some.key")` and set the active language with `Localization::SetLanguage("de")`. Any label that re-reads its key picks up the new language on the next frame, falling back to English for anything it can't find — and to the raw key if even English is missing it, so gaps are visible rather than fatal.

---

## What it is

Index's localization system is a small, self-contained engine subsystem (namespace `Index::Localization`, in `Index-Engine/src/Localization/`) that maps **keys to translated strings**. Each language is a JSON file: a `meta` block describing the language, plus a flat `strings` object of `key → translated text`.

At startup the engine loads the **English fallback table**, scans the languages it can find on disk, and reads the user's saved preference. From then on, a lookup like `Localization::Get("launcher.settings.title")` returns the active language's text for that key — or the English text if the active language is missing it — or, as a last resort, the raw key itself.

English (`en`) is the **built-in fallback**: it is always present, is never downloaded, and is the language every other one falls back to. Other languages ship as bundled JSON files and/or are listed in a manifest for on-demand download.

The bundled localization data ships under `Index-Runtime/IndexAssets/Localization/`, which a post-build step copies next to every runnable binary:

```
IndexAssets/
  Localization/
    en.json          ← English (built-in fallback, always present)
    de.json          ← Deutsch (bundled)
    ja.json          ← Japanese (日本語) (bundled; needs the CJK font)
    manifest.json    ← lists downloadable languages + the CJK font
```

> **Localization drives the Launcher today.** Every Index binary calls `Localization::Initialize()` at startup (it lives in the shared engine entry point), so the service is live everywhere. But the only code that currently *reads* translated strings, switches languages, and pumps the download poll is the **Launcher** — and every translation key is in the `launcher.*` namespace. The Editor and Runtime initialize localization without yet pulling localized text from it. The API below is the general mechanism; wiring it into your own game's UI is up to you.

---

## Supported languages

Index ships three languages out of the box. Two distinctions matter: which ones are **bundled** (work offline, no download) versus listed only for download, and which need the large **CJK font** to render their glyphs.

| Code | Display name | How it ships | Needs CJK font |
| --- | --- | --- | --- |
| **en** | English | Built-in fallback — always present, never downloaded | No |
| **de** | Deutsch | Bundled on disk (also has a download URL for updates) | No |
| **ja** | Japanese (日本語) | Bundled on disk (also has a download URL for updates) | **Yes** |

`en` is special: it is the hard-coded fallback table, it is **not** listed in `manifest.json`, and the language list always sorts it first (the rest sort by display name). `de` and `ja` are bundled so the Launcher works offline; their manifest URLs are an *update* path, not the only way to get them. Only `ja` sets `requiresCjkFont`, because Latin/German glyphs are already in the default UI font but Japanese kana and ideographs are not (see [On-demand downloads and the CJK font](#on-demand-downloads-and-the-cjk-font)).

The shipped `de` and `ja` files are intentionally **partial** — they translate the strings that matter most and lean on English for the rest. That is by design; the fallback chain makes it safe.

---

## The translation file format

A language file has two top-level objects: a `meta` block describing the language, and a `strings` object mapping dotted keys to translated text. Placeholders use `{0}`, `{1}`, … positional markers.

```json
{
  "meta": {
    "code": "de",
    "displayName": "Deutsch",
    "fallback": "en",
    "schemaVersion": 1
  },
  "strings": {
    "launcher.settings.title": "Launcher-Einstellungen",
    "launcher.list.size_prefix": "Größe: {0}",
    "launcher.delete.message": "Projekt '{0}' löschen?"
  }
}
```

| Field | Meaning |
| --- | --- |
| **`meta.code`** | The language code (`en`, `de`, `ja`). This is the identity used everywhere — file lookups, the preference file, `SetLanguage`. |
| **`meta.displayName`** | The human-readable name shown in a language picker (e.g. `"Japanese (日本語)"`). |
| **`meta.fallback`** | The language to fall back to; in practice always `en`. |
| **`meta.schemaVersion`** | Format version (currently `1`). |
| **`strings`** | A flat object of `key → translated string`. Only string-valued members are loaded; anything else is skipped. |

When a file is found, only its `meta` block is parsed up front, so a language picker can be populated cheaply without loading every table. The full `strings` table is loaded lazily the moment that language becomes active. Values are substituted at lookup time with `std::vformat`, so `{0}`/`{1}` map to the arguments you pass to `Localization::Format`.

> **`en.json` is the canonical key set.** English is the fallback for every other language, and missing keys resolve *through* it, so a translation file should use the **same keys** as `en.json` — only the values change. If `de.json` lacks a key, the German UI shows the English string; if English also lacks it, the UI shows the raw key (e.g. `launcher.settings.title`) and the engine logs the miss once. A renamed or mistyped key simply won't be found. Keep `en.json` complete and treat its keys as the source of truth.

---

## Choosing and switching the language

### At startup

`Initialize()` reads the saved preference from `locale.json` (at `%LocalAppData%/Index/locale.json` on Windows). If there is no usable preference — or the saved language is unknown or not actually installed — it falls back to `en` and writes the resolved choice back out. From then on, the active table is whatever the preference says.

### Auto-detecting the system language

`Localization::GetSystemLanguage()` probes the OS UI locale (Windows `GetUserDefaultLocaleName`, or the POSIX `LC_ALL` / `LC_MESSAGES` / `LANG` environment variables), normalizes it to a bare lowercase tag, and returns the matching language code — **but only if that language is already installed**. A system locale that maps to a not-yet-downloaded language quietly returns `en`; it does not auto-trigger a download. The Launcher uses this to offer an "Auto (system)" option that re-pins to the OS language on each launch.

### The fallback chain and missing keys

Every `Get()` lookup walks the same chain:

1. The **active** language's table.
2. The **English** fallback table.
3. On a complete miss: the engine records the key, logs a one-time warning tagged `Localization`, and returns the **raw key string** itself.

That last step means a missing key is never an exception or a blank — it surfaces visibly as the key text (`launcher.settings.title`), which makes gaps easy to spot during development.

### Switching at runtime

`Localization::SetLanguage(code)` does the right thing based on the language's status:

- **Installed** → swaps the active table immediately, persists the new preference, and (for a CJK language) makes sure the CJK font is present — fetching it if not, which can require a restart.
- **Available** (listed in the manifest but not on disk) → records a pending switch and kicks off a download; the switch is applied when the download finishes.
- **Downloading / DownloadFailed**, an unknown code, or the already-current code → no-op.

Switching to an already-installed, non-CJK language is **live**: the active string table is swapped in place, any registered change callbacks fire, and there is no restart. (The CJK font is the one exception — see below.)

---

## On-demand downloads and the CJK font

`manifest.json` (schema version 1) lists the languages and the font that can be fetched at runtime. Each language entry carries a GitHub-release `url`, a `sha256`, a `sizeBytes`, and `requiresCjkFont`. A separate `fonts.cjk` entry describes the shared CJK font — **Noto Sans CJK** (`NotoSansCJK-Regular.ttc`, ~19 MB). The font is large enough that it is *not* bundled; it is downloaded the first time it's needed.

Two download paths exist:

- **A language JSON.** Selecting an **Available** language queues its `<code>.json` for download to `%LocalAppData%/Index/Localization/<code>.json`. (Because `de`/`ja` are bundled, this path is really their *update* path; it mainly matters for any server-only languages added later.)
- **The CJK font.** Whenever a CJK language is activated and the font isn't already on disk, Index queues `NotoSansCJK-Regular.ttc` to `%LocalAppData%/Index/Fonts/NotoSansCJK/NotoSansCJK-Regular.ttc`. This happens even for **Japanese, which ships bundled** — the `ja.json` is already present, but its font is not, so selecting Japanese for the first time still triggers the ~19 MB download.

Downloads run on a single background worker, one job at a time (the actual transfer is delegated to the bundled `Index-PackageTool`). The host loop calls `Localization::Poll()` each frame to finalize a finished job — which rebuilds the language list, marks the language **Installed**, and applies any pending switch. Each file is staged to a temporary sibling and renamed into place only after its byte length (when the manifest gives one) and **SHA-256** (when the manifest gives one) check out — so a failed or tampered download never overwrites the existing file.

> **A CJK language needs a restart the first time.** The UI font atlas is baked once at startup; there is no dynamic glyph loading, so a CJK font downloaded mid-session can't be merged into the live atlas — its characters would render as boxes until the next launch. Because of this, fetching the CJK font sets a **restart-required** flag, and the Launcher shows a "Restart Required" prompt that can relaunch the app. Ordinary (non-CJK) language switches never require a restart.

A note on platforms: SHA-256 verification is implemented with BCrypt on **Windows**; on other platforms the hash check is not yet implemented, so any manifest entry carrying a `sha256` will fail to verify there. In practice the download path is Windows-centric today — which is consistent with localization being a Launcher feature for now.

User-downloaded files always **override** bundled ones on a code collision, so an updated `de.json` pulled into `%LocalAppData%/Index/Localization/` takes precedence over the bundled copy.

---

## Using it from code

The public C++ surface lives in `Index-Engine/src/Localization/Localization.hpp`, all in namespace `Index::Localization`:

| Function | What it does |
| --- | --- |
| **`Get(key)`** | Returns the active translation for `key`, falling back to English, then to the raw key. |
| **`GetFallback(key)`** | Looks up `key` in the **English table only**, ignoring the active language. Used for UI shown *before* a freshly-selected language's font is loaded (e.g. the CJK restart prompt), where localized text would render as missing-glyph boxes. |
| **`Format(key, args...)`** | Looks up `key` and substitutes `{0}`/`{1}`/… with the arguments. Header-only template over `std::vformat`; returns the raw template on a bad format spec rather than throwing. |
| **`FormatFallback(key, args...)`** | The English-fallback counterpart of `Format` (built on `GetFallback`). |
| **`GetAvailableLanguages()`** | The cached list of `LanguageInfo { Code, DisplayName, Status, RequiresCjkFont }`. |
| **`GetCurrentLanguage()`** | The active language code. |
| **`GetSystemLanguage()`** | The OS UI language code, if it's installed; otherwise `"en"`. |
| **`SetLanguage(code)`** | Switches language (or queues a download — see above). |
| **`RequestLanguageDownload(code)`** | Kicks off an async download for an *Available* language (plus the CJK font if required); no-op if a download is already running or the language isn't *Available*. `SetLanguage` calls this for you, so you rarely call it directly. |
| **`GetActiveDownload()`** | A snapshot of any in-flight or recently-finished download (`Code`, `Stage`, `Progress`, `Running`, `Failed`, `RestartRequired`, `Error`) for progress UI. |
| **`Poll()`** | Pumped every frame by the host loop to finalize downloads. **Without a pump, a finished download never transitions** — applies any pending switch and rebuilds the installed list. |
| **`Initialize()`** | Loads the fallback, scans languages, applies the saved preference. Called once at startup by the engine entry point. |
| **`RegisterChangeCallback(fn)` / `UnregisterChangeCallback(h)`** | Run `fn` whenever the active language changes; the handle unregisters it. |
| **`IDX_TR(key)`** | Macro alias for `Get(key)` — the idiomatic way to read a string. |

(`Get`, `GetFallback`, and the rest are `INDEX_API`-exported symbols; `Format` and `FormatFallback` are header-only templates with no exported symbol.)

A typical call site just re-reads its keys every frame, so live language switches are picked up automatically:

```cpp
#include "Localization/Localization.hpp"
using namespace Index;

// A label — re-fetched each frame, so it re-localizes on a language switch.
ImGui::TextUnformatted(IDX_TR("launcher.settings.title").c_str());

// A formatted string with positional args ({0}, {1}).
std::string msg = Localization::Format("launcher.asset_library.engine_too_old",
                                       requiredVersion, currentVersion);

// UI that must stay readable before a new font has loaded → use the English fallback.
ImGui::TextUnformatted(Localization::GetFallback("launcher.settings.language.restart_required").c_str());

// Switch language at runtime (live for an installed, non-CJK language).
if (userPickedGerman)
    Localization::SetLanguage("de");
```

Two things to keep in mind. First, `Get()`/`GetFallback()` return a **reference** into the engine's internal tables; **don't hold that reference across a language change** — switching languages rebuilds the active table and can invalidate it. Copy the string if you need to keep it. Second, the change-callback hook is live (it fires on every switch) but the shipped Launcher doesn't register one — it simply re-queries every label through `IDX_TR` each frame, which is the recommended pattern.

There is **no C# / scripting API for localization yet.** The system is native-only; gameplay scripts can't currently read localized strings through Index's C# layer.

---

## Adding a new language

Adding a language is drop-in — no code changes, no rebuild:

1. **Copy `en.json`** to `<code>.json` (e.g. `fr.json`), using the language's code.
2. **Edit the `meta` block**: set `code` to the same code and `displayName` to the name to show in a picker. Keep `fallback` as `"en"` and `schemaVersion` as `1`.
3. **Translate the `strings` values**, leaving the keys exactly as they are. Any key you omit falls back to English.
4. **Drop the file** into either the bundled folder (`Index-Runtime/IndexAssets/Localization/`) to ship it, or the user folder (`%LocalAppData%/Index/Localization/`) to test it locally. The engine scans both on startup, and **user-dir files override bundled ones** on a matching code.

The new language appears automatically: the scan reads each file's `meta` block, records the language, and the next `GetAvailableLanguages()` includes it with status **Installed**. Languages listed in `manifest.json` but not present on disk show up as **Available** (downloadable) instead.

---

## Summary

- Index localization maps **keys to translated strings**, one **JSON file per language** (a `meta` block plus a flat `strings` object), in namespace `Index::Localization`.
- **English (`en`) is the always-present fallback**; lookups resolve active → English → raw key, so missing keys are visible, never fatal.
- Three languages ship: **en**, **de** (bundled), and **ja** (bundled, needs the **CJK font**); `manifest.json` lists `de`/`ja` and the on-demand ~19 MB Noto Sans CJK font.
- The active language is read from **`%LocalAppData%/Index/locale.json`**, can auto-detect the **system language**, and switches **live** via `SetLanguage` — except the CJK font, which **requires a restart** because the font atlas is baked at startup.
- Read strings with **`IDX_TR(key)`** / `Localization::Get`, use **`GetFallback`** for pre-font-load UI, format with **`Localization::Format(key, args...)`**, and pump **`Poll()`** each frame. Localization is **wired into the Launcher today** and has **no C# API** yet.
- **Add a language** by dropping a `<code>.json` with a `meta` block into the bundled or user `Localization` folder — it auto-appears, with the user dir overriding bundled.

## Related pages

- [Assets](../Assets/Assets.md) — the locale JSON files are content shipped next to each binary, like other engine assets.
- [Startup Processes](Startup-Processes.md) — the engine boot sequence, where `Localization::Initialize()` runs.
- [Third-Party Libraries](../Reference/Third-Party-Libraries.md) — the bundled OFL fonts and the on-demand Noto Sans CJK font.
