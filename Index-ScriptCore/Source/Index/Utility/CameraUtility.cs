
namespace Index;

public class A
{
    public void P()
    {
        // ...
    }

    public void Q()
    {
        // ...
    }

    public virtual void R()
    {
        // ...
    }

    public virtual void S()
    {
        // ...
    }

    public virtual void T()
    {
        // ...
    }

    public virtual void U()
    {
        // ...
    }
}

public class B : A
{
    public new void P()
    {

    }


}

public static class CameraUtility
{
    public static Vector2 GetMousePosition2D()
    {
        Camera2D? mainCamera = Camera2D.Main;

        if (mainCamera == null) throw new NullReferenceException("Main Camera doesn't exist");

        return mainCamera.ScreenToWorld(Input.MousePosition);
    }
    public static Vector3 GetMousePosition(Camera2D camera)
    {
        if (camera == null) throw new NullReferenceException("Main Camera doesn't exist");

        return camera.ScreenToWorld(Input.MousePosition);
    }
}
