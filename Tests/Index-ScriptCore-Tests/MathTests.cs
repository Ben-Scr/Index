using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class Vector2Tests
{
    [Fact]
    public void Constructors_SetComponents()
    {
        var s = new Vector2(2f);
        Assert.Equal(2f, s.X);
        Assert.Equal(2f, s.Y);

        var v = new Vector2(3f, 4f);
        Assert.Equal(3f, v.X);
        Assert.Equal(4f, v.Y);
    }

    [Fact]
    public void Operators_AddSubtractScaleNegateDivide()
    {
        var a = new Vector2(1f, 2f);
        var b = new Vector2(3f, 5f);

        Assert.Equal(new Vector2(4f, 7f), a + b);
        Assert.Equal(new Vector2(-2f, -3f), a - b);
        Assert.Equal(new Vector2(2f, 4f), a * 2f);
        Assert.Equal(new Vector2(2f, 4f), 2f * a);
        Assert.Equal(new Vector2(-1f, -2f), -a);
        Assert.Equal(new Vector2(0.5f, 1f), a / 2f);
    }

    [Fact]
    public void Length_OfThreeFour_IsFive()
    {
        var v = new Vector2(3f, 4f);
        Assert.Equal(25f, v.LengthSquared());
        Assert.Equal(5.0, v.Length(), 4);
    }

    [Fact]
    public void Normalized_HasUnitLength()
    {
        var n = new Vector2(3f, 4f).Normalized();
        Assert.Equal(1.0, n.Length(), 4);
    }

    [Fact]
    public void Normalized_OfZero_ReturnsZero()
    {
        // Below Mathf.Epsilon the method must return Zero, not NaN.
        Assert.Equal(Vector2.Zero, Vector2.Zero.Normalized());
    }

    [Fact]
    public void Dot_And_Distance()
    {
        Assert.Equal(11f, Vector2.Dot(new Vector2(1f, 2f), new Vector2(3f, 4f)));
        Assert.Equal(5.0, Vector2.Distance(Vector2.Zero, new Vector2(3f, 4f)), 4);
    }

    [Fact]
    public void Lerp_ClampsParameter()
    {
        var a = new Vector2(0f, 0f);
        var b = new Vector2(10f, 20f);

        Assert.Equal(new Vector2(5f, 10f), Vector2.Lerp(a, b, 0.5f));
        Assert.Equal(a, Vector2.Lerp(a, b, -1f)); // t clamped to 0
        Assert.Equal(b, Vector2.Lerp(a, b, 2f));  // t clamped to 1
    }

    [Fact]
    public void Perpendicular_And_Reflect()
    {
        Assert.Equal(new Vector2(-2f, 1f), Vector2.Perpendicular(new Vector2(1f, 2f)));

        // A vector travelling down-right, reflected off flat ground (normal Up),
        // should travel up-right.
        Assert.Equal(new Vector2(1f, 1f), Vector2.Reflect(new Vector2(1f, -1f), Vector2.Up));
    }

    [Fact]
    public void Equality_And_HashCode()
    {
        Assert.True(new Vector2(1f, 1f) == Vector2.One);
        Assert.True(new Vector2(1f, 0f) != Vector2.One);
        Assert.Equal(Vector2.One.GetHashCode(), new Vector2(1f, 1f).GetHashCode());
    }
}

public class MathfTests
{
    [Fact]
    public void Clamp_BoundsValue()
    {
        Assert.Equal(5f, Mathf.Clamp(10f, 0f, 5f));
        Assert.Equal(0f, Mathf.Clamp(-3f, 0f, 5f));
        Assert.Equal(3f, Mathf.Clamp(3f, 0f, 5f));
        Assert.Equal(1f, Mathf.Clamp01(2f));
        Assert.Equal(0f, Mathf.Clamp01(-2f));
    }

    [Fact]
    public void Lerp_Clamps_LerpUnclamped_DoesNot()
    {
        Assert.Equal(5f, Mathf.Lerp(0f, 10f, 0.5f));
        Assert.Equal(10f, Mathf.Lerp(0f, 10f, 2f));         // clamped
        Assert.Equal(20f, Mathf.LerpUnclamped(0f, 10f, 2f)); // not clamped
    }

    [Fact]
    public void InverseLerp_IncludingDegenerateRange()
    {
        Assert.Equal(0.25, Mathf.InverseLerp(0f, 8f, 2f), 4);
        Assert.Equal(0f, Mathf.InverseLerp(5f, 5f, 5f)); // a==b guarded to 0
    }

    [Fact]
    public void Sign_ZeroMapsToZero()
    {
        Assert.Equal(-1f, Mathf.Sign(-4f));
        Assert.Equal(1f, Mathf.Sign(4f));
        Assert.Equal(0f, Mathf.Sign(0f));
    }

    [Fact]
    public void Approximately_UsesEpsilon()
    {
        Assert.True(Mathf.Approximately(1f, 1f + Mathf.Epsilon * 0.5f));
        Assert.False(Mathf.Approximately(1f, 1.001f));
    }

    [Fact]
    public void SmoothStep_EndpointsAndMidpoint()
    {
        Assert.Equal(0f, Mathf.SmoothStep(0f, 1f, 0f));
        Assert.Equal(1f, Mathf.SmoothStep(0f, 1f, 1f));
        Assert.Equal(0.5, Mathf.SmoothStep(0f, 1f, 0.5f), 4);
    }

    [Fact]
    public void MinMax_ParamsOverloads()
    {
        Assert.Equal(1, Mathf.Min(5, 3, 1, 9));
        Assert.Equal(9, Mathf.Max(5, 3, 1, 9));
    }
}
