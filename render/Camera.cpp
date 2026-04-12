#include "Camera.h"
#include <algorithm>
#include <cmath>

namespace demo {

void Camera::setClipSpaceConvention(clipspace::BackendConvention backend)
{
    m_projectionConvention = clipspace::getProjectionConvention(backend);
    updateMatrices();
}

void Camera::setPerspective(float fov, float aspectRatio, float nearPlane, float farPlane)
{
    m_fov = fov;
    m_aspectRatio = aspectRatio;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
    updateMatrices();
}

void Camera::setPosition(const glm::vec3& position)
{
    m_position = position;
}

void Camera::setYawPitch(float yaw, float pitch)
{
    m_yaw = yaw;
    m_pitch = pitch;
    updateVectors();
}

void Camera::move(const glm::vec3& delta)
{
    // Move relative to camera orientation
    m_position += m_front * delta.z;   // Forward/backward
    m_position += m_right * delta.x;   // Left/right
    m_position += m_up * delta.y;      // Up/down
}

void Camera::rotate(float deltaYaw, float deltaPitch)
{
    m_yaw += deltaYaw;
    m_pitch += deltaPitch;

    // Clamp pitch to prevent gimbal lock
    constexpr float maxPitch = 89.0f;
    m_pitch = std::clamp(m_pitch, -maxPitch, maxPitch);

    updateVectors();
}

void Camera::update()
{
    updateVectors();
    updateMatrices();
}

void Camera::updateVectors()
{
    // Calculate front vector from Euler angles
    glm::vec3 front;
    front.x = std::cos(glm::radians(m_yaw)) * std::cos(glm::radians(m_pitch));
    front.y = std::sin(glm::radians(m_pitch));
    front.z = std::sin(glm::radians(m_yaw)) * std::cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    // Calculate right and up vectors
    m_right = glm::normalize(glm::cross(m_front, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_up = glm::normalize(glm::cross(m_right, m_front));

    updateMatrices();
}

void Camera::updateMatrices()
{
    m_view = glm::lookAt(m_position, m_position + m_front, m_up);
    m_projection = clipspace::makePerspectiveProjection(
        glm::radians(m_fov), m_aspectRatio, m_nearPlane, m_farPlane, m_projectionConvention);
    m_viewProjection = m_projection * m_view;
}

}  // namespace demo
