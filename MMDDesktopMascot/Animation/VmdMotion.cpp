#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "VmdMotion.hpp"
#include "BinaryReader.hpp"
#include <windows.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace
{
	std::wstring SjisBytesToW(std::span<const std::uint8_t> bytes)
	{
		int lenA = 0;
		while (lenA < (int)bytes.size() && bytes[lenA] != 0) ++lenA;
		if (lenA == 0) return {};

		int lenW = MultiByteToWideChar(932, 0, (const char*)bytes.data(), lenA, nullptr, 0);
		if (lenW <= 0) return {};
		std::wstring out(lenW, L'\0');
		MultiByteToWideChar(932, 0, (const char*)bytes.data(), lenA, out.data(), lenW);
		return out;
	}

	std::string AsciiZ(std::span<const std::uint8_t> bytes)
	{
		size_t n = 0;
		while (n < bytes.size() && bytes[n] != 0) ++n;
		return std::string(reinterpret_cast<const char*>(bytes.data()), n);
	}

	bool HasOptionalSectionCount(BinaryReader& reader, const char* sectionName)
	{
		const size_t remaining = reader.Remaining();
		if (remaining == 0)
		{
			return false;
		}
		if (remaining < sizeof(std::uint32_t))
		{
			throw std::runtime_error(
				std::string("Truncated VMD optional section header: ") + sectionName);
		}
		return true;
	}
}

bool VmdMotion::Load(const std::filesystem::path& vmdPath)
{
	m_path = vmdPath;

	m_boneKeys.clear();
	m_morphKeys.clear();
	m_cameraKeys.clear();
	m_lightKeys.clear();
	m_shadowKeys.clear();
	m_ikKeys.clear();
	m_boneTracks.clear();
	m_morphTracks.clear();
	m_maxFrame = 0;

	BinaryReader br(vmdPath);

	std::string stage = "begin";
	try
	{
		// header string (30 bytes)
		stage = "header";
		const auto headerBytes = br.ReadSpan(30);
		auto headerStr = AsciiZ(headerBytes);

		const bool isOld = (headerStr == "Vocaloid Motion Data file");
		const bool isNew = (headerStr.find("Vocaloid Motion Data") != std::string::npos);

		if (!isOld && !isNew)
		{
			throw std::runtime_error("Not a VMD file (header mismatch).");
		}

		// model name length differs by version:
		// old: 10 bytes, new: 20 bytes  (per common VMD docs)
		stage = "modelName";
		const size_t modelNameLen = isOld ? 10 : 20;
		(void)br.ReadSpan(modelNameLen);

		// bone key count
		stage = "boneCount";
		auto boneCount = br.Read<std::uint32_t>();

		// sanity check (111 bytes per bone key)
		{
			constexpr size_t BoneRecSize = 15 + 4 + 12 + 16 + 64; // 111
			if (static_cast<size_t>(boneCount) > br.Remaining() / BoneRecSize)
			{
				throw std::runtime_error("Invalid boneCount (file is likely malformed or version mismatch).");
			}
		}

		m_boneKeys.reserve(boneCount);
		stage = "boneKeys";
		for (std::uint32_t i = 0; i < boneCount; ++i)
		{
			BoneKey k{};
			const auto nameBytes = br.ReadSpan(15);
			k.boneName = SjisBytesToW(nameBytes);

			k.frame = br.Read<std::uint32_t>();
			k.tx = br.Read<float>(); k.ty = br.Read<float>(); k.tz = br.Read<float>();
			k.qx = br.Read<float>(); k.qy = br.Read<float>(); k.qz = br.Read<float>(); k.qw = br.Read<float>();

			const auto interpBytes = br.ReadSpan(64);
			std::memcpy(k.interp, interpBytes.data(), 64);

			m_boneKeys.push_back(std::move(k));
		}

		// morph key count
		stage = "morphCount";
		auto morphCount = br.Read<std::uint32_t>();

		// sanity check (23 bytes per morph key)
		{
			constexpr size_t MorphRecSize = 15 + 4 + 4; // 23
			if (static_cast<size_t>(morphCount) > br.Remaining() / MorphRecSize)
			{
				throw std::runtime_error("Invalid morphCount (file is likely malformed or version mismatch).");
			}
		}

		m_morphKeys.reserve(morphCount);
		stage = "morphKeys";
		for (std::uint32_t i = 0; i < morphCount; ++i)
		{
			MorphKey k{};
			const auto nameBytes = br.ReadSpan(15);
			k.morphName = SjisBytesToW(nameBytes);
			k.frame = br.Read<std::uint32_t>();
			k.weight = br.Read<float>();
			m_morphKeys.push_back(std::move(k));
		}

		// camera section (optional if file ends)
		if (!HasOptionalSectionCount(br, "camera"))
		{
			BuildTracks();
			return true;
		}

		stage = "cameraCount";
		auto cameraCount = br.Read<std::uint32_t>();

		// sanity check (61 bytes per camera key)
		{
			constexpr size_t CameraRecSize = 4 + 4 + 12 + 12 + 24 + 4 + 1; // 61
			if (static_cast<size_t>(cameraCount) > br.Remaining() / CameraRecSize)
			{
				throw std::runtime_error("Invalid cameraCount (file is likely malformed or version mismatch).");
			}
		}

		m_cameraKeys.reserve(cameraCount);
		stage = "cameraKeys";
		for (std::uint32_t i = 0; i < cameraCount; ++i)
		{
			CameraKey k{};
			k.frame = br.Read<std::uint32_t>();
			k.distance = br.Read<float>();
			k.posX = br.Read<float>(); k.posY = br.Read<float>(); k.posZ = br.Read<float>();
			k.rotX = br.Read<float>(); k.rotY = br.Read<float>(); k.rotZ = br.Read<float>();
			const auto interpBytes = br.ReadSpan(24);
			std::memcpy(k.interp, interpBytes.data(), 24);
			k.viewAngle = br.Read<std::uint32_t>();
			k.perspective = br.Read<std::uint8_t>() == 0;
			m_cameraKeys.push_back(std::move(k));
		}

		// light section (optional if file ends)
		if (!HasOptionalSectionCount(br, "light"))
		{
			BuildTracks();
			return true;
		}

		stage = "lightCount";
		auto lightCount = br.Read<std::uint32_t>();

		// sanity check (28 bytes per light key)
		{
			constexpr size_t LightRecSize = 4 + 12 + 12; // 28
			if (static_cast<size_t>(lightCount) > br.Remaining() / LightRecSize)
			{
				throw std::runtime_error("Invalid lightCount (file is likely malformed or version mismatch).");
			}
		}

		m_lightKeys.reserve(lightCount);
		stage = "lightKeys";
		for (std::uint32_t i = 0; i < lightCount; ++i)
		{
			LightKey k{};
			k.frame = br.Read<std::uint32_t>();
			k.colorR = br.Read<float>(); k.colorG = br.Read<float>(); k.colorB = br.Read<float>();
			k.posX = br.Read<float>(); k.posY = br.Read<float>(); k.posZ = br.Read<float>();
			m_lightKeys.push_back(std::move(k));
		}

		// shadow section (optional if file ends)
		if (!HasOptionalSectionCount(br, "shadow"))
		{
			BuildTracks();
			return true;
		}

		stage = "shadowCount";
		auto shadowCount = br.Read<std::uint32_t>();

		// sanity check (9 bytes per shadow key)
		{
			constexpr size_t ShadowRecSize = 4 + 1 + 4; // 9
			if (static_cast<size_t>(shadowCount) > br.Remaining() / ShadowRecSize)
			{
				throw std::runtime_error("Invalid shadowCount (file is likely malformed or version mismatch).");
			}
		}

		m_shadowKeys.reserve(shadowCount);
		stage = "shadowKeys";
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			ShadowKey k{};
			k.frame = br.Read<std::uint32_t>();
			k.mode = br.Read<std::uint8_t>();
			k.distance = br.Read<float>();
			m_shadowKeys.push_back(std::move(k));
		}

		// ik section (optional if file ends)
		if (!HasOptionalSectionCount(br, "ik"))
		{
			BuildTracks();
			return true;
		}

		stage = "ikCount";
		auto ikCount = br.Read<std::uint32_t>();
		{
			constexpr size_t IkRecMinimumSize = 4 + 1 + 4;
			if (static_cast<size_t>(ikCount) > br.Remaining() / IkRecMinimumSize)
			{
				throw std::runtime_error("Invalid ikCount (file is likely malformed or version mismatch).");
			}
		}
		m_ikKeys.reserve(ikCount);

		stage = "ikKeys";
		for (std::uint32_t i = 0; i < ikCount; ++i)
		{
			IkKey k{};
			k.frame = br.Read<std::uint32_t>();
			k.show = br.Read<std::uint8_t>() != 0;

			auto ikStateCount = br.Read<std::uint32_t>();
			{
				constexpr size_t IkStateRecSize = 20 + 1;
				if (static_cast<size_t>(ikStateCount) > br.Remaining() / IkStateRecSize)
				{
					throw std::runtime_error("Invalid ikStateCount (file is likely malformed or version mismatch).");
				}
			}
			k.states.reserve(ikStateCount);

			for (std::uint32_t j = 0; j < ikStateCount; ++j)
			{
				IkState state{};
				const auto nameBytes = br.ReadSpan(20);
				state.name = SjisBytesToW(nameBytes);
				state.enabled = br.Read<std::uint8_t>() != 0;
				k.states.push_back(std::move(state));
			}
			m_ikKeys.push_back(std::move(k));
		}

		BuildTracks();
		return true;
	}
	catch (const std::exception& e)
	{
		std::ostringstream oss;
		oss << "VMD parse failed at stage='" << stage
			<< "' pos=" << br.Position()
			<< " remaining=" << br.Remaining()
			<< " : " << e.what();
		throw std::runtime_error(oss.str());
	}
}

void VmdMotion::BuildTracks()
{
	m_boneTracks.clear();
	m_morphTracks.clear();
	m_maxFrame = 0;

	auto sortBone = m_boneKeys;
	auto sortMorph = m_morphKeys;

	std::sort(sortBone.begin(), sortBone.end(), [](const BoneKey& a, const BoneKey& b) {
		if (a.boneName == b.boneName) return a.frame < b.frame;
		return a.boneName < b.boneName;
			  });

	std::sort(sortMorph.begin(), sortMorph.end(), [](const MorphKey& a, const MorphKey& b) {
		if (a.morphName == b.morphName) return a.frame < b.frame;
		return a.morphName < b.morphName;
			  });

	auto pushBoneTrack = [&](std::wstring current, std::vector<BoneKey>&& keys) {
		if (!current.empty() && !keys.empty())
		{
			m_boneTracks.push_back({ std::move(current), std::move(keys) });
		}
		};

	std::wstring currentBone;
	std::vector<BoneKey> currentBoneKeys;
	for (auto& k : sortBone)
	{
		m_maxFrame = std::max(m_maxFrame, k.frame);

		if (currentBone != k.boneName)
		{
			pushBoneTrack(std::move(currentBone), std::move(currentBoneKeys));
			currentBone = k.boneName;
			currentBoneKeys.clear();
		}
		currentBoneKeys.push_back(std::move(k));
	}
	pushBoneTrack(std::move(currentBone), std::move(currentBoneKeys));

	auto pushMorphTrack = [&](std::wstring current, std::vector<MorphKey>&& keys) {
		if (!current.empty() && !keys.empty())
		{
			m_morphTracks.push_back({ std::move(current), std::move(keys) });
		}
		};

	std::wstring currentMorph;
	std::vector<MorphKey> currentMorphKeys;
	for (auto& k : sortMorph)
	{
		m_maxFrame = std::max(m_maxFrame, k.frame);

		if (currentMorph != k.morphName)
		{
			pushMorphTrack(std::move(currentMorph), std::move(currentMorphKeys));
			currentMorph = k.morphName;
			currentMorphKeys.clear();
		}
		currentMorphKeys.push_back(std::move(k));
	}
	pushMorphTrack(std::move(currentMorph), std::move(currentMorphKeys));

	for (const auto& k : m_cameraKeys) m_maxFrame = std::max(m_maxFrame, k.frame);
	for (const auto& k : m_lightKeys)  m_maxFrame = std::max(m_maxFrame, k.frame);
	for (const auto& k : m_shadowKeys) m_maxFrame = std::max(m_maxFrame, k.frame);
	for (const auto& k : m_ikKeys)     m_maxFrame = std::max(m_maxFrame, k.frame);
}
