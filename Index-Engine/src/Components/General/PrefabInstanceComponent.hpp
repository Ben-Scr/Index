#pragma once

#include "Collections/Ids.hpp"

namespace Index {

	struct PrefabInstanceComponent {
		AssetGUID PrefabGUID = AssetGUID(0);
		uint64_t SourceEntityId = 0;
	};

}
