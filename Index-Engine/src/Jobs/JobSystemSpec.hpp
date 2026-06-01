#pragma once

namespace Index {

	struct JobSystemSpec {
		// -1 = auto: hardware_concurrency()-1 clamped [2,16]; with INDEX_WITH_SCRIPTING further capped at max(2,cores-2) for .NET ThreadPool headroom.
		int WorkerCount = -1;
	};

}
