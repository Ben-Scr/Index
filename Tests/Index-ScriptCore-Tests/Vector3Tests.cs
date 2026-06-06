using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class Vector3Tests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector3(2f);
        Assert.Equal(2f, s.X);
        Assert.Equal(2f, s.Y);
        Assert.Equal(2f, s.Z);

        var v = new Vector3(3f, 4f, 5f);
        Assert.Equal(3f, v.X);
        Assert.Equal(4f, v.Y);
        Assert.Equal(5f, v.Z);
    }

    [Fact]
    public void Operators_AddSubtractScaleNegateDivide()
    {
        var a = new Vector3(1f, 2f, 3f);
        var b = new Vector3(3f, 5f, 7f);

        Assert.Equal(new Vector3(4f, 7f, 10f), a + b);
        Assert.Equal(new Vector3(-2f, -3f, -4f), a - b);
        Assert.Equal(new Vector3(3f, 10f, 21f), a * b);
        Assert.Equal(new Vector3(2f, 4f, 6f), a * 2f);
        Assert.Equal(new Vector3(2f, 4f, 6f), 2f * a);
        Assert.Equal(new Vector3(-1f, -2f, -3f), -a);
        Assert.Equal(new Vector3(0.5f, 1f, 1.5f), a / 2f);
    }

    [Fact]
    public void Length_OfTwoThreeSix_IsSeven()
    {
        var v = new Vector3(2f, 3f, 6f);
        Assert.Equal(49f, v.LengthSquared());
        Assert.Equal(7.0, v.Length(), 4);
    }

    [Fact]
    public void Normalized_HasUnitLength()
    {
        var n = new Vector3(2f, 3f, 6f).Normalized();
        Assert.Equal(1.0, n.Length(), 4);
    }

    [Fact]
    public void Normalized_OfZero_ReturnsZero()
    {
        // Below Mathf.Epsilon the method must return Zero, not NaN.
        Assert.Equal(Vector3.Zero, Vector3.Zero.Normalized());
    }

    [Fact]
    public void Dot_And_Distance()
    {
        Assert.Equal(32f, Vector3.Dot(new Vector3(1f, 2f, 3f), new Vector3(3f, 4f, 7f)));
        Assert.Equal(7.0, Vector3.Distance(Vector3.Zero, new Vector3(2f, 3f, 6f)), 4);
    }

    [Fact]
    public void Cross_OfBasisVectors()
    {
        // Right x Up = Forward's opposite (Z+), and is right-handed.
        Assert.Equal(new Vector3(0f, 0f, 1f), Vector3.Cross(Vector3.Right, Vector3.Up));
        // Up x Right = Z-.
        Assert.Equal(new Vector3(0f, 0f, -1f), Vector3.Cross(Vector3.Up, Vector3.Right));
        // Cross of parallel vectors is Zero.
        Assert.Equal(Vector3.Zero, Vector3.Cross(Vector3.Right, Vector3.Right));
    }

    [Fact]
    public void Lerp_ClampsParameter()
    {
        var a = new Vector3(0f, 0f, 0f);
        var b = new Vector3(10f, 20f, 30f);

        Assert.Equal(new Vector3(5f, 10f, 15f), Vector3.Lerp(a, b, 0.5f));
        Assert.Equal(a, Vector3.Lerp(a, b, -1f)); // t clamped to 0
        Assert.Equal(b, Vector3.Lerp(a, b, 2f));  // t clamped to 1
    }

    [Fact]
    public void Statics_HaveExpectedValues()
    {
        Assert.Equal(new Vector3(0f, 0f, 0f), Vector3.Zero);
        Assert.Equal(new Vector3(1f, 1f, 1f), Vector3.One);
        Assert.Equal(new Vector3(1f, 0f, 0f), Vector3.Right);
        Assert.Equal(new Vector3(-1f, 0f, 0f), Vector3.Left);
        Assert.Equal(new Vector3(0f, 1f, 0f), Vector3.Up);
        Assert.Equal(new Vector3(0f, -1f, 0f), Vector3.Down);
        Assert.Equal(new Vector3(0f, 0f, -1f), Vector3.Forward);
        Assert.Equal(new Vector3(0f, 0f, 1f), Vector3.Back);
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector3(1f, 1f, 1f) == Vector3.One);
        Assert.True(new Vector3(1f, 0f, 1f) != Vector3.One);
        Assert.Equal(Vector3.One.GetHashCode(), new Vector3(1f, 1f, 1f).GetHashCode());
    }
}
