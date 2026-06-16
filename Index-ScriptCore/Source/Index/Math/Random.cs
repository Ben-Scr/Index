using System.Diagnostics;
using System;

namespace Index;
public sealed class Random
{
    private ulong state;

    public Random(ulong seed)
    {
        SetSeed(seed);
    }

    public Random()
    {
        RemoveSeed();
    }

    private ulong NextState()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717UL;
    }

    public void SetSeed(ulong seed)
    {
        // xorshift* has a fixed point at 0 (state stays 0 -> every draw is 0); remap the zero seed.
        state = seed != 0 ? seed : 0x9E3779B97F4A7C15UL;
    }
    public void RemoveSeed()
    {
        ulong t = (ulong)DateTime.Now.Ticks;
        ulong g = (ulong)Guid.NewGuid().GetHashCode();
        ulong s = (ulong)Stopwatch.GetTimestamp();
        SetSeed(t ^ g ^ s);
    }

    public ulong GetSeed() => state;

    public bool NextBool() => NextInt(0, 2) == 0;

    public byte NextByte()
    {
        return (byte)(NextState() & 0xFF);
    }
    public byte NextByte(byte max)
    {
        if (max <= 0) throw new ArgumentOutOfRangeException(nameof(max));
        return (byte)(NextByte() % max);
    }
    public byte NextByte(byte min, byte max)
    {
        if (min >= max) throw new ArgumentOutOfRangeException($"Next({min},{max}) is wrong, min can't be more or equal to max.");
        return (byte)(min + (NextByte() % (max - min)));
    }

    public int NextInt()
    {
        return (int)(NextState() & 0x7FFFFFFF);
    }
    public int NextInt(int max)
    {
        if (max <= 0) throw new ArgumentOutOfRangeException(nameof(max));
        return NextInt() % max;
    }
    public int NextInt(int min, int max)
    {
        if (min >= max) throw new ArgumentOutOfRangeException($"Next({min},{max}) is wrong, min can't be more or equal to max.");
        ulong range = (ulong)((long)max - min);
        return (int)(min + (long)(NextState() % range));
    }

    public double NextDouble()
    {
        // 53-bit mantissa / 2^53 stays in [0,1): worst case 1 - 2^-53, never exactly 1.0.
        return (NextState() >> 11) * (1.0 / 9007199254740992.0);
    }
    public double NextDouble(double max)
    {
        if (max <= 0) throw new ArgumentOutOfRangeException(nameof(max));
        return NextDouble() * max;
    }
    public double NextDouble(double min, double max)
    {
        return min + (NextDouble() * (max - min));
    }

    public float NextFloat()
    {
        // Build from 24 bits directly: (float)NextDouble() would round 1 - 2^-53 up to 1.0f.
        // Worst case 1 - 2^-24 = 0.99999994f, so the result stays in [0,1).
        return (NextState() >> 40) * (1.0f / 16777216.0f);
    }
    public float NextFloat(float max)
    {
        if (max <= 0) throw new ArgumentOutOfRangeException(nameof(max));
        return NextFloat() * max;
    }
    public float NextFloat(float min, float max)
    {
        return min + (NextFloat() * (max - min));
    }

    public Color NextColor() => new Color(NextFloat(), NextFloat(), NextFloat());

    public string NextString(int length = 10, string? charset = null)
    {
        charset ??= TextUtils.Letters;
        int charsetLength = charset.Length;

        string code = string.Empty;

        for (int i = 0; i < length; i++)
            code += charset[NextInt(charsetLength)];

        return code;
    }

    public T Next<T>() where T : IComparable<T>
    {
        if (typeof(T) == typeof(int))
        {
            int result = NextInt();
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(float))
        {
            float result = NextFloat();
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(double))
        {
            double result = NextDouble();
            return (T)(object)result;
        }
        else if(typeof(T) == typeof(byte))
        {
            byte result = NextByte();
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(bool))
        {
            bool result = NextBool();
            return (T)(object)result;
        }
        else if(typeof(T) == typeof(string))
        {
            string result = NextString();
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(Color))
        {
            return (T)(object)new Color(NextFloat(), NextFloat(), NextFloat());
        }

        throw new NotSupportedException($"Type '{typeof(T)}' is not supported.");
    }

    public T Next<T>(T max) where T : IComparable<T>
    {
        if (typeof(T) == typeof(int))
        {
            int result = NextInt(Convert.ToInt32(max));
            return (T)(object)result;
        }

        if (typeof(T) == typeof(float))
        {
            float result = NextFloat(Convert.ToSingle(max));
            return (T)(object)result;
        }

        if (typeof(T) == typeof(double))
        {
            double result = NextDouble(Convert.ToDouble(max));
            return (T)(object)result;
        }

        if (typeof(T) == typeof(byte))
        {
            byte result = NextByte(Convert.ToByte(max));
            return (T)(object)result;
        }

        throw new NotSupportedException($"Type '{typeof(T)}' is not supported.");
    }

    public T Next<T>(T min, T max) where T : IComparable<T>
    {
        if (typeof(T) == typeof(int))
        {
            int result = NextInt(Convert.ToInt32(min), Convert.ToInt32(max));
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(float))
        {
            float result = NextFloat(Convert.ToSingle(min), Convert.ToSingle(max));
            return (T)(object)result;
        }
        else if(typeof(T) == typeof(double))
        {
            double result = NextDouble(Convert.ToDouble(min), Convert.ToDouble(max));
            return (T)(object)result;
        }
        else if (typeof(T) == typeof(byte))
        {
            byte result = NextByte(Convert.ToByte(min), Convert.ToByte(max));
            return (T)(object)result;
        }

        throw new NotSupportedException($"Type '{typeof(T)}' is not supported.");
    }
}
