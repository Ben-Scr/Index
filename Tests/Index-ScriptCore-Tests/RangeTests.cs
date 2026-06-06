using System;
using Index.Collections;
using Xunit;

namespace IndexScriptCoreTests;

public class RangeTests
{
    [Fact]
    public void Constructor_SetsMinAndMax()
    {
        var r = new Range<int>(-2, 5);
        Assert.Equal(-2, r.Min);
        Assert.Equal(5, r.Max);
    }

    [Fact]
    public void Constructor_AllowsEqualMinMax()
    {
        var r = new Range<int>(3, 3);
        Assert.Equal(3, r.Min);
        Assert.Equal(3, r.Max);
    }

    [Fact]
    public void Constructor_MinGreaterThanMax_Throws()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new Range<int>(5, 1));
    }

    [Fact]
    public void MinSetter_AboveMax_Throws()
    {
        var r = new Range<int>(0, 10);
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Min = 11);
    }

    [Fact]
    public void MaxSetter_BelowMin_Throws()
    {
        var r = new Range<int>(0, 10);
        Assert.Throws<ArgumentOutOfRangeException>(() => r.Max = -1);
    }

    [Fact]
    public void Setters_UpdateBoundsWhenValid()
    {
        var r = new Range<int>(0, 10);
        r.Min = 2;
        r.Max = 8;
        Assert.Equal(2, r.Min);
        Assert.Equal(8, r.Max);
    }

    [Fact]
    public void Clamp_ReturnsBoundsOrValue()
    {
        var r = new Range<int>(0, 10);
        Assert.Equal(0, r.Clamp(-5)); // below min
        Assert.Equal(10, r.Clamp(20)); // above max
        Assert.Equal(7, r.Clamp(7)); // inside
    }

    [Fact]
    public void Clamp_ReturnsBoundsAtEdges()
    {
        var r = new Range<int>(0, 10);
        Assert.Equal(0, r.Clamp(0));
        Assert.Equal(10, r.Clamp(10));
    }

    [Fact]
    public void Clamp_WorksWithFloatingPoint()
    {
        var r = new Range<float>(-1.5f, 1.5f);
        Assert.Equal(-1.5f, r.Clamp(-2f));
        Assert.Equal(1.5f, r.Clamp(2f));
        Assert.Equal(0.25f, r.Clamp(0.25f));
    }

    [Fact]
    public void OutOfBounds_SingleValue()
    {
        var r = new Range<int>(0, 10);
        Assert.True(r.OutOfBounds(-1));
        Assert.True(r.OutOfBounds(11));
        Assert.False(r.OutOfBounds(0));
        Assert.False(r.OutOfBounds(10));
        Assert.False(r.OutOfBounds(5));
    }

    [Fact]
    public void OutOfBounds_Params_FalseIfAnyValueInBounds()
    {
        // The params overload returns false as soon as ANY value is in bounds.
        var r = new Range<int>(0, 10);
        Assert.False(r.OutOfBounds(-5, 100, 5)); // 5 is in bounds
    }

    [Fact]
    public void OutOfBounds_Params_TrueOnlyWhenAllOutOfBounds()
    {
        var r = new Range<int>(0, 10);
        Assert.True(r.OutOfBounds(-5, 100, 50));
    }

    [Fact]
    public void OutOfBounds_Params_AllInBounds_ReturnsFalse()
    {
        var r = new Range<int>(0, 10);
        Assert.False(r.OutOfBounds(1, 2, 3));
    }

    [Fact]
    public void ToString_FormatsMinAndMax()
    {
        var r = new Range<int>(2, 7);
        Assert.Equal("Range(2, 7)", r.ToString());
    }
}

// PerformanceTimer is pure-managed: it wraps System.Diagnostics.Stopwatch and
// touches no Index native InternalCall, so its state machine is safe to exercise.
public class PerformanceTimerTests
{
    [Fact]
    public void Default_IsNotRunning_WithZeroElapsed()
    {
        PerformanceTimer t = default;
        Assert.False(t.IsRunning);
        Assert.Equal(0L, t.ElapsedRawTicks);
        Assert.Equal(0.0, t.ElapsedSeconds);
        Assert.Equal(0f, t.ElapsedSecondsF);
    }

    [Fact]
    public void Start_SetsRunning()
    {
        PerformanceTimer t = default;
        t.Start();
        Assert.True(t.IsRunning);
    }

    [Fact]
    public void StartNew_ReturnsRunningTimer()
    {
        var t = PerformanceTimer.StartNew();
        Assert.True(t.IsRunning);
    }

    [Fact]
    public void Stop_FreezesElapsedAndClearsRunning()
    {
        var t = PerformanceTimer.StartNew();
        t.Stop();
        Assert.False(t.IsRunning);

        // Once stopped, ElapsedRawTicks reflects accumulated time and stays put.
        long frozen = t.ElapsedRawTicks;
        Assert.Equal(frozen, t.ElapsedRawTicks);
        Assert.True(frozen >= 0L);
    }

    [Fact]
    public void Stop_WhenNotRunning_IsNoOp()
    {
        PerformanceTimer t = default;
        t.Stop();
        Assert.False(t.IsRunning);
        Assert.Equal(0L, t.ElapsedRawTicks);
    }

    [Fact]
    public void Reset_ClearsRunningAndElapsed()
    {
        var t = PerformanceTimer.StartNew();
        t.Stop();
        t.Reset();
        Assert.False(t.IsRunning);
        Assert.Equal(0L, t.ElapsedRawTicks);
        Assert.Equal(0.0, t.ElapsedSeconds);
    }

    [Fact]
    public void Restart_SetsRunningAndResetsAccumulatedElapsed()
    {
        var t = PerformanceTimer.StartNew();
        t.Stop();
        t.Restart();
        Assert.True(t.IsRunning);
        // Accumulated time was cleared, so elapsed is measured fresh and non-negative.
        Assert.True(t.ElapsedRawTicks >= 0L);
    }

    [Fact]
    public void ElapsedRawTicks_IsNonNegativeWhileRunning()
    {
        var t = PerformanceTimer.StartNew();
        Assert.True(t.ElapsedRawTicks >= 0L);
        Assert.True(t.ElapsedSeconds >= 0.0);
    }

    [Fact]
    public void Start_WhenAlreadyRunning_IsNoOp()
    {
        var t = PerformanceTimer.StartNew();
        Assert.True(t.IsRunning);
        t.Start(); // second Start must not change the running state
        Assert.True(t.IsRunning);
    }
}
