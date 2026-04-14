#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

// Debug pass to draw CSM cascade frustum wireframes as 3D lines
// Draws 4 cascade frustums color-coded: Red (c0), Green (c1), Blue (c2), Cyan (c3)
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