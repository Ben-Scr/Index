namespace Index.UI;

// Underlying type is byte; integer values must stay in sync with the native UITransitionMode enum (marshalled as int through the binding layer).
public enum UITransitionMode : byte
{
    // Per-state Color writes into the widget's ImageComponent.Color,
    // replacing it outright — not a blend / tint. (The original
    // mouse-only behaviour, default for every widget.)
    ColorSwap  = 0,

    // Per-state UUID writes into the widget's
    // ImageComponent.TextureAssetId / TextureHandle. Slots that are
    // 0 fall back to NormalSprite; if NormalSprite is also 0 the
    // swap is skipped, leaving the authored texture untouched.
    SpriteSwap = 1,

    // No automatic transition. Game code drives the visual change.
    None       = 2,
}
 