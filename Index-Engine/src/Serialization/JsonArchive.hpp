#pragma once

#include "Serialization/Archive.hpp"
#include "Serialization/Json.hpp"

#include <vector>

namespace Index {
	class ComponentRegistry;
}

namespace Index::Serialization {

	// Backed by Index::Json::Value; on-disk layout is backward-compatible with the legacy hand-coded serializer. Missing fields on read are silently skipped (forward-compatible evolution).
	class INDEX_API JsonArchive final : public IArchive {
	public:
		// Write mode: the archive mutates `target` directly, adding per-component
		// member objects as components serialize. `registry` resolves component
		// nameHash → serializedName for the JSON key.
		static JsonArchive ForWriting(Json::Value& target, const ComponentRegistry& registry);

		// Read mode: the archive reads from `source` without mutating it.
		static JsonArchive ForReading(const Json::Value& source, const ComponentRegistry& registry);

		std::uint16_t ComponentVersion() const override { return m_CurrentComponentVersion; }

		void BeginComponent(std::uint64_t nameHash, std::uint16_t version) override;
		void EndComponent() override;

		void Field(std::string_view name, bool& v) override;
		void Field(std::string_view name, std::int32_t& v) override;
		void Field(std::string_view name, std::int64_t& v) override;
		void Field(std::string_view name, std::uint32_t& v) override;
		void Field(std::string_view name, std::uint64_t& v) override;
		void Field(std::string_view name, float& v) override;
		void Field(std::string_view name, double& v) override;
		void Field(std::string_view name, std::string& v) override;
		void Field(std::string_view name, UUID& v) override;
		void Field(std::string_view name, Color& v) override;

		void Object(std::string_view name, void* userData,
			void (*fn)(IArchive&, void*)) override;
		void Array(std::string_view name, std::uint32_t& count,
			void* userData,
			void (*fn)(IArchive&, std::uint32_t, void*)) override;

	private:
		JsonArchive(ArchiveMode mode, Json::Value* target, const Json::Value* source,
			const ComponentRegistry* registry);

		Json::Value* CurrentWrite() { return m_ScopeStack.empty() ? nullptr : m_ScopeStack.back().write; }
		const Json::Value* CurrentRead() const { return m_ScopeStack.empty() ? nullptr : m_ScopeStack.back().read; }

		struct Scope {
			Json::Value* write = nullptr;       // valid in write mode
			const Json::Value* read = nullptr;  // valid in read mode
		};

		const ComponentRegistry* m_Registry = nullptr;
		std::vector<Scope> m_ScopeStack;
		std::uint16_t m_CurrentComponentVersion = 1;
	};

} // namespace Index::Serialization
