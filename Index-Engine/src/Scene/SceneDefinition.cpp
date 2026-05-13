#include "pch.hpp"
#include "Scene/SceneDefinition.hpp"
#include "Scene/Scene.hpp"


namespace Index {
	std::shared_ptr<Scene> SceneDefinition::Instantiate() const {
		std::shared_ptr<Scene> scene(new Scene(m_Name, this, m_IsPersistent));

		for (const auto& factory : m_SystemFactories) {
			auto system = factory();

			if (system) {
				scene->m_Systems.push_back(std::move(system));
			}
			else {
				IDX_LOG_ERROR(IndexErrorCode::LoadFailed, "Failed creating system for scene with name '" +
					m_Name + "'");
			}
		}

		for (const auto& callback : m_InitializeCallbacks) {
			try {
				callback(*scene);
			}
			catch (const std::exception& e) {
				IDX_LOG_ERROR(IndexErrorCode::Undefined, "Exception in initialize callback for scene with name '" +
					m_Name + "': " + e.what());
			}
			catch (...) {
				IDX_LOG_ERROR(IndexErrorCode::Undefined, "Unknown Exception in initialize callback for scene with name '" +
					m_Name + "'");
			}
		}

		return scene;
	}
}