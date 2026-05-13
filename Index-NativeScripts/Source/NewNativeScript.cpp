#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>

class NewNativeScript : public Index::NativeScript {
public:
	void Start() override
	{
		IDX_NATIVE_LOG_INFO("NewNativeScript started!");
	}

	void Update(float dt) override
	{
		auto& transform = GetComponent<Index::Transform2DComponent>();
		transform.SetPosition({ transform.Position.x + 5.0f * dt, transform.Position.y });
	}
};
REGISTER_SCRIPT(NewNativeScript)
