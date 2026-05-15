using Index.UI;

namespace Index;

public static class RectTransformUtility
{
    public static Vector2 GetMouseLocalPoint(RectTransform targetParent)
    {
        return ScreenPointToLocalPointInRectangle(
            targetParent,
            Input.MousePosition
        );
    }

    public static Vector2 ScreenPointToLocalPointInRectangle(
        RectTransform targetParent,
        Vector2 screenPoint
    )
    {
        Vector2 screenSize = GetScreenSize();

        Vector2 canvasLocal = ScreenToCanvasLocal(screenPoint, screenSize);

        Vector2 parentCanvasPosition = GetRectTransformCanvasPosition(targetParent);

        return canvasLocal - parentCanvasPosition;
    } 

    private static Vector2 ScreenToCanvasLocal(Vector2 screenPoint, Vector2 screenSize)
    {
        return new Vector2(
            screenPoint.X - screenSize.X * 0.5f,
            screenSize.Y * 0.5f - screenPoint.Y
        );
    }

    private static Vector2 GetRectTransformCanvasPosition(RectTransform rectTransform)
    {
        return rectTransform.AnchoredPosition;
    }

    private static Vector2 GetScreenSize()
    {
        return new Vector2(1920, 1080);
    }
}