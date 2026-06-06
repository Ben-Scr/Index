using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class ColorTests
{
    [Fact]
    public void Constructor_RGB_SetsAlphaToOne()
    {
        var c = new Color(0.2f, 0.4f, 0.6f);
        Assert.Equal(0.2f, c.R);
        Assert.Equal(0.4f, c.G);
        Assert.Equal(0.6f, c.B);
        Assert.Equal(1.0f, c.A);
    }

    [Fact]
    public void Constructor_RGBA_SetsAllChannels()
    {
        var c = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        Assert.Equal(0.1f, c.R);
        Assert.Equal(0.2f, c.G);
        Assert.Equal(0.3f, c.B);
        Assert.Equal(0.4f, c.A);
    }

    [Fact]
    public void XyzwAccessors_AliasRgbaChannels()
    {
        var c = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        Assert.Equal(c.R, c.X);
        Assert.Equal(c.G, c.Y);
        Assert.Equal(c.B, c.Z);
        Assert.Equal(c.A, c.W);

        c.X = 0.9f;
        c.Y = 0.8f;
        c.Z = 0.7f;
        c.W = 0.6f;
        Assert.Equal(0.9f, c.R);
        Assert.Equal(0.8f, c.G);
        Assert.Equal(0.7f, c.B);
        Assert.Equal(0.6f, c.A);
    }

    [Fact]
    public void NamedStatics_HaveExpectedChannels()
    {
        Assert.Equal(new Color(1f, 1f, 1f, 1f), Color.White);
        Assert.Equal(new Color(0f, 0f, 0f, 1f), Color.Black);
        Assert.Equal(new Color(1f, 0f, 0f, 1f), Color.Red);
        Assert.Equal(new Color(0f, 1f, 0f, 1f), Color.Green);
        Assert.Equal(new Color(0f, 0f, 1f, 1f), Color.Blue);
        Assert.Equal(new Color(1f, 1f, 0f, 1f), Color.Yellow);
        Assert.Equal(new Color(0f, 1f, 1f, 1f), Color.Cyan);
        Assert.Equal(new Color(1f, 0f, 1f, 1f), Color.Magenta);
        Assert.Equal(new Color(0.5f, 0.5f, 0.5f, 1f), Color.Gray);
        Assert.Equal(new Color(0f, 0f, 0f, 0f), Color.Clear);
    }

    [Fact]
    public void Operator_Add_AddsAllChannels()
    {
        var sum = new Color(0.1f, 0.2f, 0.3f, 0.4f) + new Color(0.5f, 0.1f, 0.2f, 0.3f);
        Assert.Equal(0.6f, sum.R, 5);
        Assert.Equal(0.3f, sum.G, 5);
        Assert.Equal(0.5f, sum.B, 5);
        Assert.Equal(0.7f, sum.A, 5);
    }

    [Fact]
    public void Operator_Subtract_SubtractsAllChannels()
    {
        var diff = new Color(0.6f, 0.5f, 0.4f, 1.0f) - new Color(0.1f, 0.2f, 0.3f, 0.4f);
        Assert.Equal(0.5f, diff.R, 5);
        Assert.Equal(0.3f, diff.G, 5);
        Assert.Equal(0.1f, diff.B, 5);
        Assert.Equal(0.6f, diff.A, 5);
    }

    [Fact]
    public void Operator_Scale_BothOrders_ScaleAllChannels()
    {
        var c = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        var expected = new Color(0.2f, 0.4f, 0.6f, 0.8f);
        Assert.Equal(expected, c * 2f);
        Assert.Equal(expected, 2f * c);
    }

    [Fact]
    public void ExplicitConversion_ToVector3_DropsAlpha()
    {
        var v = (Vector3)new Color(0.2f, 0.4f, 0.6f, 0.8f);
        Assert.Equal(0.2f, v.X);
        Assert.Equal(0.4f, v.Y);
        Assert.Equal(0.6f, v.Z);
    }

    [Fact]
    public void ExplicitConversion_ToVector2_KeepsRedGreen()
    {
        var v = (Vector2)new Color(0.2f, 0.4f, 0.6f, 0.8f);
        Assert.Equal(0.2f, v.X);
        Assert.Equal(0.4f, v.Y);
    }

    [Fact]
    public void Equality_And_Operators()
    {
        var a = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        var b = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        var c = new Color(0.1f, 0.2f, 0.3f, 0.5f);

        Assert.True(a == b);
        Assert.False(a == c);
        Assert.True(a != c);
        Assert.False(a != b);
        Assert.True(a.Equals(b));
        Assert.False(a.Equals(c));
        Assert.True(a.Equals((object)b));
        Assert.False(a.Equals((object)"not a color"));
    }

    [Fact]
    public void HashCode_MatchesForEqualColors()
    {
        var a = new Color(0.25f, 0.5f, 0.75f, 1.0f);
        var b = new Color(0.25f, 0.5f, 0.75f, 1.0f);
        Assert.Equal(a.GetHashCode(), b.GetHashCode());
    }

    [Fact]
    public void Lerp_Midpoint_InterpolatesEachChannel()
    {
        var mid = Color.Lerp(Color.Black, Color.White, 0.5f);
        Assert.Equal(0.5f, mid.R, 5);
        Assert.Equal(0.5f, mid.G, 5);
        Assert.Equal(0.5f, mid.B, 5);
        Assert.Equal(1.0f, mid.A, 5);
    }

    [Fact]
    public void Lerp_ClampsParameter()
    {
        var a = new Color(0f, 0f, 0f, 0f);
        var b = new Color(1f, 1f, 1f, 1f);

        Assert.Equal(a, Color.Lerp(a, b, -1f)); // t clamped to 0
        Assert.Equal(b, Color.Lerp(a, b, 2f));  // t clamped to 1
    }

    [Fact]
    public void Lerp_Endpoints_ReturnEndpoints()
    {
        var a = new Color(0.1f, 0.2f, 0.3f, 0.4f);
        var b = new Color(0.9f, 0.8f, 0.7f, 0.6f);
        Assert.Equal(a, Color.Lerp(a, b, 0f));
        Assert.Equal(b, Color.Lerp(a, b, 1f));
    }

    [Fact]
    public void ToString_ContainsChannelValues()
    {
        var s = new Color(1f, 0f, 0f, 1f).ToString();
        Assert.Contains("Color(", s);
    }
}
