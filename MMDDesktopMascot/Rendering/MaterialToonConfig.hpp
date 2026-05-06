#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "PmxModel.hpp"

struct MaterialToonOverride
{
	bool hasClass{};
	uint32_t materialClass{};

	bool hasShadowColor{};
	DirectX::XMFLOAT3 shadowColor{};
	float shadowColorWeight{ 1.0f };

	bool hasRimMask{};
	float rimMask{};

	bool hasSkinLightInfluence{};
	float skinLightInfluence{};

	bool hasShadowHueShift{};
	float shadowHueShift{};

	bool hasShadowSaturationScale{};
	float shadowSaturationScale{};

	bool hasShadowValueScale{};
	float shadowValueScale{};

	bool hasSpecularScale{};
	float specularScale{};

	bool hairBandSpecular{};
	bool disableSpecular{};
	bool hasHairSpecCenter{};
	float hairSpecCenter{};
	bool hasHairSpecWidth{};
	float hairSpecWidth{};
	bool hasHairSpecIntensity{};
	float hairSpecIntensity{};
	bool hasHairSpecMask{};
	std::filesystem::path hairSpecMask;

	bool hasEdgeColor{};
	DirectX::XMFLOAT4 edgeColor{};

	bool hasEdgeSize{};
	float edgeSize{};

	bool hasEdgeEnabled{};
	bool edgeEnabled{};

	bool hasAlphaCutout{};
	bool alphaCutout{};
};

class MaterialToonConfig
{
public:
	static MaterialToonConfig LoadForModel(const std::filesystem::path& modelPath);

	const MaterialToonOverride* FindOverride(const PmxModel::Material& material) const;
	bool Empty() const noexcept
	{
		return m_entries.empty() && !m_defaultOverride.has_value();
	}

private:
	struct Entry
	{
		std::wstring key;
		MaterialToonOverride value;
	};

	std::optional<MaterialToonOverride> m_defaultOverride;
	std::vector<Entry> m_entries;
};
