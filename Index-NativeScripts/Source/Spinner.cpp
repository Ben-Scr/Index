#include <Components/General/Transform2DComponent.hpp>
#include <Scripting/NativeScript.hpp>

class Spinner : public Index::NativeScript {
public:
	void Update(float dt) override {
		auto& transform = GetComponent<Index::Transform2DComponent>();
		transform.SetRotation(transform.Rotation + 3.14159f * dt);
	}
};

REGISTER_SCRIPT(Spinner)
