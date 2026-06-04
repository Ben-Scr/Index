using System;
using Index.Collections.Native;
using Xunit;

namespace IndexScriptCoreTests;

public class NativeListTests
{
    [Fact]
    public void Add_IncreasesLength_AndIndexerReads()
    {
        using var list = new NativeList<int>(4);
        list.Add(10);
        list.Add(20);

        Assert.Equal(2, list.Length);
        Assert.Equal(10, list[0]);
        Assert.Equal(20, list[1]);
    }

    [Fact]
    public void Add_BeyondCapacity_GrowsAndPreservesValues()
    {
        using var list = new NativeList<int>(2);
        for (int i = 0; i < 50; i++)
            list.Add(i);

        Assert.Equal(50, list.Length);
        Assert.True(list.Capacity >= 50);
        for (int i = 0; i < 50; i++)
            Assert.Equal(i, list[i]);
    }

    [Fact]
    public void Indexer_ReturnsByRef_AllowsWrite()
    {
        using var list = new NativeList<int>(2);
        list.Add(1);
        list[0] = 99;
        Assert.Equal(99, list[0]);
    }

    [Fact]
    public void RemoveAt_ShiftsRemaining()
    {
        using var list = new NativeList<int>(4);
        list.Add(1);
        list.Add(2);
        list.Add(3);

        list.RemoveAt(1);

        Assert.Equal(2, list.Length);
        Assert.Equal(1, list[0]);
        Assert.Equal(3, list[1]);
    }

    [Fact]
    public void Remove_ReturnsFalseWhenAbsent_TrueWhenPresent()
    {
        using var list = new NativeList<int>(4);
        list.Add(1);

        Assert.False(list.Remove(42));
        Assert.True(list.Remove(1));
        Assert.Equal(0, list.Length);
    }

    [Fact]
    public void IndexOf_FindsValueOrReturnsMinusOne()
    {
        using var list = new NativeList<int>(4);
        list.Add(7);
        list.Add(8);

        Assert.Equal(1, list.IndexOf(8));
        Assert.Equal(-1, list.IndexOf(123));
    }

    [Fact]
    public void Indexer_OutOfRange_Throws()
    {
        using var list = new NativeList<int>(4);
        list.Add(1);

        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = list[5]; });
        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = list[-1]; });
    }

    [Fact]
    public void Constructor_NegativeCapacity_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeList<int>(-1));
    }

    [Fact]
    public void ZeroCapacity_NotCreated_ThenAddAllocates()
    {
        using var list = new NativeList<int>(0);
        Assert.False(list.IsCreated); // null buffer at zero capacity

        list.Add(5);

        Assert.True(list.IsCreated);
        Assert.Equal(1, list.Length);
        Assert.Equal(5, list[0]);
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var list = new NativeList<int>(4);
        list.Add(1);
        list.Dispose();

        Assert.False(list.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = list.Length; });

        // Double-dispose must be a safe no-op.
        list.Dispose();
    }

    [Fact]
    public void Enumerator_IteratesAllValues()
    {
        using var list = new NativeList<int>(4);
        list.Add(1);
        list.Add(2);
        list.Add(3);

        int sum = 0;
        foreach (var v in list)
            sum += v;

        Assert.Equal(6, sum);
    }

    [Fact]
    public void Enumerator_ThrowsIfModifiedDuringIteration()
    {
        using var list = new NativeList<int>(8);
        list.Add(1);
        list.Add(2);

        Assert.Throws<InvalidOperationException>(() =>
        {
            foreach (var v in list)
                list.Add(v); // structural modification bumps version -> next MoveNext throws
        });
    }
}
