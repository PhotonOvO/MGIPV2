#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class LightCullingPass : public ComputePassNode {
public:
    explicit LightCullingPass(Renderer* renderer);
    ~LightCullingPass() override = default;

    [[nodiscard]] const char* getName() const override { return "LightCulling"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo