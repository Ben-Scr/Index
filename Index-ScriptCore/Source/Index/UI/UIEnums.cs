namespace Index.UI;

// Mirrors the native enums in Index-Engine/src/Components/UI/. Integer
// values must stay in sync — they cross the bridge as int through the
// binding layer.

// 9-cell alignment grid used by HorizontalLayoutGroup, VerticalLayoutGroup,
// and GridLayoutGroup. See Index-Engine/src/Components/UI/UIAlignment.hpp.
public enum UIAlignment : int
{
    UpperLeft   = 0,
    UpperCenter = 1,
    UpperRight  = 2,
    MiddleLeft  = 3,
    MiddleCenter = 4,
    MiddleRight = 5,
    LowerLeft   = 6,
    LowerCenter = 7,
    LowerRight  = 8,
}

// Direction the scrollbar's Value axis runs along. See
// Index-Engine/src/Components/UI/ScrollbarComponent.hpp.
public enum ScrollbarDirection : int
{
    LeftToRight = 0,
    RightToLeft = 1,
    BottomToTop = 2,
    TopToBottom = 3,
}

// How a ScrollRect's content reacts when dragged past its bounds. See
// Index-Engine/src/Components/UI/ScrollRectComponent.hpp.
public enum ScrollRectMovementType : int
{
    Unrestricted = 0,
    Elastic      = 1,
    Clamped      = 2,
}

// Visibility rule applied to a scrollbar attached to a ScrollRect. See
// Index-Engine/src/Components/UI/ScrollRectComponent.hpp.
public enum ScrollbarVisibility : int
{
    Permanent                 = 0,
    AutoHide                  = 1,
    AutoHideAndExpandViewport = 2,
}

// Which corner of the GridLayoutGroup holds the first child cell. See
// Index-Engine/src/Components/UI/GridLayoutGroupComponent.hpp.
public enum GridLayoutStartCorner : int
{
    UpperLeft  = 0,
    UpperRight = 1,
    LowerLeft  = 2,
    LowerRight = 3,
}

// Which axis the GridLayoutGroup fills first before wrapping. See
// Index-Engine/src/Components/UI/GridLayoutGroupComponent.hpp.
public enum GridLayoutStartAxis : int
{
    Horizontal = 0,
    Vertical   = 1,
}

// How GridLayoutGroup decides its row × column count. See
// Index-Engine/src/Components/UI/GridLayoutGroupComponent.hpp.
public enum GridLayoutConstraint : int
{
    Flexible         = 0,
    FixedColumnCount = 1,
    FixedRowCount    = 2,
}
