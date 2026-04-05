#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace demo {

class Camera
{
public:
    Camera() = default;

    // Configuration
    void setPerspective(float fov, float aspectRatio, float nearPlane, float farPlane);
    void setPosition(const glm::vec3& position);
    void setYawPitch(float yaw, float pitch);

    // Y-flip configuration for different RHI backends:
    // - Vulkan/D3D12: NDC Y-axis points down, need flip (flipY = true, default)
    // - Metal: NDC Y-axis points up, no flip needed (flipY = false)
    void setFlipY(bool flipY) { m_flipY = flipY; }

    // Input interfaces (abstract, platform-agnostic)
    void move(const glm::vec3& delta);              // Move in camera-relative space
    void rotate(float deltaYaw, float deltaPitch);  // Rotate view (degrees)

    // Per-frame update
    void update();

    // Getters for rendering
    const glm::mat4& getViewMatrix() const { return m_view; }
    const glm::mat4& getProjectionMatrix() const { return m_projection; }
    const glm::mat4& getViewProjectionMatrix() const { return m_viewProjection; }
    const glm::vec3& getPosition() const { return m_position; }

private:
    // Camera state
    glm::vec3 m_position{0.0f, 0.0f, 3.0f};
    float m_yaw{-90.0f};    // Yaw (left-right), initial facing -Z
    float m_pitch{0.0f};    // Pitch (up-down)

    // Projection parameters
    float m_fov{45.0f};
    float m_aspectRatio{16.0f / 9.0f};
    float m_nearPlane{0.1f};
    float m_farPlane{100.0f};
    bool m_flipY{true};  // Y-flip for Vulkan/D3D12 (default), false for Metal

    // Derived vectors
    glm::vec3 m_front{0.0f, 0.0f, -1.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
    glm::vec3 m_right{1.0f, 0.0f, 0.0f};

    // Computed matrices
    glm::mat4 m_view{1.0f};
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_viewProjection{1.0f};

    void updateVectors();
    void updateMatrices();
};

}  // namespace demo