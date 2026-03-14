#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

class VmdMotion
{
public:
	struct BoneKey
	{
		std::wstring boneName;
		std::uint32_t frame{};
		float tx{}, ty{}, tz{};
		float qx{}, qy{}, qz{}, qw{};
		std::uint8_t interp[64]{};
	};

	struct MorphKey
	{
		std::wstring morphName;
		std::uint32_t frame{};
		float weight{};
	};

	struct CameraKey
	{
		std::uint32_t frame{};
		float distance{};
		float posX{}, posY{}, posZ{};
		float rotX{}, rotY{}, rotZ{};
		std::uint8_t interp[24]{};
		std::uint32_t viewAngle{};
		bool perspective{};
	};

	struct LightKey
	{
		std::uint32_t frame{};
		float colorR{}, colorG{}, colorB{};
		float posX{}, posY{}, posZ{};
	};

	struct ShadowKey
	{
		std::uint32_t frame{};
		std::uint8_t mode{};
		float distance{};
	};

	struct IkState
	{
		std::wstring name;
		bool enabled{};
	};

	struct IkKey
	{
		std::uint32_t frame{};
		bool show{};
		std::vector<IkState> states;
	};

	struct BoneTrack
	{
		std::wstring name;
		std::vector<BoneKey> keys;
	};

	struct MorphTrack
	{
		std::wstring name;
		std::vector<MorphKey> keys;
	};

	bool Load(const std::filesystem::path& vmdPath);

	const std::vector<BoneKey>& BoneKeys() const
	{
		return m_boneKeys;
	}
	const std::vector<MorphKey>& MorphKeys() const
	{
		return m_morphKeys;
	}
	const std::vector<BoneTrack>& BoneTracks() const
	{
		return m_boneTracks;
	}
	const std::vector<MorphTrack>& MorphTracks() const
	{
		return m_morphTracks;
	}

	const std::vector<CameraKey>& CameraKeys() const
	{
		return m_cameraKeys;
	}

	const std::vector<LightKey>& LightKeys() const
	{
		return m_lightKeys;
	}

	const std::vector<ShadowKey>& ShadowKeys() const
	{
		return m_shadowKeys;
	}

	const std::vector<IkKey>& IkKeys() const
	{
		return m_ikKeys;
	}

	uint32_t MaxFrame() const
	{
		return m_maxFrame;
	}

private:
	void BuildTracks();
	std::filesystem::path m_path;
	std::vector<BoneKey> m_boneKeys;
	std::vector<MorphKey> m_morphKeys;
	std::vector<CameraKey> m_cameraKeys;
	std::vector<LightKey> m_lightKeys;
	std::vector<ShadowKey> m_shadowKeys;
	std::vector<IkKey> m_ikKeys;

	std::vector<BoneTrack> m_boneTracks;
	std::vector<MorphTrack> m_morphTracks;
	uint32_t m_maxFrame{};
};
