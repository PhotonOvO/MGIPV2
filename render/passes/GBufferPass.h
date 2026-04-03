#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class GBufferPass : public RenderPassNode {
public:
    explicit GBufferPass(Renderer* renderer);
    ~GBufferPass() override = default;

    [[nodiscard]] const char* getName() const override { return "GBufferPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

    void setModel(const void* modelData) { m_modelData = modelData; }

private:
    Renderer* m_renderer;
    const void* m_modelData = nullptr;
};

}  // namespace demo