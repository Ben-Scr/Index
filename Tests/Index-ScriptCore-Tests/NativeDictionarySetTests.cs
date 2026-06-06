using System;
using System.Collections.Generic;
using Index.Collections.Native;
using Xunit;

namespace IndexScriptCoreTests;

public class NativeDictionaryTests
{
    [Fact]
    public void Add_StoresValue_IndexerReads_AndCountIncreases()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 100);
        dict.Add(2, 200);

        Assert.Equal(2, dict.Count);
        Assert.Equal(100, dict[1]);
        Assert.Equal(200, dict[2]);
    }

    [Fact]
    public void Add_DuplicateKey_Throws()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 100);

        Assert.Throws<ArgumentException>(() => dict.Add(1, 999));
    }

    [Fact]
    public void TryAdd_ReturnsTrueForNew_FalseForDuplicate()
    {
        using var dict = new NativeDictionary<int, int>(4);

        Assert.True(dict.TryAdd(1, 100));
        Assert.False(dict.TryAdd(1, 200));
        Assert.Equal(1, dict.Count);
        Assert.Equal(100, dict[1]);
    }

    [Fact]
    public void TryGetValue_HitReturnsTrueAndValue()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(5, 50);

        Assert.True(dict.TryGetValue(5, out int value));
        Assert.Equal(50, value);
    }

    [Fact]
    public void TryGetValue_MissReturnsFalseAndDefault()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(5, 50);

        Assert.False(dict.TryGetValue(42, out int value));
        Assert.Equal(0, value);
    }

    [Fact]
    public void ContainsKey_TrueWhenPresent_FalseWhenAbsent()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(7, 70);

        Assert.True(dict.ContainsKey(7));
        Assert.False(dict.ContainsKey(8));
    }

    [Fact]
    public void Remove_PresentReturnsTrue_AbsentReturnsFalse()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 10);
        dict.Add(2, 20);

        Assert.False(dict.Remove(99));
        Assert.True(dict.Remove(1));
        Assert.Equal(1, dict.Count);
        Assert.False(dict.ContainsKey(1));
        Assert.True(dict.ContainsKey(2));
    }

    [Fact]
    public void Indexer_Setter_OverwritesExistingKey_WithoutChangingCount()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 100);

        dict[1] = 999;

        Assert.Equal(1, dict.Count);
        Assert.Equal(999, dict[1]);
    }

    [Fact]
    public void Indexer_Setter_AddsNewKey()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict[3] = 30;

        Assert.Equal(1, dict.Count);
        Assert.Equal(30, dict[3]);
    }

    [Fact]
    public void Indexer_Getter_MissingKey_Throws()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 100);

        Assert.Throws<KeyNotFoundException>(() => { _ = dict[42]; });
    }

    [Fact]
    public void Add_BeyondInitialCapacity_GrowsAndPreservesEntries()
    {
        using var dict = new NativeDictionary<int, int>(2);
        for (int i = 0; i < 100; i++)
            dict.Add(i, i * 10);

        Assert.Equal(100, dict.Count);
        for (int i = 0; i < 100; i++)
        {
            Assert.True(dict.TryGetValue(i, out int value));
            Assert.Equal(i * 10, value);
        }
    }

    [Fact]
    public void Constructor_NegativeCapacity_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeDictionary<int, int>(-1));
    }

    [Fact]
    public void ZeroCapacity_NotCreated_ThenAddAllocates()
    {
        using var dict = new NativeDictionary<int, int>(0);
        Assert.False(dict.IsCreated);

        dict.Add(1, 100);

        Assert.True(dict.IsCreated);
        Assert.Equal(1, dict.Count);
        Assert.Equal(100, dict[1]);
    }

    [Fact]
    public void Clear_ResetsCount_AndRemovesEntries()
    {
        using var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 10);
        dict.Add(2, 20);

        dict.Clear();

        Assert.Equal(0, dict.Count);
        Assert.False(dict.ContainsKey(1));
        Assert.False(dict.ContainsKey(2));
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var dict = new NativeDictionary<int, int>(4);
        dict.Add(1, 10);
        dict.Dispose();

        Assert.False(dict.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = dict.Count; });

        // Double-dispose must be a safe no-op.
        dict.Dispose();
    }

    [Fact]
    public void Enumerator_IteratesAllEntries()
    {
        using var dict = new NativeDictionary<int, int>(8);
        dict.Add(1, 10);
        dict.Add(2, 20);
        dict.Add(3, 30);

        int keySum = 0;
        int valueSum = 0;
        foreach (var kvp in dict)
        {
            keySum += kvp.Key;
            valueSum += kvp.Value;
        }

        Assert.Equal(6, keySum);
        Assert.Equal(60, valueSum);
    }
}

public class NativeHashsetTests
{
    [Fact]
    public void Add_ReturnsTrueForNew_FalseForDuplicate()
    {
        using var set = new NativeHashset<int>(4);

        Assert.True(set.Add(1));
        Assert.True(set.Add(2));
        Assert.False(set.Add(1));
        Assert.Equal(2, set.Count);
    }

    [Fact]
    public void Contains_TrueWhenPresent_FalseWhenAbsent()
    {
        using var set = new NativeHashset<int>(4);
        set.Add(5);

        Assert.True(set.Contains(5));
        Assert.False(set.Contains(6));
    }

    [Fact]
    public void Remove_PresentReturnsTrue_AbsentReturnsFalse()
    {
        using var set = new NativeHashset<int>(4);
        set.Add(1);
        set.Add(2);

        Assert.False(set.Remove(99));
        Assert.True(set.Remove(1));
        Assert.Equal(1, set.Count);
        Assert.False(set.Contains(1));
        Assert.True(set.Contains(2));
    }

    [Fact]
    public void Add_BeyondInitialCapacity_GrowsAndPreservesElements()
    {
        using var set = new NativeHashset<int>(2);
        for (int i = 0; i < 100; i++)
            Assert.True(set.Add(i));

        Assert.Equal(100, set.Count);
        for (int i = 0; i < 100; i++)
            Assert.True(set.Contains(i));
    }

    [Fact]
    public void Constructor_NegativeCapacity_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeHashset<int>(-1));
    }

    [Fact]
    public void Clear_ResetsCount_AndRemovesElements()
    {
        using var set = new NativeHashset<int>(4);
        set.Add(1);
        set.Add(2);

        set.Clear();

        Assert.Equal(0, set.Count);
        Assert.False(set.Contains(1));
        Assert.False(set.Contains(2));
    }

    [Fact]
    public void Reset_ClearsAllElements()
    {
        using var set = new NativeHashset<int>(4);
        set.Add(1);
        set.Add(2);

        set.Reset(clearMemory: true);

        Assert.Equal(0, set.Count);
        Assert.False(set.Contains(1));
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var set = new NativeHashset<int>(4);
        set.Add(1);
        set.Dispose();

        Assert.False(set.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = set.Count; });

        // Double-dispose must be a safe no-op.
        set.Dispose();
    }

    [Fact]
    public void Enumerator_IteratesAllElements()
    {
        using var set = new NativeHashset<int>(8);
        set.Add(1);
        set.Add(2);
        set.Add(3);

        int sum = 0;
        foreach (var v in set)
            sum += v;

        Assert.Equal(6, sum);
    }

    [Fact]
    public void Enumerator_ThrowsIfModifiedDuringIteration()
    {
        using var set = new NativeHashset<int>(16);
        set.Add(1);
        set.Add(2);

        Assert.Throws<InvalidOperationException>(() =>
        {
            foreach (var v in set)
                set.Add(v + 100); // structural modification bumps version -> next MoveNext throws
        });
    }
}
