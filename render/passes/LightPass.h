#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class LightPass : public RenderPassNode {
public:
    explicit LightPass(Renderer* renderer);
    ~LightPass() override = default;

    [[nodiscard]] const char* getName() const override { return "LightPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo