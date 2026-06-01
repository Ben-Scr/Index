#pragma once

#include <cstring>
#include <typeindex>
#include <typeinfo>

namespace Index {

	class NativeScript;

	class NativeScriptRegistry {
	public:
		using Factory = NativeScript* (*)();

		struct Entry {
			const char* name;
			Factory factory;
			Entry* next;
		};

		// Uses `const std::type_info*` not `std::type_index`: type_index has no default ctor so a value-initialized `TypeEntry entries[256]` array would be ill-formed.
		struct TypeEntry {
			const std::type_info* type;
			const char* name;
			TypeEntry* next;
		};

		inline static Entry* s_Head = nullptr;
		inline static TypeEntry* s_TypeHead = nullptr;

		static void Register(const char* name, Factory factory) {
			if (!name || !factory) {
				return;
			}

			for (Entry* entry = s_Head; entry; entry = entry->next) {
				if (std::strcmp(name, entry->name) == 0) {
					entry->factory = factory;
					return;
				}
			}

			static Entry entries[256];
			static int count = 0;
			if (count < 256) {
				entries[count] = { name, factory, s_Head };
				s_Head = &entries[count++];
			}
		}

		static void RegisterType(const std::type_info& type, const char* name) {
			if (!name) return;
			for (TypeEntry* e = s_TypeHead; e; e = e->next) {
				if (*e->type == type) {
					e->name = name;
					return;
				}
			}
			static TypeEntry entries[256];
			static int count = 0;
			if (count < 256) {
				entries[count] = { &type, name, s_TypeHead };
				s_TypeHead = &entries[count++];
			}
		}

		static const char* NameOfType(const std::type_info& type) {
			for (TypeEntry* e = s_TypeHead; e; e = e->next) {
				if (*e->type == type) return e->name;
			}
			return nullptr;
		}

		static NativeScript* Create(const char* name) {
			if (!name) {
				return nullptr;
			}

			for (auto* e = s_Head; e; e = e->next) {
				if (std::strcmp(name, e->name) == 0) {
					return e->factory();
				}
			}

			return nullptr;
		}

		static bool Has(const char* name) {
			if (!name) {
				return false;
			}

			for (auto* e = s_Head; e; e = e->next) {
				if (std::strcmp(name, e->name) == 0) {
					return true;
				}
			}

			return false;
		}
	};

} // namespace Index
