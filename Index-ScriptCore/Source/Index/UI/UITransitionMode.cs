namespace Index.UI;

// Mirrors the native UITransitionMode enum (see
// Index-Engine/src/Components/UI/UITransitionMode.hpp). Underlying
// type is byte to match the C++ side; integer values must stay in
// sync because we marshal as int through the binding layer.
public enum UITransitionMode : byte
{
    // Per-state Color writes into the widget's ImageComponent.Color
    // (the original mouse-only behaviour, default for every widget).
    ColorTint  = 0,

    // Per-state UUID writes into the widget's
    // ImageComponent.TextureAssetId / TextureHandle. Slots that are
    // 0 fall back to NormalSprite; if NormalSprite is also 0 the
    // swap is skipped, leaving the authored texture untouched.
    SpriteSwap = 1,

    // No automatic transition. Game code drives the visual change.
    None       = 2,
}
 