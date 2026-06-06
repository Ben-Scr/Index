using System;
using Index.Collections.Native;
using Xunit;

namespace IndexScriptCoreTests;

public class NativeArrayTests
{
    [Fact]
    public void Constructor_AllocatesLength_IsCreated_DefaultsToZero()
    {
        using var array = new NativeArray<int>(4);

        Assert.Equal(4, array.Length);
        Assert.True(array.IsCreated);
        for (int i = 0; i < array.Length; i++)
            Assert.Equal(0, array[i]);
    }

    [Fact]
    public void Indexer_ReturnsByRef_AllowsReadAndWrite()
    {
        using var array = new NativeArray<int>(3);
        array[0] = 10;
        array[1] = 20;
        array[2] = 30;

        Assert.Equal(10, array[0]);
        Assert.Equal(20, array[1]);
        Assert.Equal(30, array[2]);
    }

    [Fact]
    public void Indexer_OutOfRange_Throws()
    {
        using var array = new NativeArray<int>(4);

        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = array[4]; });
        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = array[-1]; });
    }

    [Fact]
    public void Constructor_NegativeLength_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeArray<int>(-1));
    }

    [Fact]
    public void ZeroLength_NotCreated_LengthIsZero()
    {
        using var array = new NativeArray<int>(0);

        Assert.False(array.IsCreated); // null buffer at zero length
        Assert.Equal(0, array.Length);
    }

    [Fact]
    public void Fill_SetsEveryElement()
    {
        using var array = new NativeArray<int>(5);
        array.Fill(7);

        for (int i = 0; i < array.Length; i++)
            Assert.Equal(7, array[i]);
    }

    [Fact]
    public void CopyFrom_CopiesManagedArrayContents()
    {
        using var array = new NativeArray<int>(4);
        array.CopyFrom(new[] { 1, 2, 3, 4 });

        Assert.Equal(1, array[0]);
        Assert.Equal(2, array[1]);
        Assert.Equal(3, array[2]);
        Assert.Equal(4, array[3]);
    }

    [Fact]
    public void CopyFrom_LengthMismatch_Throws()
    {
        using var array = new NativeArray<int>(4);

        Assert.Throws<ArgumentException>(() => array.CopyFrom(new[] { 1, 2 }));
    }

    [Fact]
    public void CopyFrom_Null_Throws()
    {
        using var array = new NativeArray<int>(4);

        Assert.Throws<ArgumentNullException>(() => array.CopyFrom(null!));
    }

    [Fact]
    public void CopyTo_CopiesIntoManagedArray()
    {
        using var array = new NativeArray<int>(3);
        array[0] = 5;
        array[1] = 6;
        array[2] = 7;

        var destination = new int[3];
        array.CopyTo(destination);

        Assert.Equal(new[] { 5, 6, 7 }, destination);
    }

    [Fact]
    public void CopyTo_LengthMismatch_Throws()
    {
        using var array = new NativeArray<int>(3);

        Assert.Throws<ArgumentException>(() => array.CopyTo(new int[2]));
    }

    [Fact]
    public void CopyTo_Null_Throws()
    {
        using var array = new NativeArray<int>(3);

        Assert.Throws<ArgumentNullException>(() => array.CopyTo(null!));
    }

    [Fact]
    public void RoundTrip_CopyFromThenCopyTo_PreservesValues()
    {
        var source = new[] { 11, 22, 33, 44 };
        using var array = new NativeArray<int>(source.Length);
        array.CopyFrom(source);

        var destination = new int[source.Length];
        array.CopyTo(destination);

        Assert.Equal(source, destination);
    }

    [Fact]
    public void Enumerator_SumsAllValues()
    {
        using var array = new NativeArray<int>(4);
        array[0] = 1;
        array[1] = 2;
        array[2] = 3;
        array[3] = 4;

        int sum = 0;
        foreach (var v in array)
            sum += v;

        Assert.Equal(10, sum);
    }

    [Fact]
    public void AsReadOnly_ReflectsValues_AndBoundsThrow()
    {
        using var array = new NativeArray<int>(3);
        array[0] = 100;
        array[2] = 300;

        var view = array.AsReadOnly();

        Assert.Equal(3, view.Length);
        Assert.Equal(100, view[0]);
        Assert.Equal(300, view[2]);
        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = view[3]; });
    }

    [Fact]
    public void AsWriteOnly_WritesThroughToBuffer_AndBoundsThrow()
    {
        using var array = new NativeArray<int>(3);

        var view = array.AsWriteOnly();
        view[1] = 42;

        Assert.Equal(42, array[1]);
        Assert.Throws<ArgumentOutOfRangeException>(() => view[3] = 0);
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var array = new NativeArray<int>(4);
        array[0] = 1;
        array.Dispose();

        Assert.False(array.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = array.Length; });
        Assert.Throws<ObjectDisposedException>(() => { _ = array[0]; });

        // Double-dispose must be a safe no-op.
        array.Dispose();
    }
}
