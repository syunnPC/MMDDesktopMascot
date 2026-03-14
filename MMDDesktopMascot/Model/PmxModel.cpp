#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "PmxModel.hpp"
#include "PmxLoader.hpp"
#include "BinaryReader.hpp"
#include "StringUtil.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	std::wstring U16ToW(const std::u16string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	void EnsureRemaining(BinaryReader& reader, size_t bytes, const char* context)
	{
		if (bytes > reader.Remaining())
		{
			throw std::runtime_error(std::string("PMX truncated while reading ") + context);
		}
	}

	DirectX::XMFLOAT3 ReadFloat3(BinaryReader& reader)
	{
		return {
			reader.Read<float>(),
			reader.Read<float>(),
			reader.Read<float>()
		};
	}

	DirectX::XMFLOAT4 ReadFloat4(BinaryReader& reader)
	{
		return {
			reader.Read<float>(),
			reader.Read<float>(),
			reader.Read<float>(),
			reader.Read<float>()
		};
	}

	bool IsValidIndexSize(std::uint8_t value) noexcept
	{
		return value == 1 || value == 2 || value == 4;
	}
}

void PmxModel::ResetForLoad(const std::filesystem::path& pmxPath)
{
	m_path = pmxPath;
	m_header = {};

	m_vertices.clear();
	m_indices.clear();
	m_textures.clear();
	m_materials.clear();
	m_bones.clear();
	m_morphs.clear();
	m_rigidBodies.clear();
	m_joints.clear();
	m_softBodies.clear();

	m_minx = m_miny = m_minz = +std::numeric_limits<float>::infinity();
	m_maxx = m_maxy = m_maxz = -std::numeric_limits<float>::infinity();
}

void PmxModel::LoadHeader(BinaryReader& br)
{
	EnsureRemaining(br, 4, "signature");
	const auto signature = br.ReadSpan(4);
	if (signature[0] != 'P' || signature[1] != 'M' || signature[2] != 'X' || signature[3] != ' ')
	{
		throw std::runtime_error("Not a PMX file.");
	}

	m_header.version = br.Read<float>();

	const auto headerSize = br.Read<std::uint8_t>();
	if (headerSize < 8)
	{
		throw std::runtime_error("Unsupported PMX header size.");
	}

	m_header.encoding = br.Read<std::uint8_t>();
	m_header.additionalUV = br.Read<std::uint8_t>();
	m_header.vertexIndexSize = br.Read<std::uint8_t>();
	m_header.textureIndexSize = br.Read<std::uint8_t>();
	m_header.materialIndexSize = br.Read<std::uint8_t>();
	m_header.boneIndexSize = br.Read<std::uint8_t>();
	m_header.morphIndexSize = br.Read<std::uint8_t>();
	m_header.rigidIndexSize = br.Read<std::uint8_t>();

	if (m_header.encoding > 1)
	{
		throw std::runtime_error("Unsupported PMX text encoding.");
	}
	if (m_header.additionalUV > 4)
	{
		throw std::runtime_error("Unsupported PMX additional UV count.");
	}
	if (!IsValidIndexSize(m_header.vertexIndexSize) ||
		!IsValidIndexSize(m_header.textureIndexSize) ||
		!IsValidIndexSize(m_header.materialIndexSize) ||
		!IsValidIndexSize(m_header.boneIndexSize) ||
		!IsValidIndexSize(m_header.morphIndexSize) ||
		!IsValidIndexSize(m_header.rigidIndexSize))
	{
		throw std::runtime_error("Unsupported PMX index size.");
	}

	if (headerSize > 8)
	{
		br.Skip(headerSize - 8);
	}

	SkipModelMetadata(br);
}

void PmxModel::SkipModelMetadata(BinaryReader& br) const
{
	(void)ReadPmxText(br);
	(void)ReadPmxText(br);
	(void)ReadPmxText(br);
	(void)ReadPmxText(br);
}

void PmxModel::LoadVertices(BinaryReader& br)
{
	const auto vertexCount = br.Read<std::int32_t>();
	if (vertexCount < 0)
	{
		throw std::runtime_error("Invalid vertexCount.");
	}

	const size_t minVertexBytesPerRecord = static_cast<size_t>(38 + 16 * m_header.additionalUV);
	if (minVertexBytesPerRecord == 0)
	{
		throw std::runtime_error("Invalid PMX vertex record size.");
	}
	if (static_cast<size_t>(vertexCount) > br.Remaining() / minVertexBytesPerRecord)
	{
		throw std::runtime_error("Invalid vertexCount.");
	}

	m_vertices.reserve(static_cast<size_t>(vertexCount));
	const size_t minVertexBytes = static_cast<size_t>(vertexCount) * minVertexBytesPerRecord;
	EnsureRemaining(br, minVertexBytes, "vertex block");

	for (int32_t i = 0; i < vertexCount; ++i)
	{
		Vertex vertex{};
		vertex.px = br.Read<float>();
		vertex.py = br.Read<float>();
		vertex.pz = br.Read<float>();
		vertex.nx = br.Read<float>();
		vertex.ny = br.Read<float>();
		vertex.nz = br.Read<float>();
		vertex.u = br.Read<float>();
		vertex.v = br.Read<float>();

		for (uint8_t uvIndex = 0; uvIndex < m_header.additionalUV; ++uvIndex)
		{
			const DirectX::XMFLOAT4 uv = ReadFloat4(br);
			if (uvIndex < 4)
			{
				vertex.additionalUV[uvIndex] = uv;
			}
		}

		vertex.weight = ReadVertexWeight(br);
		vertex.edgeScale = br.Read<float>();

		ExpandBounds(vertex);
		m_vertices.push_back(vertex);
	}
}

void PmxModel::LoadIndices(BinaryReader& br)
{
	const auto indexCount = br.Read<std::int32_t>();
	if (indexCount < 0 || (indexCount % 3) != 0)
	{
		throw std::runtime_error("Invalid indexCount.");
	}
	if (m_header.vertexIndexSize == 0 ||
		static_cast<size_t>(indexCount) > br.Remaining() / static_cast<size_t>(m_header.vertexIndexSize))
	{
		throw std::runtime_error("Invalid indexCount.");
	}

	m_indices.reserve(static_cast<size_t>(indexCount));
	EnsureRemaining(br, static_cast<size_t>(indexCount) * m_header.vertexIndexSize, "indices");

	for (int32_t i = 0; i < indexCount; ++i)
	{
		const uint32_t index = ReadIndexUnsigned(br, m_header.vertexIndexSize);
		if (index >= static_cast<uint32_t>(m_vertices.size()))
		{
			throw std::runtime_error("Vertex index out of range.");
		}

		m_indices.push_back(index);
	}
}

void PmxModel::LoadTextures(BinaryReader& br)
{
	const auto textureCount = br.Read<std::int32_t>();
	if (textureCount < 0)
	{
		throw std::runtime_error("Invalid textureCount.");
	}

	m_textures.reserve(static_cast<size_t>(textureCount));
	const std::filesystem::path modelDirectory = m_path.parent_path();
	for (int32_t i = 0; i < textureCount; ++i)
	{
		m_textures.push_back(modelDirectory / ReadPmxText(br));
	}
}

void PmxModel::LoadMaterials(BinaryReader& br)
{
	const auto materialCount = br.Read<std::int32_t>();
	if (materialCount < 0)
	{
		throw std::runtime_error("Invalid materialCount.");
	}

	m_materials.reserve(static_cast<size_t>(materialCount));
	size_t runningIndexOffset = 0;

	for (int32_t i = 0; i < materialCount; ++i)
	{
		Material material{};
		material.name = ReadPmxText(br);
		material.nameEn = ReadPmxText(br);

		for (float& value : material.diffuse) value = br.Read<float>();
		for (float& value : material.specular) value = br.Read<float>();
		material.specularPower = br.Read<float>();
		for (float& value : material.ambient) value = br.Read<float>();

		material.drawFlags = br.Read<std::uint8_t>();
		for (float& value : material.edgeColor) value = br.Read<float>();
		material.edgeSize = br.Read<float>();

		material.textureIndex = ReadIndexSigned(br, m_header.textureIndexSize);
		material.sphereTextureIndex = ReadIndexSigned(br, m_header.textureIndexSize);
		material.sphereMode = br.Read<std::uint8_t>();
		material.toonFlag = br.Read<std::uint8_t>();
		material.toonIndex = (material.toonFlag == 0)
			? ReadIndexSigned(br, m_header.textureIndexSize)
			: static_cast<int32_t>(br.Read<std::uint8_t>());

		material.memo = ReadPmxText(br);
		material.indexCount = br.Read<std::int32_t>();
		if (material.indexCount < 0)
		{
			throw std::runtime_error("Invalid material indexCount.");
		}
		if (runningIndexOffset + static_cast<size_t>(material.indexCount) > m_indices.size())
		{
			throw std::runtime_error("Material indexCount total mismatch.");
		}
		if (runningIndexOffset > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
		{
			throw std::runtime_error("Material index offset overflow.");
		}

		material.indexOffset = static_cast<int32_t>(runningIndexOffset);
		runningIndexOffset += static_cast<size_t>(material.indexCount);
		m_materials.push_back(std::move(material));
	}

	if (runningIndexOffset != m_indices.size())
	{
		throw std::runtime_error("Material indexCount total mismatch.");
	}
}

std::wstring PmxModel::ReadPmxText(BinaryReader& br) const
{
	if (m_header.encoding == 0)
	{
		return U16ToW(br.ReadStringUtf16LeWithLength());
	}
	if (m_header.encoding == 1)
	{
		return StringUtil::Utf8ToWide(br.ReadStringUtf8WithLength());
	}

	throw std::runtime_error("Unknown PMX encoding.");
}

int32_t PmxModel::ReadIndexSigned(BinaryReader& br, std::uint8_t size) const
{
	switch (size)
	{
		case 1: return static_cast<int32_t>(br.Read<std::int8_t>());
		case 2: return static_cast<int32_t>(br.Read<std::int16_t>());
		case 4: return br.Read<std::int32_t>();
		default: throw std::runtime_error("Unsupported index size.");
	}
}

uint32_t PmxModel::ReadIndexUnsigned(BinaryReader& br, std::uint8_t size) const
{
	switch (size)
	{
		case 1: return static_cast<uint32_t>(br.Read<std::uint8_t>());
		case 2: return static_cast<uint32_t>(br.Read<std::uint16_t>());
		case 4: return br.Read<std::uint32_t>();
		default: throw std::runtime_error("Unsupported index size.");
	}
}

PmxModel::VertexWeight PmxModel::ReadVertexWeight(BinaryReader& br) const
{
	VertexWeight weight{};
	weight.type = br.Read<std::uint8_t>();

	auto readBoneIndex = [&]() -> std::int32_t
		{
			return ReadIndexSigned(br, m_header.boneIndexSize);
		};

	switch (weight.type)
	{
		case 0:
			weight.boneIndices[0] = readBoneIndex();
			weight.weights[0] = 1.0f;
			break;
		case 1:
			weight.boneIndices[0] = readBoneIndex();
			weight.boneIndices[1] = readBoneIndex();
			weight.weights[0] = br.Read<float>();
			weight.weights[1] = 1.0f - weight.weights[0];
			break;
		case 2:
		case 4:
			for (auto& boneIndex : weight.boneIndices) boneIndex = readBoneIndex();
			for (auto& value : weight.weights) value = br.Read<float>();
			break;
		case 3:
			weight.boneIndices[0] = readBoneIndex();
			weight.boneIndices[1] = readBoneIndex();
			weight.weights[0] = br.Read<float>();
			weight.weights[1] = 1.0f - weight.weights[0];
			weight.sdefC = ReadFloat3(br);
			weight.sdefR0 = ReadFloat3(br);
			weight.sdefR1 = ReadFloat3(br);
			break;
		default:
			throw std::runtime_error("Unknown weight type.");
	}

	return weight;
}

void PmxModel::LoadBones(BinaryReader& br)
{
	const auto boneCount = br.Read<std::int32_t>();
	if (boneCount < 0)
	{
		throw std::runtime_error("Invalid boneCount.");
	}

	m_bones.reserve(static_cast<size_t>(boneCount));
	for (int32_t i = 0; i < boneCount; ++i)
	{
		Bone bone{};
		bone.name = ReadPmxText(br);
		bone.nameEn = ReadPmxText(br);
		bone.position = ReadFloat3(br);
		bone.parentIndex = ReadIndexSigned(br, m_header.boneIndexSize);
		bone.layer = br.Read<std::int32_t>();
		bone.flags = br.Read<std::uint16_t>();

		if ((bone.flags & 0x0001) != 0)
		{
			bone.tailBoneIndex = ReadIndexSigned(br, m_header.boneIndexSize);
		}
		else
		{
			bone.tailOffset = ReadFloat3(br);
		}

		if ((bone.flags & 0x0100) != 0 || (bone.flags & 0x0200) != 0)
		{
			bone.grantParentIndex = ReadIndexSigned(br, m_header.boneIndexSize);
			bone.grantWeight = br.Read<float>();
		}

		if ((bone.flags & 0x0400) != 0)
		{
			bone.axisDirection = ReadFloat3(br);
		}

		if ((bone.flags & 0x0800) != 0)
		{
			bone.localAxisX = ReadFloat3(br);
			bone.localAxisZ = ReadFloat3(br);
		}

		if ((bone.flags & 0x2000) != 0)
		{
			bone.externalParentKey = br.Read<std::int32_t>();
		}

		if ((bone.flags & 0x0020) != 0)
		{
			bone.ikTargetIndex = ReadIndexSigned(br, m_header.boneIndexSize);
			bone.ikLoopCount = br.Read<std::int32_t>();
			bone.ikLimitAngle = br.Read<float>();

			const auto linkCount = br.Read<std::int32_t>();
			if (linkCount < 0)
			{
				throw std::runtime_error("Invalid IK linkCount.");
			}

			bone.ikLinks.reserve(static_cast<size_t>(linkCount));
			for (int32_t j = 0; j < linkCount; ++j)
			{
				Bone::IKLink link{};
				link.boneIndex = ReadIndexSigned(br, m_header.boneIndexSize);
				link.hasLimit = br.Read<std::uint8_t>() != 0;
				if (link.hasLimit)
				{
					link.limitMin = ReadFloat3(br);
					link.limitMax = ReadFloat3(br);
				}

				bone.ikLinks.push_back(link);
			}
		}

		m_bones.push_back(std::move(bone));
	}
}

bool PmxModel::Load(const std::filesystem::path& pmxPath, ProgressCallback onProgress)
{
	return PmxLoader::LoadModel(pmxPath, *this, onProgress);
}

void PmxModel::GetBounds(float& minx, float& miny, float& minz,
						 float& maxx, float& maxy, float& maxz) const
{
	minx = m_minx;
	miny = m_miny;
	minz = m_minz;
	maxx = m_maxx;
	maxy = m_maxy;
	maxz = m_maxz;
}

void PmxModel::LoadMorphs(BinaryReader& br)
{
	const int32_t morphCount = br.Read<std::int32_t>();
	if (morphCount < 0) throw std::runtime_error("Invalid morphCount.");

	m_morphs.clear();
	m_morphs.reserve(static_cast<size_t>(morphCount));

	for (int32_t i = 0; i < morphCount; ++i)
	{
		Morph morph{};
		morph.name = ReadPmxText(br);
		morph.nameEn = ReadPmxText(br);
		morph.panel = br.Read<std::uint8_t>();
		morph.type = static_cast<Morph::Type>(br.Read<std::uint8_t>());

		const int32_t offsetCount = br.Read<std::int32_t>();
		if (offsetCount < 0) throw std::runtime_error("Invalid morph offsetCount.");

		switch (morph.type)
		{
			case Morph::Type::Group: morph.groupOffsets.reserve(offsetCount); break;
			case Morph::Type::Vertex: morph.vertexOffsets.reserve(offsetCount); break;
			case Morph::Type::Bone: morph.boneOffsets.reserve(offsetCount); break;
			case Morph::Type::UV:
			case Morph::Type::AdditionalUV1:
			case Morph::Type::AdditionalUV2:
			case Morph::Type::AdditionalUV3:
			case Morph::Type::AdditionalUV4:
				morph.uvOffsets.reserve(offsetCount);
				break;
			case Morph::Type::Material: morph.materialOffsets.reserve(offsetCount); break;
			case Morph::Type::Flip: morph.flipOffsets.reserve(offsetCount); break;
			case Morph::Type::Impulse: morph.impulseOffsets.reserve(offsetCount); break;
		}

		for (int32_t k = 0; k < offsetCount; ++k)
		{
			switch (morph.type)
			{
				case Morph::Type::Group:
				{
					Morph::GroupOffset offset{};
					offset.morphIndex = ReadIndexSigned(br, m_header.morphIndexSize);
					offset.weight = br.Read<float>();
					morph.groupOffsets.push_back(offset);
					break;
				}
				case Morph::Type::Vertex:
				{
					Morph::VertexOffset offset{};
					offset.vertexIndex = ReadIndexUnsigned(br, m_header.vertexIndexSize);
					offset.positionOffset = ReadFloat3(br);
					morph.vertexOffsets.push_back(offset);
					break;
				}
				case Morph::Type::Bone:
				{
					Morph::BoneOffset offset{};
					offset.boneIndex = ReadIndexSigned(br, m_header.boneIndexSize);
					offset.translation = ReadFloat3(br);
					offset.rotation = ReadFloat4(br);
					morph.boneOffsets.push_back(offset);
					break;
				}
				case Morph::Type::UV:
				case Morph::Type::AdditionalUV1:
				case Morph::Type::AdditionalUV2:
				case Morph::Type::AdditionalUV3:
				case Morph::Type::AdditionalUV4:
				{
					Morph::UVOffset offset{};
					offset.vertexIndex = ReadIndexUnsigned(br, m_header.vertexIndexSize);
					offset.channel = static_cast<std::uint8_t>(
						morph.type == Morph::Type::UV
							? 0
							: (static_cast<int>(morph.type) - static_cast<int>(Morph::Type::AdditionalUV1) + 1));
					offset.offset = ReadFloat4(br);
					morph.uvOffsets.push_back(offset);
					break;
				}
				case Morph::Type::Material:
				{
					Morph::MaterialOffset offset{};
					offset.materialIndex = ReadIndexSigned(br, m_header.materialIndexSize);
					offset.operation = br.Read<std::uint8_t>();
					offset.diffuse = ReadFloat4(br);
					offset.specular = ReadFloat3(br);
					offset.specularPower = br.Read<float>();
					offset.ambient = ReadFloat3(br);
					offset.edgeColor = ReadFloat4(br);
					offset.edgeSize = br.Read<float>();
					offset.textureFactor = ReadFloat4(br);
					offset.sphereTextureFactor = ReadFloat4(br);
					offset.toonTextureFactor = ReadFloat4(br);
					morph.materialOffsets.push_back(offset);
					break;
				}
				case Morph::Type::Flip:
				{
					Morph::FlipOffset offset{};
					offset.morphIndex = ReadIndexSigned(br, m_header.morphIndexSize);
					offset.weight = br.Read<float>();
					morph.flipOffsets.push_back(offset);
					break;
				}
				case Morph::Type::Impulse:
				{
					Morph::ImpulseOffset offset{};
					offset.rigidBodyIndex = ReadIndexSigned(br, m_header.rigidIndexSize);
					offset.localFlag = br.Read<std::uint8_t>();
					offset.velocity = ReadFloat3(br);
					offset.torque = ReadFloat3(br);
					morph.impulseOffsets.push_back(offset);
					break;
				}
				default:
					throw std::runtime_error("Unknown morph type.");
			}
		}

		m_morphs.push_back(std::move(morph));
	}
}

void PmxModel::SkipDisplayFrames(BinaryReader& br)
{
	const int32_t frameCount = br.Read<std::int32_t>();
	if (frameCount < 0) throw std::runtime_error("Invalid frameCount.");

	for (int32_t i = 0; i < frameCount; ++i)
	{
		(void)ReadPmxText(br);
		(void)ReadPmxText(br);
		(void)br.Read<std::uint8_t>();

		const int32_t elementCount = br.Read<std::int32_t>();
		if (elementCount < 0) throw std::runtime_error("Invalid frame elemCount.");

		for (int32_t k = 0; k < elementCount; ++k)
		{
			const std::uint8_t elementType = br.Read<std::uint8_t>();
			switch (elementType)
			{
				case 0:
					(void)ReadIndexSigned(br, m_header.boneIndexSize);
					break;
				case 1:
					(void)ReadIndexSigned(br, m_header.morphIndexSize);
					break;
				default:
					throw std::runtime_error("Unknown frame element type.");
			}
		}
	}
}

void PmxModel::LoadRigidBodies(BinaryReader& br)
{
	m_rigidBodies.clear();

	const int32_t rigidCount = br.Read<std::int32_t>();
	if (rigidCount < 0) throw std::runtime_error("Invalid rigidCount.");
	m_rigidBodies.reserve(static_cast<size_t>(rigidCount));

	for (int32_t i = 0; i < rigidCount; ++i)
	{
		RigidBody rigidBody{};
		rigidBody.name = ReadPmxText(br);
		rigidBody.nameEn = ReadPmxText(br);
		rigidBody.boneIndex = ReadIndexSigned(br, m_header.boneIndexSize);
		rigidBody.groupIndex = br.Read<std::uint8_t>();
		rigidBody.collisionGroupMask = br.Read<std::uint16_t>();
		rigidBody.shapeType = static_cast<RigidBody::ShapeType>(br.Read<std::uint8_t>());
		rigidBody.shapeSize = ReadFloat3(br);
		rigidBody.position = ReadFloat3(br);
		rigidBody.rotation = ReadFloat3(br);
		rigidBody.mass = br.Read<float>();
		rigidBody.linearDamping = br.Read<float>();
		rigidBody.angularDamping = br.Read<float>();
		rigidBody.restitution = br.Read<float>();
		rigidBody.friction = br.Read<float>();
		rigidBody.operation = static_cast<RigidBody::OperationType>(br.Read<std::uint8_t>());
		m_rigidBodies.push_back(std::move(rigidBody));
	}
}

void PmxModel::LoadJoints(BinaryReader& br)
{
	m_joints.clear();

	const int32_t jointCount = br.Read<std::int32_t>();
	if (jointCount < 0) throw std::runtime_error("Invalid jointCount.");
	m_joints.reserve(static_cast<size_t>(jointCount));

	for (int32_t i = 0; i < jointCount; ++i)
	{
		Joint joint{};
		joint.name = ReadPmxText(br);
		joint.nameEn = ReadPmxText(br);
		joint.operation = static_cast<Joint::OperationType>(br.Read<std::uint8_t>());
		joint.rigidBodyA = ReadIndexSigned(br, m_header.rigidIndexSize);
		joint.rigidBodyB = ReadIndexSigned(br, m_header.rigidIndexSize);
		joint.position = ReadFloat3(br);
		joint.rotation = ReadFloat3(br);
		joint.positionLower = ReadFloat3(br);
		joint.positionUpper = ReadFloat3(br);
		joint.rotationLower = ReadFloat3(br);
		joint.rotationUpper = ReadFloat3(br);
		joint.springPosition = ReadFloat3(br);
		joint.springRotation = ReadFloat3(br);
		m_joints.push_back(std::move(joint));
	}
}

void PmxModel::LoadSoftBodies(BinaryReader& br)
{
	m_softBodies.clear();
	if (br.Remaining() == 0)
	{
		return;
	}

	const int32_t softBodyCount = br.Read<std::int32_t>();
	if (softBodyCount < 0)
	{
		throw std::runtime_error("Invalid softBodyCount.");
	}
	m_softBodies.reserve(static_cast<size_t>(softBodyCount));

	for (int32_t i = 0; i < softBodyCount; ++i)
	{
		SoftBody softBody{};
		softBody.name = ReadPmxText(br);
		softBody.nameEn = ReadPmxText(br);
		softBody.shape = br.Read<std::uint8_t>();
		softBody.materialIndex = ReadIndexSigned(br, m_header.materialIndexSize);
		softBody.groupIndex = br.Read<std::uint8_t>();
		softBody.collisionGroupMask = br.Read<std::uint16_t>();
		softBody.flags = br.Read<std::uint8_t>();
		softBody.bendingLinkDistance = br.Read<std::int32_t>();
		softBody.clusterCount = br.Read<std::int32_t>();
		softBody.totalMass = br.Read<float>();
		softBody.collisionMargin = br.Read<float>();
		softBody.aeroModel = static_cast<std::int32_t>(br.Read<std::int8_t>());

		softBody.config.velocityCorrectionFactor = br.Read<float>();
		softBody.config.dampingCoefficient = br.Read<float>();
		softBody.config.dragCoefficient = br.Read<float>();
		softBody.config.liftCoefficient = br.Read<float>();
		softBody.config.pressureCoefficient = br.Read<float>();
		softBody.config.volumeConversationCoefficient = br.Read<float>();
		softBody.config.dynamicFrictionCoefficient = br.Read<float>();
		softBody.config.poseMatchingCoefficient = br.Read<float>();
		softBody.config.rigidContactHardness = br.Read<float>();
		softBody.config.kineticContactHardness = br.Read<float>();
		softBody.config.softContactHardness = br.Read<float>();
		softBody.config.anchorHardness = br.Read<float>();

		softBody.cluster.softVsRigidHardness = br.Read<float>();
		softBody.cluster.softVsKineticHardness = br.Read<float>();
		softBody.cluster.softVsSoftHardness = br.Read<float>();
		softBody.cluster.softVsRigidImpulseSplit = br.Read<float>();
		softBody.cluster.softVsKineticImpulseSplit = br.Read<float>();
		softBody.cluster.softVsSoftImpulseSplit = br.Read<float>();

		softBody.iteration.velocityIterations = br.Read<std::int32_t>();
		softBody.iteration.positionIterations = br.Read<std::int32_t>();
		softBody.iteration.driftIterations = br.Read<std::int32_t>();
		softBody.iteration.clusterIterations = br.Read<std::int32_t>();

		softBody.material.linearStiffnessCoefficient = br.Read<float>();
		softBody.material.areaAngularStiffnessCoefficient = br.Read<float>();
		softBody.material.volumeStiffnessCoefficient = br.Read<float>();

		const int32_t anchorCount = br.Read<std::int32_t>();
		if (anchorCount < 0)
		{
			throw std::runtime_error("Invalid softBody anchorCount.");
		}
		softBody.anchors.reserve(static_cast<size_t>(anchorCount));
		for (int32_t anchorIndex = 0; anchorIndex < anchorCount; ++anchorIndex)
		{
			SoftBody::Anchor anchor{};
			anchor.rigidBodyIndex = ReadIndexSigned(br, m_header.rigidIndexSize);
			anchor.vertexIndex = ReadIndexUnsigned(br, m_header.vertexIndexSize);
			anchor.nearMode = br.Read<std::uint8_t>();
			softBody.anchors.push_back(anchor);
		}

		const int32_t pinVertexCount = br.Read<std::int32_t>();
		if (pinVertexCount < 0)
		{
			throw std::runtime_error("Invalid softBody pinVertexCount.");
		}
		softBody.pinVertexIndices.reserve(static_cast<size_t>(pinVertexCount));
		for (int32_t pinIndex = 0; pinIndex < pinVertexCount; ++pinIndex)
		{
			softBody.pinVertexIndices.push_back(ReadIndexUnsigned(br, m_header.vertexIndexSize));
		}

		m_softBodies.push_back(std::move(softBody));
	}
}

void PmxModel::ExpandBounds(const Vertex& vertex) noexcept
{
	m_minx = std::min(m_minx, vertex.px);
	m_miny = std::min(m_miny, vertex.py);
	m_minz = std::min(m_minz, vertex.pz);
	m_maxx = std::max(m_maxx, vertex.px);
	m_maxy = std::max(m_maxy, vertex.py);
	m_maxz = std::max(m_maxz, vertex.pz);
}
