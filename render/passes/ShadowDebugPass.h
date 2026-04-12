#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

// Debug pass to draw shadow frustum wireframe as 3D lines
class ShadowDebugPass : public RenderPassNode {
public:
    explicit ShadowDebugPass(Renderer* renderer);
    ~ShadowDebugPass() override = default;

    [[nodiscard]] const char* getName() const override { return "ShadowDebugPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;

    // Vertex structure for debug lines
    struct DebugVertex {
        float position[3];
        float color[4];
    };
};

}  // namespace demo