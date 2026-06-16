#pragma once

#include "Core/Export.hpp"

namespace Index {

	// `Default` is a sentinel used by renderer components (SpriteRenderer / Image):
	// it means "use the texture's own filter" (its .meta/import setting) instead of
	// overriding it. It is never a real sampler value — when no .meta resolves it,
	// sampler creation maps it to Point (see ToBackendFilter / TextureManager), the
	// crisp default for pixel art. MUST stay last: the value is serialized as an
	// int, so appending keeps existing saved values (Point=0 … Anisotropic=3) stable.
	enum class INDEX_API Filter { Point, Bilinear, Trilinear, Anisotropic, Default };

} // namespace Index
