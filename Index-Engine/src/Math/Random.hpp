#pragma once
#include "Core/Export.hpp"
#include <random>
#include <cstdint>

namespace Index {
    struct Color;

    class INDEX_API Random {
    public:
        Random() = delete;

        static bool NextBool();
        static Color NextColor();

        static std::uint8_t NextByte(std::uint8_t min, std::uint8_t max);
        static std::uint8_t NextByte(std::uint8_t max);

        static float NextFloat();
        static float NextFloat(float max);
        static float NextFloat(float min, float max);

        static double NextDouble();
        static double NextDouble(double max);
        static double NextDouble(double min, double max);

        static int NextInt();
        static  int NextInt(int max);
        static int NextInt(int min, int max);

        // Seeds the calling thread's generator. Each thread has its own
        // generator (lazy-seeded from std::random_device on first use), so
        // SetSeed only affects the thread that calls it — exactly what most
        // gameplay code wants (deterministic per-thread sequences).
        static void SetSeed(uint32_t seed);
    };

}