#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class ShadowPass : public RenderPassNode {
public:
    explicit ShadowPass(Renderer* renderer);
    ~ShadowPass() override = default;

    [[nodiscard]] const char* getName() const override { return "ShadowPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo