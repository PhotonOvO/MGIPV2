#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class ForwardPass : public RenderPassNode {
public:
    explicit ForwardPass(Renderer* renderer);
    ~ForwardPass() override = default;

    [[nodiscard]] const char* getName() const override { return "ForwardPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo