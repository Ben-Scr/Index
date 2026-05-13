#pragma once

namespace Index {

	enum class IndexErrorCode {
		InvalidArgument,
		NotInitialized,
		AlreadyInitialized,
		FileNotFound,
		InvalidHandle,
		OutOfRange,
		OutOfBounds,
		Overflow,
		NullReference,
		LoadFailed,
		InvalidValue,
		Undefined
	};

} // namespace Index
