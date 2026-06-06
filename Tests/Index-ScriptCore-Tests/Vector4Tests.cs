using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class Vector4Tests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector4(2f);
        Assert.Equal(2f, s.X);
        Assert.Equal(2f, s.Y);
        Assert.Equal(2f, s.Z);
        Assert.Equal(2f, s.W);

        var v = new Vector4(1f, 2f, 3f, 4f);
        Assert.Equal(1f, v.X);
        Assert.Equal(2f, v.Y);
        Assert.Equal(3f, v.Z);
        Assert.Equal(4f, v.W);
    }

    [Fact]
    public void Constructor_FromVector3AndW()
    {
        var v = new Vector4(new Vector3(1f, 2f, 3f), 4f);
        Assert.Equal(new Vector4(1f, 2f, 3f, 4f), v);
    }

    [Fact]
    public void Constructor_FromVector2AndZW()
    {
        var v = new Vector4(new Vector2(1f, 2f), 3f, 4f);
        Assert.Equal(new Vector4(1f, 2f, 3f, 4f), v);
    }

    [Fact]
    public void Swizzle_XY_And_XYZ()
    {
        var v = new Vector4(1f, 2f, 3f, 4f);
        Assert.Equal(new Vector2(1f, 2f), v.XY);
        Assert.Equal(new Vector3(1f, 2f, 3f), v.XYZ);
    }

    [Fact]
    public void Statics_ZeroAndOne()
    {
        Assert.Equal(new Vector4(0f, 0f, 0f, 0f), Vector4.Zero);
        Assert.Equal(new Vector4(1f, 1f, 1f, 1f), Vector4.One);
    }

    [Fact]
    public void Operators_AddSubtractScaleNegateDivide()
    {
        var a = new Vector4(1f, 2f, 3f, 4f);
        var b = new Vector4(5f, 6f, 7f, 8f);

        Assert.Equal(new Vector4(6f, 8f, 10f, 12f), a + b);
        Assert.Equal(new Vector4(-4f, -4f, -4f, -4f), a - b);
        Assert.Equal(new Vector4(2f, 4f, 6f, 8f), a * 2f);
        Assert.Equal(new Vector4(2f, 4f, 6f, 8f), 2f * a);
        Assert.Equal(new Vector4(-1f, -2f, -3f, -4f), -a);
        Assert.Equal(new Vector4(0.5f, 1f, 1.5f, 2f), a / 2f);
    }

    [Fact]
    public void Length_OfTwoTwoTwoTwo_IsFour()
    {
        // sqrt(2^2 * 4) = sqrt(16) = 4
        var v = new Vector4(2f, 2f, 2f, 2f);
        Assert.Equal(4.0, v.Length(), 4);
    }

    [Fact]
    public void Length_OfZero_IsZero()
    {
        Assert.Equal(0f, Vector4.Zero.Length());
    }

    [Fact]
    public void Dot_ComputesComponentProductsSum()
    {
        // 1*5 + 2*6 + 3*7 + 4*8 = 5 + 12 + 21 + 32 = 70
        Assert.Equal(70f, Vector4.Dot(new Vector4(1f, 2f, 3f, 4f), new Vector4(5f, 6f, 7f, 8f)));
    }

    [Fact]
    public void Lerp_ClampsParameter()
    {
        var a = new Vector4(0f, 0f, 0f, 0f);
        var b = new Vector4(10f, 20f, 30f, 40f);

        Assert.Equal(new Vector4(5f, 10f, 15f, 20f), Vector4.Lerp(a, b, 0.5f));
        Assert.Equal(a, Vector4.Lerp(a, b, -1f)); // t clamped to 0
        Assert.Equal(b, Vector4.Lerp(a, b, 2f));  // t clamped to 1
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector4(1f, 1f, 1f, 1f) == Vector4.One);
        Assert.True(new Vector4(1f, 0f, 0f, 0f) != Vector4.One);
        Assert.True(Vector4.One.Equals(new Vector4(1f, 1f, 1f, 1f)));
        Assert.True(Vector4.One.Equals((object)new Vector4(1f, 1f, 1f, 1f)));
        Assert.False(Vector4.One.Equals((object)42));
        Assert.Equal(Vector4.One.GetHashCode(), new Vector4(1f, 1f, 1f, 1f).GetHashCode());
    }
}
