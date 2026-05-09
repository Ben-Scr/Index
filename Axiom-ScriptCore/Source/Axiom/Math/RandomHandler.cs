using System;

namespace Axiom;
public static class RandomHandler
{
    private static Random random = new Random();

    public static void SetSeed(ulong seed) => random.SetSeed(seed);
    public static void RemoveSeed() => random.RemoveSeed();
    public static bool NextBool() => random.NextBool();
    public static byte NextByte() => random.NextByte();
    public static int NextInt(int min, int max) => random.NextInt(min, max);
    public static double NextDouble(double min, double max) => random.NextDouble(min, max);
    public static string NextString(int length = 10, string? charset = null) => random.NextString(length, charset);

    public static T Next<T>(T min, T max) where T : IComparable<T> => random.Next(min, max);
    public static T Next<T>() where T : IComparable<T> => random.Next<T>();
}
