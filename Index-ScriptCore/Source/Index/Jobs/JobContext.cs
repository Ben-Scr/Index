using System;
using Index.Interop;

namespace Index.Jobs;

/// <summary>Blittable per-frame snapshot for parallel jobs: ECB writer, delta time, frame number, and base RNG seed. Construct via <see cref="ForFrame"/> on the main thread.</summary>
public readonly struct JobContext
{
    /// <summary>Thread-safe parallel writer for structural changes.</summary>
    public readonly EntityCommandBuffer.ParallelWriter Ecb;

    /// <summary>Scaled delta-time at snapshot time. See <see cref="Time.DeltaTime"/>.</summary>
    public readonly float DeltaTime;

    /// <summary>Frame counter at snapshot time. See <see cref="Time.FrameCount"/>.</summary>
    public readonly int FrameNumber;

    /// <summary>Base seed for <see cref="GetRng"/>. Zero means derive from frame number; pass an explicit value for bit-identical replay.</summary>
    public readonly uint BaseSeed;

    public JobContext(
        EntityCommandBuffer.ParallelWriter ecb,
        float deltaTime,
        int frameNumber,
        uint baseSeed)
    {
        Ecb = ecb;
        DeltaTime = deltaTime;
        FrameNumber = frameNumber;
        BaseSeed = baseSeed;
    }

    /// <summary>Returns a per-worker deterministic <see cref="JobRandom"/>. Call once per Execute and reuse — re-seeding per draw defeats divergence.</summary>
    public JobRandom GetRng() => JobRandom.ForCurrentWorker(BaseSeed, FrameNumber);

    /// <summary>Snapshot frame state for parallel dispatch. Call on the main thread; safe to copy into worker job structs.</summary>
    public static JobContext ForFrame(
        EntityCommandBuffer.ParallelWriter ecb,
        uint baseSeed = 0u)
    {
        float dt = InternalCalls.Application_GetDeltaTime();
        int frame = InternalCalls.Time_GetFrameCount();

        uint resolvedSeed = baseSeed != 0u
            ? baseSeed
            : unchecked((uint)frame * 2654435761u);
        if (resolvedSeed == 0u) resolvedSeed = 1u; // xorshift cannot start from zero

        return new JobContext(ecb, dt, frame, resolvedSeed);
    }
}

/// <summary>Deterministic per-worker xorshift32 RNG. Construct via <see cref="JobContext.GetRng"/>; outside jobs use <see cref="Index.Math.ParallelRandom"/> instead.</summary>
public struct JobRandom
{
    private uint m_State;

    public static JobRandom ForCurrentWorker(uint baseSeed, int frame)
    {
        uint threadSalt = unchecked((uint)Environment.CurrentManagedThreadId);
        uint frameSalt = unchecked((uint)frame * 0x9E3779B1u);
        uint state = baseSeed ^ threadSalt ^ frameSalt;
        if (state == 0u) state = 1u; // xorshift cannot start from zero
        return new JobRandom { m_State = state };
    }

    /// <summary>Uniform random uint in [0, uint.MaxValue]. Marsaglia xorshift32, 2^32-1 period. Not cryptographic.</summary>
    public uint NextUInt()
    {
        uint x = m_State;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_State = x;
        return x;
    }

    /// <summary>Uniform random float in [0, 1).</summary>
    public float NextFloat01()
    {
        return (NextUInt() >> 8) * (1.0f / 16777216.0f);
    }

    /// <summary>Uniform random float in [min, max).</summary>
    public float NextFloat(float min, float max) => min + (max - min) * NextFloat01();

    /// <summary>Uniform random int in [min, max). Returns <paramref name="min"/> when the range is empty.</summary>
    public int NextInt(int min, int max)
    {
        if (max <= min) return min;
        uint range = unchecked((uint)(max - min));
        return min + (int)(NextUInt() % range);
    }
}
