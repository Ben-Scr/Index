using System;
using Index.Collections.Native;
using Xunit;

namespace IndexScriptCoreTests;

public class NativeStackTests
{
    [Fact]
    public void PushPop_IsLifo()
    {
        using var stack = new NativeStack<int>(4);
        stack.Push(1);
        stack.Push(2);
        stack.Push(3);

        Assert.Equal(3, stack.Count);
        Assert.Equal(3, stack.Pop());
        Assert.Equal(2, stack.Pop());
        Assert.Equal(1, stack.Pop());
        Assert.Equal(0, stack.Count);
    }

    [Fact]
    public void Peek_ReturnsTop_WithoutRemoving()
    {
        using var stack = new NativeStack<int>(4);
        stack.Push(10);
        stack.Push(20);

        Assert.Equal(20, stack.Peek());
        Assert.Equal(2, stack.Count);
        Assert.Equal(20, stack.Peek());
    }

    [Fact]
    public void Pop_OnEmpty_Throws()
    {
        using var stack = new NativeStack<int>(4);
        Assert.Throws<InvalidOperationException>(() => stack.Pop());
    }

    [Fact]
    public void Peek_OnEmpty_Throws()
    {
        using var stack = new NativeStack<int>(4);
        Assert.Throws<InvalidOperationException>(() => stack.Peek());
    }

    [Fact]
    public void TryPop_FalseWhenEmpty_TrueWhenPresent()
    {
        using var stack = new NativeStack<int>(4);

        Assert.False(stack.TryPop(out int empty));
        Assert.Equal(0, empty);

        stack.Push(42);
        Assert.True(stack.TryPop(out int value));
        Assert.Equal(42, value);
        Assert.Equal(0, stack.Count);
    }

    [Fact]
    public void Push_BeyondCapacity_GrowsAndPreservesLifoOrder()
    {
        using var stack = new NativeStack<int>(2);
        for (int i = 0; i < 50; i++)
            stack.Push(i);

        Assert.Equal(50, stack.Count);
        Assert.True(stack.Capacity >= 50);

        for (int i = 49; i >= 0; i--)
            Assert.Equal(i, stack.Pop());
    }

    [Fact]
    public void ZeroCapacity_NotCreated_ThenPushAllocates()
    {
        using var stack = new NativeStack<int>(0);
        Assert.False(stack.IsCreated);

        stack.Push(7);

        Assert.True(stack.IsCreated);
        Assert.Equal(1, stack.Count);
        Assert.Equal(7, stack.Peek());
    }

    [Fact]
    public void Constructor_NegativeCapacity_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeStack<int>(-1));
    }

    [Fact]
    public void Clear_ResetsCount()
    {
        using var stack = new NativeStack<int>(4);
        stack.Push(1);
        stack.Push(2);

        stack.Clear();

        Assert.Equal(0, stack.Count);
        Assert.False(stack.TryPop(out _));
    }

    [Fact]
    public void Reserve_GrowsCapacity_PreservesContents()
    {
        using var stack = new NativeStack<int>(2);
        stack.Push(5);
        stack.Push(6);

        stack.Reserve(64);

        Assert.True(stack.Capacity >= 64);
        Assert.Equal(2, stack.Count);
        Assert.Equal(6, stack.Pop());
        Assert.Equal(5, stack.Pop());
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var stack = new NativeStack<int>(4);
        stack.Push(1);
        stack.Dispose();

        Assert.False(stack.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = stack.Count; });

        // Double-dispose must be a safe no-op.
        stack.Dispose();
    }
}

public class NativeQueueTests
{
    [Fact]
    public void EnqueueDequeue_IsFifo()
    {
        using var queue = new NativeQueue<int>(4);
        queue.Enqueue(1);
        queue.Enqueue(2);
        queue.Enqueue(3);

        Assert.Equal(3, queue.Count);
        Assert.Equal(1, queue.Dequeue());
        Assert.Equal(2, queue.Dequeue());
        Assert.Equal(3, queue.Dequeue());
        Assert.Equal(0, queue.Count);
        Assert.True(queue.IsEmpty);
    }

    [Fact]
    public void Peek_ReturnsFront_WithoutRemoving()
    {
        using var queue = new NativeQueue<int>(4);
        queue.Enqueue(10);
        queue.Enqueue(20);

        Assert.Equal(10, queue.Peek());
        Assert.Equal(2, queue.Count);
        Assert.Equal(10, queue.Peek());
    }

    [Fact]
    public void Dequeue_OnEmpty_Throws()
    {
        using var queue = new NativeQueue<int>(4);
        Assert.Throws<InvalidOperationException>(() => queue.Dequeue());
    }

    [Fact]
    public void Peek_OnEmpty_Throws()
    {
        using var queue = new NativeQueue<int>(4);
        Assert.Throws<InvalidOperationException>(() => queue.Peek());
    }

    [Fact]
    public void TryDequeue_FalseWhenEmpty_TrueWhenPresent()
    {
        using var queue = new NativeQueue<int>(4);

        Assert.False(queue.TryDequeue(out int empty));
        Assert.Equal(0, empty);

        queue.Enqueue(99);
        Assert.True(queue.TryDequeue(out int value));
        Assert.Equal(99, value);
        Assert.True(queue.IsEmpty);
    }

    [Fact]
    public void TryPeek_FalseWhenEmpty_TrueWhenPresent()
    {
        using var queue = new NativeQueue<int>(4);

        Assert.False(queue.TryPeek(out int empty));
        Assert.Equal(0, empty);

        queue.Enqueue(8);
        Assert.True(queue.TryPeek(out int value));
        Assert.Equal(8, value);
        Assert.Equal(1, queue.Count);
    }

    [Fact]
    public void Enqueue_BeyondCapacity_GrowsAndPreservesFifoOrder()
    {
        using var queue = new NativeQueue<int>(2);
        for (int i = 0; i < 50; i++)
            queue.Enqueue(i);

        Assert.Equal(50, queue.Count);
        Assert.True(queue.Capacity >= 50);

        for (int i = 0; i < 50; i++)
            Assert.Equal(i, queue.Dequeue());
    }

    [Fact]
    public void Wraparound_PreservesFifoOrderAcrossRingBuffer()
    {
        using var queue = new NativeQueue<int>(4);

        // Fill, drain part, refill to force the tail to wrap around head.
        queue.Enqueue(1);
        queue.Enqueue(2);
        queue.Enqueue(3);
        Assert.Equal(1, queue.Dequeue());
        Assert.Equal(2, queue.Dequeue());

        queue.Enqueue(4);
        queue.Enqueue(5);
        queue.Enqueue(6);

        Assert.Equal(3, queue.Dequeue());
        Assert.Equal(4, queue.Dequeue());
        Assert.Equal(5, queue.Dequeue());
        Assert.Equal(6, queue.Dequeue());
        Assert.True(queue.IsEmpty);
    }

    [Fact]
    public void Grow_WhileWrapped_PreservesFifoOrder()
    {
        using var queue = new NativeQueue<int>(4);

        // Advance head so the live region straddles the buffer end before growth.
        queue.Enqueue(0);
        queue.Enqueue(1);
        queue.Dequeue();
        queue.Dequeue();

        for (int i = 0; i < 10; i++)
            queue.Enqueue(i);

        Assert.True(queue.Capacity >= 10);
        Assert.Equal(10, queue.Count);
        for (int i = 0; i < 10; i++)
            Assert.Equal(i, queue.Dequeue());
    }

    [Fact]
    public void ZeroCapacity_NotCreated_ThenEnqueueAllocates()
    {
        using var queue = new NativeQueue<int>(0);
        Assert.False(queue.IsCreated);

        queue.Enqueue(7);

        Assert.True(queue.IsCreated);
        Assert.Equal(1, queue.Count);
        Assert.Equal(7, queue.Peek());
    }

    [Fact]
    public void Constructor_NegativeCapacity_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeQueue<int>(-1));
    }

    [Fact]
    public void Clear_ResetsCount()
    {
        using var queue = new NativeQueue<int>(4);
        queue.Enqueue(1);
        queue.Enqueue(2);

        queue.Clear();

        Assert.Equal(0, queue.Count);
        Assert.True(queue.IsEmpty);
        Assert.False(queue.TryDequeue(out _));
    }

    [Fact]
    public void Dispose_MakesNotCreated_AccessThrows_AndIsIdempotent()
    {
        var queue = new NativeQueue<int>(4);
        queue.Enqueue(1);
        queue.Dispose();

        Assert.False(queue.IsCreated);
        Assert.Throws<ObjectDisposedException>(() => { _ = queue.Count; });

        // Double-dispose must be a safe no-op.
        queue.Dispose();
    }
}
