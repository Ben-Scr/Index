#pragma once

// Non-owning view over a C# NativeArray/NativeList/NativeBuffer passed via P/Invoke (pointer + length).
// Lifetime is the C# caller's responsibility — DO NOT free Data; use-after-Dispose is a use-after-free.

#include <cstddef>

namespace Index {

    template <typename T>
    struct NativeArrayView {
        T*  Data   = nullptr;
        int Length = 0;

        constexpr NativeArrayView() = default;
        constexpr NativeArrayView(T* data, int length) noexcept : Data(data), Length(length) {}

        constexpr bool   IsEmpty()        const noexcept { return Length == 0 || Data == nullptr; }
        constexpr T&     operator[](int i) const noexcept { return Data[i]; }
        constexpr T*     begin()          const noexcept { return Data; }
        constexpr T*     end()            const noexcept { return Data + Length; }

        // Bytes occupied by the view (handy for memcpy / memset paths).
        constexpr std::size_t SizeInBytes() const noexcept {
            return static_cast<std::size_t>(Length) * sizeof(T);
        }
    };

} // namespace Index
