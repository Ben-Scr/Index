using Index.Jobs;
using Xunit;

namespace IndexScriptCoreTests;

public class AtomicIntTests
{
    [Fact]
    public void DefaultConstructor_StartsAtZero()
    {
        var atomic = new AtomicInt();
        Assert.Equal(0, atomic.Value);
        Assert.Equal(0, atomic.Load());
    }

    [Fact]
    public void Constructor_SetsInitialValue()
    {
        var atomic = new AtomicInt(42);
        Assert.Equal(42, atomic.Value);
        Assert.Equal(42, atomic.Load());
    }

    [Fact]
    public void Value_SetAndStore_RoundTrip()
    {
        var atomic = new AtomicInt();
        atomic.Value = 7;
        Assert.Equal(7, atomic.Value);

        atomic.Store(13);
        Assert.Equal(13, atomic.Load());
    }

    [Fact]
    public void Add_ReturnsNewValue()
    {
        var atomic = new AtomicInt(10);
        Assert.Equal(15, atomic.Add(5));
        Assert.Equal(15, atomic.Load());
    }

    [Fact]
    public void Sub_ReturnsNewValue()
    {
        var atomic = new AtomicInt(10);
        Assert.Equal(4, atomic.Sub(6));
        Assert.Equal(4, atomic.Load());
    }

    [Fact]
    public void Increment_And_Decrement()
    {
        var atomic = new AtomicInt(0);
        Assert.Equal(1, atomic.Increment());
        Assert.Equal(2, atomic.Increment());
        Assert.Equal(1, atomic.Decrement());
        Assert.Equal(1, atomic.Load());
    }

    [Fact]
    public void Exchange_ReturnsOldValue_AndStoresNew()
    {
        var atomic = new AtomicInt(3);
        int old = atomic.Exchange(99);
        Assert.Equal(3, old);
        Assert.Equal(99, atomic.Load());
    }

    [Fact]
    public void CompareExchange_Succeeds_WhenComparandMatches()
    {
        var atomic = new AtomicInt(5);
        int prev = atomic.CompareExchange(50, 5);
        Assert.Equal(5, prev);       // returns the prior value
        Assert.Equal(50, atomic.Load()); // value was replaced
    }

    [Fact]
    public void CompareExchange_Fails_WhenComparandDiffers()
    {
        var atomic = new AtomicInt(5);
        int prev = atomic.CompareExchange(50, 999);
        Assert.Equal(5, prev);       // returns the prior value
        Assert.Equal(5, atomic.Load()); // value unchanged
    }

    [Fact]
    public void Or_And_BitwiseOps()
    {
        var atomic = new AtomicInt(0b0100);
        Assert.Equal(0b0100, atomic.Or(0b0001));   // returns original
        Assert.Equal(0b0101, atomic.Load());

        Assert.Equal(0b0101, atomic.And(0b0001));  // returns original
        Assert.Equal(0b0001, atomic.Load());
    }

    [Fact]
    public void FetchMax_RaisesValue_AndReturnsPrior()
    {
        var atomic = new AtomicInt(10);
        Assert.Equal(10, atomic.FetchMax(20)); // raised; prior returned
        Assert.Equal(20, atomic.Load());

        // Smaller operand does not lower the value.
        Assert.Equal(20, atomic.FetchMax(5));
        Assert.Equal(20, atomic.Load());
    }

    [Fact]
    public void FetchMin_LowersValue_AndReturnsPrior()
    {
        var atomic = new AtomicInt(10);
        Assert.Equal(10, atomic.FetchMin(3)); // lowered; prior returned
        Assert.Equal(3, atomic.Load());

        // Larger operand does not raise the value.
        Assert.Equal(3, atomic.FetchMin(50));
        Assert.Equal(3, atomic.Load());
    }

    [Fact]
    public void ToString_ReflectsCurrentValue()
    {
        var atomic = new AtomicInt(123);
        Assert.Equal("123", atomic.ToString());
    }
}

public class AtomicLongTests
{
    [Fact]
    public void DefaultConstructor_StartsAtZero()
    {
        var atomic = new AtomicLong();
        Assert.Equal(0L, atomic.Value);
        Assert.Equal(0L, atomic.Load());
    }

    [Fact]
    public void Constructor_SetsInitialValue()
    {
        var atomic = new AtomicLong(9_000_000_000L);
        Assert.Equal(9_000_000_000L, atomic.Value);
        Assert.Equal(9_000_000_000L, atomic.Load());
    }

    [Fact]
    public void Value_SetAndStore_RoundTrip()
    {
        var atomic = new AtomicLong();
        atomic.Value = 7L;
        Assert.Equal(7L, atomic.Value);

        atomic.Store(13L);
        Assert.Equal(13L, atomic.Load());
    }

    [Fact]
    public void Add_And_Sub_ReturnNewValue()
    {
        var atomic = new AtomicLong(100L);
        Assert.Equal(150L, atomic.Add(50L));
        Assert.Equal(120L, atomic.Sub(30L));
        Assert.Equal(120L, atomic.Load());
    }

    [Fact]
    public void Increment_And_Decrement()
    {
        var atomic = new AtomicLong(0L);
        Assert.Equal(1L, atomic.Increment());
        Assert.Equal(2L, atomic.Increment());
        Assert.Equal(1L, atomic.Decrement());
        Assert.Equal(1L, atomic.Load());
    }

    [Fact]
    public void Exchange_ReturnsOldValue_AndStoresNew()
    {
        var atomic = new AtomicLong(3L);
        long old = atomic.Exchange(99L);
        Assert.Equal(3L, old);
        Assert.Equal(99L, atomic.Load());
    }

    [Fact]
    public void CompareExchange_Succeeds_WhenComparandMatches()
    {
        var atomic = new AtomicLong(5L);
        long prev = atomic.CompareExchange(50L, 5L);
        Assert.Equal(5L, prev);
        Assert.Equal(50L, atomic.Load());
    }

    [Fact]
    public void CompareExchange_Fails_WhenComparandDiffers()
    {
        var atomic = new AtomicLong(5L);
        long prev = atomic.CompareExchange(50L, 999L);
        Assert.Equal(5L, prev);
        Assert.Equal(5L, atomic.Load());
    }

    [Fact]
    public void Or_And_BitwiseOps()
    {
        var atomic = new AtomicLong(0b0100L);
        Assert.Equal(0b0100L, atomic.Or(0b0001L));
        Assert.Equal(0b0101L, atomic.Load());

        Assert.Equal(0b0101L, atomic.And(0b0001L));
        Assert.Equal(0b0001L, atomic.Load());
    }

    [Fact]
    public void FetchMax_RaisesValue_AndReturnsPrior()
    {
        var atomic = new AtomicLong(10L);
        Assert.Equal(10L, atomic.FetchMax(20L));
        Assert.Equal(20L, atomic.Load());

        Assert.Equal(20L, atomic.FetchMax(5L));
        Assert.Equal(20L, atomic.Load());
    }

    [Fact]
    public void FetchMin_LowersValue_AndReturnsPrior()
    {
        var atomic = new AtomicLong(10L);
        Assert.Equal(10L, atomic.FetchMin(3L));
        Assert.Equal(3L, atomic.Load());

        Assert.Equal(3L, atomic.FetchMin(50L));
        Assert.Equal(3L, atomic.Load());
    }

    [Fact]
    public void ToString_ReflectsCurrentValue()
    {
        var atomic = new AtomicLong(123L);
        Assert.Equal("123", atomic.ToString());
    }
}

public class AtomicBoolTests
{
    [Fact]
    public void DefaultConstructor_IsFalse()
    {
        var atomic = new AtomicBool();
        Assert.False(atomic.Value);
        Assert.False(atomic.Load());
    }

    [Fact]
    public void Constructor_SetsInitialValue()
    {
        Assert.True(new AtomicBool(true).Value);
        Assert.False(new AtomicBool(false).Value);
    }

    [Fact]
    public void Value_SetAndStore_RoundTrip()
    {
        var atomic = new AtomicBool();
        atomic.Value = true;
        Assert.True(atomic.Value);

        atomic.Store(false);
        Assert.False(atomic.Load());
    }

    [Fact]
    public void Exchange_ReturnsOldValue_AndStoresNew()
    {
        var atomic = new AtomicBool(false);
        bool old = atomic.Exchange(true);
        Assert.False(old);
        Assert.True(atomic.Load());
    }

    [Fact]
    public void CompareExchange_Succeeds_WhenComparandMatches()
    {
        var atomic = new AtomicBool(false);
        bool prev = atomic.CompareExchange(true, false);
        Assert.False(prev);          // returns the prior value
        Assert.True(atomic.Load());  // value was replaced
    }

    [Fact]
    public void CompareExchange_Fails_WhenComparandDiffers()
    {
        var atomic = new AtomicBool(false);
        bool prev = atomic.CompareExchange(true, true);
        Assert.False(prev);          // returns the prior value
        Assert.False(atomic.Load()); // value unchanged
    }

    [Fact]
    public void TrySet_TransitionsFalseToTrue_OnlyOnce()
    {
        var atomic = new AtomicBool(false);
        Assert.True(atomic.TrySet());  // made the transition
        Assert.True(atomic.Load());

        Assert.False(atomic.TrySet()); // already true, no transition
        Assert.True(atomic.Load());
    }

    [Fact]
    public void TryClear_TransitionsTrueToFalse_OnlyOnce()
    {
        var atomic = new AtomicBool(true);
        Assert.True(atomic.TryClear());  // made the transition
        Assert.False(atomic.Load());

        Assert.False(atomic.TryClear()); // already false, no transition
        Assert.False(atomic.Load());
    }

    [Fact]
    public void ToString_ReflectsCurrentValue()
    {
        Assert.Equal("true", new AtomicBool(true).ToString());
        Assert.Equal("false", new AtomicBool(false).ToString());
    }
}

public class AtomicFloatTests
{
    private const int Precision = 5;

    [Fact]
    public void DefaultConstructor_StartsAtZero()
    {
        var atomic = new AtomicFloat();
        Assert.Equal(0f, atomic.Value, Precision);
        Assert.Equal(0f, atomic.Load(), Precision);
    }

    [Fact]
    public void Constructor_SetsInitialValue()
    {
        var atomic = new AtomicFloat(1.5f);
        Assert.Equal(1.5f, atomic.Value, Precision);
        Assert.Equal(1.5f, atomic.Load(), Precision);
    }

    [Fact]
    public void Value_SetAndStore_RoundTrip()
    {
        var atomic = new AtomicFloat();
        atomic.Value = 2.25f;
        Assert.Equal(2.25f, atomic.Value, Precision);

        atomic.Store(-3.5f);
        Assert.Equal(-3.5f, atomic.Load(), Precision);
    }

    [Fact]
    public void Add_ReturnsNewValue()
    {
        var atomic = new AtomicFloat(1.0f);
        Assert.Equal(2.5f, atomic.Add(1.5f), Precision);
        Assert.Equal(2.5f, atomic.Load(), Precision);
    }

    [Fact]
    public void Sub_ReturnsNewValue()
    {
        var atomic = new AtomicFloat(5.0f);
        Assert.Equal(3.5f, atomic.Sub(1.5f), Precision);
        Assert.Equal(3.5f, atomic.Load(), Precision);
    }

    [Fact]
    public void Exchange_ReturnsOldValue_AndStoresNew()
    {
        var atomic = new AtomicFloat(3.0f);
        float old = atomic.Exchange(9.0f);
        Assert.Equal(3.0f, old, Precision);
        Assert.Equal(9.0f, atomic.Load(), Precision);
    }

    [Fact]
    public void CompareExchange_Succeeds_WhenComparandMatches()
    {
        var atomic = new AtomicFloat(5.0f);
        float prev = atomic.CompareExchange(50.0f, 5.0f);
        Assert.Equal(5.0f, prev, Precision);
        Assert.Equal(50.0f, atomic.Load(), Precision);
    }

    [Fact]
    public void CompareExchange_Fails_WhenComparandDiffers()
    {
        var atomic = new AtomicFloat(5.0f);
        float prev = atomic.CompareExchange(50.0f, 999.0f);
        Assert.Equal(5.0f, prev, Precision);
        Assert.Equal(5.0f, atomic.Load(), Precision);
    }

    [Fact]
    public void FetchMax_RaisesValue_AndReturnsPrior()
    {
        var atomic = new AtomicFloat(10.0f);
        Assert.Equal(10.0f, atomic.FetchMax(20.0f), Precision);
        Assert.Equal(20.0f, atomic.Load(), Precision);

        Assert.Equal(20.0f, atomic.FetchMax(5.0f), Precision);
        Assert.Equal(20.0f, atomic.Load(), Precision);
    }

    [Fact]
    public void FetchMin_LowersValue_AndReturnsPrior()
    {
        var atomic = new AtomicFloat(10.0f);
        Assert.Equal(10.0f, atomic.FetchMin(3.0f), Precision);
        Assert.Equal(3.0f, atomic.Load(), Precision);

        Assert.Equal(3.0f, atomic.FetchMin(50.0f), Precision);
        Assert.Equal(3.0f, atomic.Load(), Precision);
    }
}

public class AtomicDoubleTests
{
    private const int Precision = 10;

    [Fact]
    public void DefaultConstructor_StartsAtZero()
    {
        var atomic = new AtomicDouble();
        Assert.Equal(0d, atomic.Value, Precision);
        Assert.Equal(0d, atomic.Load(), Precision);
    }

    [Fact]
    public void Constructor_SetsInitialValue()
    {
        var atomic = new AtomicDouble(1.5d);
        Assert.Equal(1.5d, atomic.Value, Precision);
        Assert.Equal(1.5d, atomic.Load(), Precision);
    }

    [Fact]
    public void Value_SetAndStore_RoundTrip()
    {
        var atomic = new AtomicDouble();
        atomic.Value = 2.25d;
        Assert.Equal(2.25d, atomic.Value, Precision);

        atomic.Store(-3.5d);
        Assert.Equal(-3.5d, atomic.Load(), Precision);
    }

    [Fact]
    public void Add_ReturnsNewValue()
    {
        var atomic = new AtomicDouble(1.0d);
        Assert.Equal(2.5d, atomic.Add(1.5d), Precision);
        Assert.Equal(2.5d, atomic.Load(), Precision);
    }

    [Fact]
    public void Sub_ReturnsNewValue()
    {
        var atomic = new AtomicDouble(5.0d);
        Assert.Equal(3.5d, atomic.Sub(1.5d), Precision);
        Assert.Equal(3.5d, atomic.Load(), Precision);
    }

    [Fact]
    public void Exchange_ReturnsOldValue_AndStoresNew()
    {
        var atomic = new AtomicDouble(3.0d);
        double old = atomic.Exchange(9.0d);
        Assert.Equal(3.0d, old, Precision);
        Assert.Equal(9.0d, atomic.Load(), Precision);
    }

    [Fact]
    public void CompareExchange_Succeeds_WhenComparandMatches()
    {
        var atomic = new AtomicDouble(5.0d);
        double prev = atomic.CompareExchange(50.0d, 5.0d);
        Assert.Equal(5.0d, prev, Precision);
        Assert.Equal(50.0d, atomic.Load(), Precision);
    }

    [Fact]
    public void CompareExchange_Fails_WhenComparandDiffers()
    {
        var atomic = new AtomicDouble(5.0d);
        double prev = atomic.CompareExchange(50.0d, 999.0d);
        Assert.Equal(5.0d, prev, Precision);
        Assert.Equal(5.0d, atomic.Load(), Precision);
    }

    [Fact]
    public void FetchMax_RaisesValue_AndReturnsPrior()
    {
        var atomic = new AtomicDouble(10.0d);
        Assert.Equal(10.0d, atomic.FetchMax(20.0d), Precision);
        Assert.Equal(20.0d, atomic.Load(), Precision);

        Assert.Equal(20.0d, atomic.FetchMax(5.0d), Precision);
        Assert.Equal(20.0d, atomic.Load(), Precision);
    }

    [Fact]
    public void FetchMin_LowersValue_AndReturnsPrior()
    {
        var atomic = new AtomicDouble(10.0d);
        Assert.Equal(10.0d, atomic.FetchMin(3.0d), Precision);
        Assert.Equal(3.0d, atomic.Load(), Precision);

        Assert.Equal(3.0d, atomic.FetchMin(50.0d), Precision);
        Assert.Equal(3.0d, atomic.Load(), Precision);
    }
}
