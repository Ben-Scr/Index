using System.Threading;

namespace Index.Jobs;

/// <summary>
/// Reference-typed atomic 64-bit float. Arithmetic is a CAS loop on
/// <see cref="Interlocked.CompareExchange(ref double, double, double)"/>.
/// </summary>
public sealed class AtomicDouble
{
    private double m_Value;

    public AtomicDouble()
    {
    }

    public AtomicDouble(double initialValue)
    {
        m_Value = initialValue;
    }

    public double Value
    {
        get => Volatile.Read(ref m_Value);
        set => Volatile.Write(ref m_Value, value);
    }

    public double Load() => Volatile.Read(ref m_Value);

    public void Store(double value) => Volatile.Write(ref m_Value, value);

    public double Exchange(double newValue) => Interlocked.Exchange(ref m_Value, newValue);

    public double CompareExchange(double newValue, double comparand)
        => Interlocked.CompareExchange(ref m_Value, newValue, comparand);

    public double Add(double operand)
    {
        double current = Volatile.Read(ref m_Value);
        SpinWait spinner = default;
        while (true)
        {
            double updated = current + operand;
            double prev = Interlocked.CompareExchange(ref m_Value, updated, current);
            if (prev.Equals(current))
            {
                return updated;
            }
            current = prev;
            // SpinOnce ramps from pause instructions to thread yield as
            // contention grows; avoids burning the cache line in a tight
            // retry loop when many workers race on the same atomic.
            spinner.SpinOnce();
        }
    }

    public double Sub(double operand) => Add(-operand);

    public double FetchMax(double operand)
    {
        double current = Volatile.Read(ref m_Value);
        SpinWait spinner = default;
        while (operand > current)
        {
            double prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev.Equals(current))
            {
                return prev;
            }
            current = prev;
            spinner.SpinOnce();
        }
        return current;
    }

    public double FetchMin(double operand)
    {
        double current = Volatile.Read(ref m_Value);
        SpinWait spinner = default;
        while (operand < current)
        {
            double prev = Interlocked.CompareExchange(ref m_Value, operand, current);
            if (prev.Equals(current))
            {
                return prev;
            }
            current = prev;
            spinner.SpinOnce();
        }
        return current;
    }

    public override string ToString() => Load().ToString();
}
