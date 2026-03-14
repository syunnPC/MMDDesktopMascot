#include "Camera.hpp"

#include <algorithm>
#include <cmath>

void Camera::AdjustScale(LightSettings& lightSettings, float delta)
{
	lightSettings.modelScale += delta;
	lightSettings.modelScale = std::clamp(lightSettings.modelScale, 0.1f, 8.75f);
}

void Camera::AddCameraRotation(float dxPixels, float dyPixels)
{
	constexpr float sensitivity = 0.005f;

	m_cameraYaw += dxPixels * sensitivity;
	m_cameraPitch += dyPixels * sensitivity;

	const float limit = DirectX::XM_PIDIV2 - 0.05f;
	m_cameraPitch = std::clamp(m_cameraPitch, -limit, limit);
}

void Camera::CacheMatrices(const DirectX::XMMATRIX& model,
						   const DirectX::XMMATRIX& view,
						   const DirectX::XMMATRIX& proj,
						   UINT width,
						   UINT height)
{
	DirectX::XMStoreFloat4x4(&m_lastModelMatrix, model);
	DirectX::XMStoreFloat4x4(&m_lastViewMatrix, view);
	DirectX::XMStoreFloat4x4(&m_lastProjMatrix, proj);
	m_cachedWidth = width;
	m_cachedHeight = height;
	m_matricesValid = true;
}

DirectX::XMFLOAT3 Camera::ProjectToScreen(const DirectX::XMFLOAT3& localPos) const
{
	using namespace DirectX;
	if (!m_matricesValid || m_cachedWidth == 0 || m_cachedHeight == 0)
	{
		return { 0.0f, 0.0f, 0.0f };
	}

	const XMMATRIX model = XMLoadFloat4x4(&m_lastModelMatrix);
	const XMMATRIX view = XMLoadFloat4x4(&m_lastViewMatrix);
	const XMMATRIX proj = XMLoadFloat4x4(&m_lastProjMatrix);

	XMVECTOR local = XMLoadFloat3(&localPos);
	local = XMVectorSetW(local, 1.0f);

	const XMVECTOR world = XMVector3TransformCoord(local, model);
	const XMVECTOR clip = XMVector3Transform(world, view * proj);

	float w = XMVectorGetW(clip);
	if (w < 0.001f)
	{
		w = 0.001f;
	}

	const XMVECTOR ndc = XMVectorScale(clip, 1.0f / w);

	const float x = (XMVectorGetX(ndc) + 1.0f) * 0.5f * static_cast<float>(m_cachedWidth);
	const float y = (1.0f - XMVectorGetY(ndc)) * 0.5f * static_cast<float>(m_cachedHeight);

	return { x, y, w };
}
