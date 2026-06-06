using Index;
using Xunit;

namespace IndexScriptCoreTests;

public class QuaternionTests
{
    // NOTE: As authored, Quaternion only exposes three public float fields
    // (X, Y, Z) with no constructors, operators, constants, or methods.
    // There is no Identity / multiply / normalize / conjugate / Euler API to
    // exercise, so these tests cover only the literal public surface.

    [Fact]
    public void Default_FieldsAreZero()
    {
        var q = new Quaternion();
        Assert.Equal(0f, q.X);
        Assert.Equal(0f, q.Y);
        Assert.Equal(0f, q.Z);
    }

    [Fact]
    public void Fields_AreMutable()
    {
        var q = new Quaternion();
        q.X = 1f;
        q.Y = 2f;
        q.Z = 3f;

        Assert.Equal(1f, q.X);
        Assert.Equal(2f, q.Y);
        Assert.Equal(3f, q.Z);
    }

    [Fact]
    public void Struct_HasValueCopySemantics()
    {
        var a = new Quaternion();
        a.X = 5f;

        var b = a;
        b.X = 9f;

        // Quaternion is a struct, so the copy must not alias the original.
        Assert.Equal(5f, a.X);
        Assert.Equal(9f, b.X);
    }
}
