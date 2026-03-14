#pragma once

#include <windows.h>
#include <DirectXMath.h>

#include "Settings.hpp"

class Camera
{
public:
	void AdjustScale(LightSettings& lightSettings, float delta);
	void AddCameraRotation(float dxPixels, float dyPixels);

	void CacheMatrices(const DirectX::XMMATRIX& model,
					   const DirectX::XMMATRIX& view,
					   const DirectX::XMMATRIX& proj,
					   UINT width,
					   UINT height);

	DirectX::XMFLOAT3 ProjectToScreen(const DirectX::XMFLOAT3& localPos) const;

	float GetYaw() const
	{
		return m_cameraYaw;
	}

	float GetPitch() const
	{
		return m_cameraPitch;
	}

	float GetDistance() const
	{
		return m_cameraDistance;
	}

private:
	float m_cameraYaw{ 0.0f };
	float m_cameraPitch{ 0.0f };
	float m_cameraDistance{ 2.5f };

	DirectX::XMFLOAT4X4 m_lastModelMatrix{};
	DirectX::XMFLOAT4X4 m_lastViewMatrix{};
	DirectX::XMFLOAT4X4 m_lastProjMatrix{};
	UINT m_cachedWidth{};
	UINT m_cachedHeight{};
	bool m_matricesValid{ false };
};
