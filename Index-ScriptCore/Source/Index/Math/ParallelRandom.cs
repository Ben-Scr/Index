using System;

namespace Index;

/// <summary>
/// Thread-safe random facade — each thread owns its own <see cref="Random"/> instance so parallel jobs never race. For deterministic sequences carry a <see cref="Random"/> as a job-struct field instead.
/// </summary>
public static class ParallelRandom
{
    [ThreadStatic] private static Random? s_Random;

    private static Random Instance => s_Random ??= CreateForCurrentThread();

    private static Random CreateForCurrentThread()
    {
        // XOR the managed thread id into the seed so threads constructed in the same tick don't get identical sequences.
        Random r = new Random();
        r.SetSeed(r.GetSeed() ^ (ulong)Environment.CurrentManagedThreadId);
        return r;
    }

    public static void SetSeed(ulong seed) => Instance.SetSeed(seed);
    public static void RemoveSeed() => Instance.RemoveSeed();

    public static bool NextBool() => Instance.NextBool();

    public static byte NextByte() => Instance.NextByte();
    public static byte NextByte(byte max) => Instance.NextByte(max);
    public static byte NextByte(byte min, byte max) => Instance.NextByte(min, max);

    public static int NextInt() => Instance.NextInt();
    public static int NextInt(int max) => Instance.NextInt(max);
    public static int NextInt(int min, int max) => Instance.NextInt(min, max);

    public static float NextFloat() => Instance.NextFloat();
    public static float NextFloat(float max) => Instance.NextFloat(max);
    public static float NextFloat(float min, float max) => Instance.NextFloat(min, max);

    public static double NextDouble() => Instance.NextDouble();
    public static double NextDouble(double max) => Instance.NextDouble(max);
    public static double NextDouble(double min, double max) => Instance.NextDouble(min, max);

    public static string NextString(int length = 10, string? charset = null) => Instance.NextString(length, charset);

    public static T Next<T>() where T : IComparable<T> => Instance.Next<T>();
    public static T Next<T>(T max) where T : IComparable<T> => Instance.Next(max);
    public static T Next<T>(T min, T max) where T : IComparable<T> => Instance.Next(min, max);
}
