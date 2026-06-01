#pragma once

// DEPRECATED: include Forward.hpp or specific component headers instead; this umbrella will become a hard error in a future release.

#if defined(_MSC_VER)
#pragma message("warning: <Components/Components.hpp> is deprecated. Include Forward.hpp or specific component headers; see header for details.")
#elif defined(__GNUC__) || defined(__clang__)
#warning "<Components/Components.hpp> is deprecated. Include Forward.hpp or specific component headers; see header for details."
#endif

#include "Components/Forward.hpp"
#include "Components/General/General.hpp"
#include "Components/UI/UI.hpp"
#include "Components/Tags.hpp"
