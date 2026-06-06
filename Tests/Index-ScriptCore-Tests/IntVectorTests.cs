using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class Vector2IntTests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector2Int(2);
        Assert.Equal(2, s.X);
        Assert.Equal(2, s.Y);

        var v = new Vector2Int(3, 4);
        Assert.Equal(3, v.X);
        Assert.Equal(4, v.Y);

        // From a Vector3Int: drops Z.
        var fromV3 = new Vector2Int(new Vector3Int(5, 6, 7));
        Assert.Equal(5, fromV3.X);
        Assert.Equal(6, fromV3.Y);
    }

    [Fact]
    public void Statics_ZeroOneAndDirections()
    {
        Assert.Equal(new Vector2Int(0, 0), Vector2Int.Zero);
        Assert.Equal(new Vector2Int(1, 1), Vector2Int.One);
        Assert.Equal(new Vector2Int(1, 0), Vector2Int.Right);
        Assert.Equal(new Vector2Int(-1, 0), Vector2Int.Left);
        Assert.Equal(new Vector2Int(0, 1), Vector2Int.Up);
        Assert.Equal(new Vector2Int(0, -1), Vector2Int.Down);
    }

    [Fact]
    public void Operators_AddSubtractMultiplyDivideNegate()
    {
        var a = new Vector2Int(6, 8);
        var b = new Vector2Int(2, 4);

        Assert.Equal(new Vector2Int(8, 12), a + b);
        Assert.Equal(new Vector2Int(4, 4), a - b);
        Assert.Equal(new Vector2Int(12, 32), a * b);
        Assert.Equal(new Vector2Int(3, 2), a / b);
        Assert.Equal(new Vector2Int(-6, -8), -a);
    }

    [Fact]
    public void Operators_ScalarMultiplyAndDivide()
    {
        var v = new Vector2Int(4, 6);

        Assert.Equal(new Vector2Int(8, 12), v * 2);
        Assert.Equal(new Vector2Int(8, 12), 2 * v);
        Assert.Equal(new Vector2Int(2, 3), v / 2);
        // Integer division truncates toward zero.
        Assert.Equal(new Vector2Int(2, 3), new Vector2Int(5, 7) / 2);
    }

    [Fact]
    public void LengthSquared_IsIntegerExact()
    {
        Assert.Equal(25, new Vector2Int(3, 4).LengthSquared());
    }

    [Fact]
    public void DotProduct()
    {
        Assert.Equal(11, Vector2Int.Dot(new Vector2Int(1, 2), new Vector2Int(3, 4)));
    }

    [Fact]
    public void MinMax_Componentwise()
    {
        var a = new Vector2Int(1, 8);
        var b = new Vector2Int(5, 3);
        Assert.Equal(new Vector2Int(1, 3), Vector2Int.Min(a, b));
        Assert.Equal(new Vector2Int(5, 8), Vector2Int.Max(a, b));
    }

    [Fact]
    public void Clamp_BoundsEachComponent()
    {
        var v = new Vector2Int(-5, 20);
        v.Clamp(new Vector2Int(0, 0), new Vector2Int(10, 10));
        Assert.Equal(new Vector2Int(0, 10), v);
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector2Int(1, 1) == Vector2Int.One);
        Assert.True(new Vector2Int(1, 0) != Vector2Int.One);
        Assert.True(new Vector2Int(2, 3).Equals(new Vector2Int(2, 3)));
        Assert.False(new Vector2Int(2, 3).Equals(new Vector2Int(3, 2)));
        Assert.Equal(Vector2Int.One.GetHashCode(), new Vector2Int(1, 1).GetHashCode());
    }

    [Fact]
    public void ImplicitConversion_ToVector2()
    {
        Vector2 f = new Vector2Int(3, 4);
        Assert.Equal(3f, f.X);
        Assert.Equal(4f, f.Y);
    }

    [Fact]
    public void ExplicitConversion_FromVector2_Truncates()
    {
        var i = (Vector2Int)new Vector2(3.9f, -2.5f);
        Assert.Equal(3, i.X);
        Assert.Equal(-2, i.Y);
    }
}

public class Vector3IntTests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector3Int(2);
        Assert.Equal(2, s.X);
        Assert.Equal(2, s.Y);
        Assert.Equal(2, s.Z);

        var v = new Vector3Int(3, 4, 5);
        Assert.Equal(3, v.X);
        Assert.Equal(4, v.Y);
        Assert.Equal(5, v.Z);

        var fromV2 = new Vector3Int(new Vector2Int(7, 8), 9);
        Assert.Equal(7, fromV2.X);
        Assert.Equal(8, fromV2.Y);
        Assert.Equal(9, fromV2.Z);
    }

    [Fact]
    public void Statics_ZeroOneAndDirections()
    {
        Assert.Equal(new Vector3Int(0, 0, 0), Vector3Int.Zero);
        Assert.Equal(new Vector3Int(1, 1, 1), Vector3Int.One);
        Assert.Equal(new Vector3Int(1, 0, 0), Vector3Int.Right);
        Assert.Equal(new Vector3Int(-1, 0, 0), Vector3Int.Left);
        Assert.Equal(new Vector3Int(0, 1, 0), Vector3Int.Up);
        Assert.Equal(new Vector3Int(0, -1, 0), Vector3Int.Down);
        Assert.Equal(new Vector3Int(0, 0, -1), Vector3Int.Forward);
        Assert.Equal(new Vector3Int(0, 0, 1), Vector3Int.Back);
    }

    [Fact]
    public void XY_Property_DropsZ()
    {
        Assert.Equal(new Vector2Int(3, 4), new Vector3Int(3, 4, 5).XY);
    }

    [Fact]
    public void Operators_AddSubtractMultiplyDivideNegate()
    {
        var a = new Vector3Int(6, 8, 12);
        var b = new Vector3Int(2, 4, 3);

        Assert.Equal(new Vector3Int(8, 12, 15), a + b);
        Assert.Equal(new Vector3Int(4, 4, 9), a - b);
        Assert.Equal(new Vector3Int(12, 32, 36), a * b);
        Assert.Equal(new Vector3Int(3, 2, 4), a / b);
        Assert.Equal(new Vector3Int(-6, -8, -12), -a);
    }

    [Fact]
    public void Operators_ScalarMultiplyAndDivide()
    {
        var v = new Vector3Int(4, 6, 8);
        Assert.Equal(new Vector3Int(8, 12, 16), v * 2);
        Assert.Equal(new Vector3Int(8, 12, 16), 2 * v);
        Assert.Equal(new Vector3Int(2, 3, 4), v / 2);
    }

    [Fact]
    public void LengthSquared_IsIntegerExact()
    {
        Assert.Equal(14, new Vector3Int(1, 2, 3).LengthSquared());
    }

    [Fact]
    public void DotProduct()
    {
        Assert.Equal(32, Vector3Int.Dot(new Vector3Int(1, 2, 3), new Vector3Int(4, 5, 6)));
    }

    [Fact]
    public void Cross_OfUnitAxes()
    {
        // X cross Y == Z for a right-handed cross product.
        Assert.Equal(new Vector3Int(0, 0, 1),
            Vector3Int.Cross(new Vector3Int(1, 0, 0), new Vector3Int(0, 1, 0)));
    }

    [Fact]
    public void MinMax_Componentwise()
    {
        var a = new Vector3Int(1, 8, 4);
        var b = new Vector3Int(5, 3, 9);
        Assert.Equal(new Vector3Int(1, 3, 4), Vector3Int.Min(a, b));
        Assert.Equal(new Vector3Int(5, 8, 9), Vector3Int.Max(a, b));
    }

    [Fact]
    public void Clamp_BoundsEachComponent()
    {
        var v = new Vector3Int(-5, 20, 5);
        v.Clamp(new Vector3Int(0, 0, 0), new Vector3Int(10, 10, 10));
        Assert.Equal(new Vector3Int(0, 10, 5), v);
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector3Int(1, 1, 1) == Vector3Int.One);
        Assert.True(new Vector3Int(1, 1, 0) != Vector3Int.One);
        Assert.True(new Vector3Int(2, 3, 4).Equals(new Vector3Int(2, 3, 4)));
        Assert.False(new Vector3Int(2, 3, 4).Equals(new Vector3Int(2, 3, 5)));
        Assert.Equal(Vector3Int.One.GetHashCode(), new Vector3Int(1, 1, 1).GetHashCode());
    }

    [Fact]
    public void ImplicitConversion_ToVector3()
    {
        Vector3 f = new Vector3Int(3, 4, 5);
        Assert.Equal(3f, f.X);
        Assert.Equal(4f, f.Y);
        Assert.Equal(5f, f.Z);
    }

    [Fact]
    public void ExplicitConversion_FromVector3_Truncates()
    {
        var i = (Vector3Int)new Vector3(3.9f, -2.5f, 1.2f);
        Assert.Equal(3, i.X);
        Assert.Equal(-2, i.Y);
        Assert.Equal(1, i.Z);
    }

    [Fact]
    public void ExplicitConversion_FromVector2Int_AddsZeroZ()
    {
        var i = (Vector3Int)new Vector2Int(7, 8);
        Assert.Equal(new Vector3Int(7, 8, 0), i);
    }
}

public class Vector4IntTests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector4Int(2);
        Assert.Equal(2, s.X);
        Assert.Equal(2, s.Y);
        Assert.Equal(2, s.Z);
        Assert.Equal(2, s.W);

        var v = new Vector4Int(3, 4, 5, 6);
        Assert.Equal(3, v.X);
        Assert.Equal(4, v.Y);
        Assert.Equal(5, v.Z);
        Assert.Equal(6, v.W);

        var fromV3 = new Vector4Int(new Vector3Int(1, 2, 3), 4);
        Assert.Equal(new Vector4Int(1, 2, 3, 4), fromV3);

        var fromV2 = new Vector4Int(new Vector2Int(1, 2), 3, 4);
        Assert.Equal(new Vector4Int(1, 2, 3, 4), fromV2);
    }

    [Fact]
    public void Statics_ZeroAndOne()
    {
        Assert.Equal(new Vector4Int(0, 0, 0, 0), Vector4Int.Zero);
        Assert.Equal(new Vector4Int(1, 1, 1, 1), Vector4Int.One);
    }

    [Fact]
    public void Swizzle_Properties()
    {
        var v = new Vector4Int(3, 4, 5, 6);
        Assert.Equal(new Vector2Int(3, 4), v.XY);
        Assert.Equal(new Vector3Int(3, 4, 5), v.XYZ);
    }

    [Fact]
    public void Operators_AddSubtractMultiplyDivideNegate()
    {
        var a = new Vector4Int(6, 8, 12, 10);
        var b = new Vector4Int(2, 4, 3, 5);

        Assert.Equal(new Vector4Int(8, 12, 15, 15), a + b);
        Assert.Equal(new Vector4Int(4, 4, 9, 5), a - b);
        Assert.Equal(new Vector4Int(12, 32, 36, 50), a * b);
        Assert.Equal(new Vector4Int(3, 2, 4, 2), a / b);
        Assert.Equal(new Vector4Int(-6, -8, -12, -10), -a);
    }

    [Fact]
    public void Operators_ScalarMultiplyAndDivide()
    {
        var v = new Vector4Int(4, 6, 8, 10);
        Assert.Equal(new Vector4Int(8, 12, 16, 20), v * 2);
        Assert.Equal(new Vector4Int(8, 12, 16, 20), 2 * v);
        Assert.Equal(new Vector4Int(2, 3, 4, 5), v / 2);
    }

    [Fact]
    public void LengthSquared_IsIntegerExact()
    {
        Assert.Equal(30, new Vector4Int(1, 2, 3, 4).LengthSquared());
    }

    [Fact]
    public void DotProduct()
    {
        Assert.Equal(70, Vector4Int.Dot(new Vector4Int(1, 2, 3, 4), new Vector4Int(5, 6, 7, 8)));
    }

    [Fact]
    public void MinMax_Componentwise()
    {
        var a = new Vector4Int(1, 8, 4, 6);
        var b = new Vector4Int(5, 3, 9, 2);
        Assert.Equal(new Vector4Int(1, 3, 4, 2), Vector4Int.Min(a, b));
        Assert.Equal(new Vector4Int(5, 8, 9, 6), Vector4Int.Max(a, b));
    }

    [Fact]
    public void Clamp_BoundsEachComponent()
    {
        var v = new Vector4Int(-5, 20, 5, 100);
        v.Clamp(new Vector4Int(0, 0, 0, 0), new Vector4Int(10, 10, 10, 10));
        Assert.Equal(new Vector4Int(0, 10, 5, 10), v);
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector4Int(1, 1, 1, 1) == Vector4Int.One);
        Assert.True(new Vector4Int(1, 1, 1, 0) != Vector4Int.One);
        Assert.True(new Vector4Int(2, 3, 4, 5).Equals(new Vector4Int(2, 3, 4, 5)));
        Assert.False(new Vector4Int(2, 3, 4, 5).Equals(new Vector4Int(2, 3, 4, 6)));
        Assert.Equal(Vector4Int.One.GetHashCode(), new Vector4Int(1, 1, 1, 1).GetHashCode());
    }

    [Fact]
    public void ImplicitConversion_ToVector4()
    {
        Vector4 f = new Vector4Int(3, 4, 5, 6);
        Assert.Equal(3f, f.X);
        Assert.Equal(4f, f.Y);
        Assert.Equal(5f, f.Z);
        Assert.Equal(6f, f.W);
    }

    [Fact]
    public void ExplicitConversion_FromVector4_Truncates()
    {
        var i = (Vector4Int)new Vector4(3.9f, -2.5f, 1.2f, 9.9f);
        Assert.Equal(3, i.X);
        Assert.Equal(-2, i.Y);
        Assert.Equal(1, i.Z);
        Assert.Equal(9, i.W);
    }

    [Fact]
    public void ExplicitConversion_FromVector3Int_AddsZeroW()
    {
        var i = (Vector4Int)new Vector3Int(7, 8, 9);
        Assert.Equal(new Vector4Int(7, 8, 9, 0), i);
    }
}
