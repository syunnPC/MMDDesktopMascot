#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>
#include <DirectXMath.h>
#include <functional>

class BinaryReader;

class PmxModel
{
public:
	struct Header
	{
		float version{};
		std::uint8_t encoding{}; // 0: UTF16LE, 1: UTF8
		std::uint8_t additionalUV{};
		std::uint8_t vertexIndexSize{};
		std::uint8_t textureIndexSize{};
		std::uint8_t materialIndexSize{};
		std::uint8_t boneIndexSize{};
		std::uint8_t morphIndexSize{};
		std::uint8_t rigidIndexSize{};
	};

	// ボーンウェイト用の構造体
	struct VertexWeight
	{
		std::int32_t boneIndices[4]{ -1, -1, -1, -1 };
		float weights[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		std::uint8_t type{}; // 0:BDEF1, 1:BDEF2, 2:BDEF4, 3:SDEF, 4:QDEF

		// SDEF用の追加パラメータ
		DirectX::XMFLOAT3 sdefC{};
		DirectX::XMFLOAT3 sdefR0{};
		DirectX::XMFLOAT3 sdefR1{};
	};

	struct Vertex
	{
		float px{}, py{}, pz{};
		float nx{}, ny{}, nz{};
		float u{}, v{};
		DirectX::XMFLOAT4 additionalUV[4]{};
		VertexWeight weight;
		float edgeScale{};
	};

	struct Material
	{
		std::wstring name;
		std::wstring nameEn;

		float diffuse[4]{};
		float specular[3]{};
		float specularPower{};
		float ambient[3]{};

		std::uint8_t drawFlags{};
		float edgeColor[4]{};
		float edgeSize{};

		int32_t textureIndex{ -1 };
		int32_t sphereTextureIndex{ -1 };
		std::uint8_t sphereMode{};
		std::uint8_t toonFlag{};
		int32_t toonIndex{ -1 };

		std::wstring memo;

		int32_t indexCount{};
		int32_t indexOffset{};

		bool IsDoubleSided() const
		{
			return (drawFlags & 0x01) != 0;
		}

		bool ShouldDrawEdge() const
		{
			return (drawFlags & 0x10) != 0;
		}
	};

	// ボーン構造体
	struct Bone
	{
		std::wstring name;
		std::wstring nameEn;
		DirectX::XMFLOAT3 position{};
		std::int32_t parentIndex{ -1 };
		std::int32_t layer{};
		std::uint16_t flags{};

		// 接続先
		std::int32_t tailBoneIndex{ -1 };
		DirectX::XMFLOAT3 tailOffset{};

		// 付与ボーン
		std::int32_t grantParentIndex{ -1 };
		float grantWeight{};

		// 軸制限
		DirectX::XMFLOAT3 axisDirection{};

		// ローカル軸
		DirectX::XMFLOAT3 localAxisX{};
		DirectX::XMFLOAT3 localAxisZ{};

		// 外部親
		std::int32_t externalParentKey{};

		// IK
		struct IKLink
		{
			std::int32_t boneIndex{ -1 };
			bool hasLimit{ false };
			DirectX::XMFLOAT3 limitMin{};
			DirectX::XMFLOAT3 limitMax{};
		};

		std::int32_t ikTargetIndex{ -1 };
		std::int32_t ikLoopCount{};
		float ikLimitAngle{};
		std::vector<IKLink> ikLinks;

		// ボーンフラグ
		bool IsIK() const
		{
			return (flags & 0x0020) != 0;
		}
		bool CanRotate() const
		{
			return (flags & 0x0002) != 0;
		}
		bool CanTranslate() const
		{
			return (flags & 0x0004) != 0;
		}
		bool HasRotationGrant() const
		{
			return (flags & 0x0100) != 0;
		}
		bool HasTranslationGrant() const
		{
			return (flags & 0x0200) != 0;
		}
		bool IsLocalAxis() const
		{
			return (flags & 0x0800) != 0;
		}
		bool IsAfterPhysics() const
		{
			return (flags & 0x1000) != 0;
		}
		bool IsExternalParent() const
		{
			return (flags & 0x2000) != 0;
		}
	};

	// モーフ構造体
	struct Morph
	{
		enum class Type : std::uint8_t
		{
			Group = 0,
			Vertex = 1,
			Bone = 2,
			UV = 3,
			AdditionalUV1 = 4,
			AdditionalUV2 = 5,
			AdditionalUV3 = 6,
			AdditionalUV4 = 7,
			Material = 8,
			Flip = 9,
			Impulse = 10
		};

		struct GroupOffset
		{
			std::int32_t morphIndex;
			float weight;
		};

		struct VertexOffset
		{
			std::uint32_t vertexIndex;
			DirectX::XMFLOAT3 positionOffset;
		};

		struct BoneOffset
		{
			std::int32_t boneIndex;
			DirectX::XMFLOAT3 translation;
			DirectX::XMFLOAT4 rotation;
		};

		struct UVOffset
		{
			std::uint32_t vertexIndex;
			std::uint8_t channel{};
			DirectX::XMFLOAT4 offset;
		};

		struct MaterialOffset
		{
			std::int32_t materialIndex;
			std::uint8_t operation; // 0:乗算, 1:加算
			DirectX::XMFLOAT4 diffuse;
			DirectX::XMFLOAT3 specular;
			float specularPower;
			DirectX::XMFLOAT3 ambient;
			DirectX::XMFLOAT4 edgeColor;
			float edgeSize;
			DirectX::XMFLOAT4 textureFactor;
			DirectX::XMFLOAT4 sphereTextureFactor;
			DirectX::XMFLOAT4 toonTextureFactor;
		};

		struct FlipOffset
		{
			std::int32_t morphIndex;
			float weight;
		};

		struct ImpulseOffset
		{
			std::int32_t rigidBodyIndex;
			std::uint8_t localFlag;
			DirectX::XMFLOAT3 velocity;
			DirectX::XMFLOAT3 torque;
		};

		std::wstring name;
		std::wstring nameEn;
		std::uint8_t panel;
		Type type;

		// データ保持
		std::vector<GroupOffset> groupOffsets;
		std::vector<VertexOffset> vertexOffsets;
		std::vector<BoneOffset> boneOffsets;
		std::vector<UVOffset> uvOffsets; // UV(0~4)すべてここで扱う
		std::vector<MaterialOffset> materialOffsets;
		std::vector<FlipOffset> flipOffsets;
		std::vector<ImpulseOffset> impulseOffsets;
	};

	// 剛体(PMX)
	struct RigidBody
	{
		enum class ShapeType : std::uint8_t
		{
			Sphere = 0, Box = 1, Capsule = 2
		};
		enum class OperationType : std::uint8_t
		{
			Static = 0, Dynamic = 1, DynamicAndPositionAdjust = 2
		};

		std::wstring name;
		std::wstring nameEn;

		std::int32_t boneIndex{ -1 };
		std::uint8_t groupIndex{};
		// Raw PMX 16-bit collision mask. Runtime resolves the effective Bullet
		// mask semantics when building the physics world.
		std::uint16_t collisionGroupMask{};
		ShapeType shapeType{ ShapeType::Sphere };
		DirectX::XMFLOAT3 shapeSize{};

		DirectX::XMFLOAT3 position{};
		DirectX::XMFLOAT3 rotation{};

		float mass{};
		float linearDamping{};
		float angularDamping{};
		float restitution{};
		float friction{};
		OperationType operation{ OperationType::Static };
	};

	// ジョイント(PMX: Spring 6DOF)
	struct Joint
	{
		enum class OperationType : std::uint8_t
		{
			Springy6Dof = 0
		};

		std::wstring name;
		std::wstring nameEn;

		OperationType operation{ OperationType::Springy6Dof };
		std::int32_t rigidBodyA{ -1 };
		std::int32_t rigidBodyB{ -1 };

		DirectX::XMFLOAT3 position{};
		DirectX::XMFLOAT3 rotation{};
		DirectX::XMFLOAT3 positionLower{};
		DirectX::XMFLOAT3 positionUpper{};
		DirectX::XMFLOAT3 rotationLower{};
		DirectX::XMFLOAT3 rotationUpper{};
		DirectX::XMFLOAT3 springPosition{};
		DirectX::XMFLOAT3 springRotation{};
	};

	struct SoftBody
	{
		struct Config
		{
			float velocityCorrectionFactor{};
			float dampingCoefficient{};
			float dragCoefficient{};
			float liftCoefficient{};
			float pressureCoefficient{};
			float volumeConversationCoefficient{};
			float dynamicFrictionCoefficient{};
			float poseMatchingCoefficient{};
			float rigidContactHardness{};
			float kineticContactHardness{};
			float softContactHardness{};
			float anchorHardness{};
		};

		struct Cluster
		{
			float softVsRigidHardness{};
			float softVsKineticHardness{};
			float softVsSoftHardness{};
			float softVsRigidImpulseSplit{};
			float softVsKineticImpulseSplit{};
			float softVsSoftImpulseSplit{};
		};

		struct Iteration
		{
			std::int32_t velocityIterations{};
			std::int32_t positionIterations{};
			std::int32_t driftIterations{};
			std::int32_t clusterIterations{};
		};

		struct Material
		{
			float linearStiffnessCoefficient{};
			float areaAngularStiffnessCoefficient{};
			float volumeStiffnessCoefficient{};
		};

		struct Anchor
		{
			std::int32_t rigidBodyIndex{ -1 };
			std::uint32_t vertexIndex{};
			std::uint8_t nearMode{};
		};

		std::wstring name;
		std::wstring nameEn;
		std::uint8_t shape{};
		std::int32_t materialIndex{ -1 };
		std::uint8_t groupIndex{};
		std::uint16_t collisionGroupMask{};
		std::uint8_t flags{};
		std::int32_t bendingLinkDistance{};
		std::int32_t clusterCount{};
		float totalMass{};
		float collisionMargin{};
		std::int32_t aeroModel{};
		Config config{};
		Cluster cluster{};
		Iteration iteration{};
		Material material{};
		std::vector<Anchor> anchors;
		std::vector<std::uint32_t> pinVertexIndices;
	};

	using ProgressCallback = std::function<void(float, const wchar_t*)>;

	bool Load(const std::filesystem::path& pmxPath, ProgressCallback onProgress = nullptr);

	const Header& GetHeader() const
	{
		return m_header;
	}
	const std::filesystem::path& Path() const
	{
		return m_path;
	}

	const std::vector<Vertex>& Vertices() const
	{
		return m_vertices;
	}
	const std::vector<uint32_t>& Indices() const
	{
		return m_indices;
	}
	const std::vector<std::filesystem::path>& TexturePaths() const
	{
		return m_textures;
	}
	const std::vector<Material>& Materials() const
	{
		return m_materials;
	}
	const std::vector<Bone>& Bones() const
	{
		return m_bones;
	}
	const std::vector<Morph>& Morphs() const
	{
		return m_morphs;
	}

	const std::vector<RigidBody>& RigidBodies() const
	{
		return m_rigidBodies;
	}
	const std::vector<Joint>& Joints() const
	{
		return m_joints;
	}
	const std::vector<SoftBody>& SoftBodies() const
	{
		return m_softBodies;
	}

	bool HasGeometry() const
	{
		return !m_vertices.empty() && !m_indices.empty();
	}

	void GetBounds(float& minx, float& miny, float& minz,
				   float& maxx, float& maxy, float& maxz) const;

	uint64_t Revision() const
	{
		return m_revision;
	}

private:
	friend class PmxLoader;

	void ResetForLoad(const std::filesystem::path& pmxPath);
	void LoadHeader(BinaryReader& br);
	void SkipModelMetadata(BinaryReader& br) const;
	void LoadVertices(BinaryReader& br);
	void LoadIndices(BinaryReader& br);
	void LoadTextures(BinaryReader& br);
	void LoadMaterials(BinaryReader& br);

	std::wstring ReadPmxText(BinaryReader& br) const;
	int32_t ReadIndexSigned(BinaryReader& br, std::uint8_t size) const;
	uint32_t ReadIndexUnsigned(BinaryReader& br, std::uint8_t size) const;

	VertexWeight ReadVertexWeight(BinaryReader& br) const;
	void LoadBones(BinaryReader& br);
	void LoadMorphs(BinaryReader& br);
	void SkipDisplayFrames(BinaryReader& br);
	void LoadRigidBodies(BinaryReader& br);
	void LoadJoints(BinaryReader& br);
	void LoadSoftBodies(BinaryReader& br);
	void ExpandBounds(const Vertex& vertex) noexcept;

	std::filesystem::path m_path;
	Header m_header{};

	std::vector<Vertex> m_vertices;
	std::vector<uint32_t> m_indices;
	std::vector<std::filesystem::path> m_textures;
	std::vector<Material> m_materials;
	std::vector<Bone> m_bones;
	std::vector<Morph> m_morphs;

	std::vector<RigidBody> m_rigidBodies;
	std::vector<Joint> m_joints;
	std::vector<SoftBody> m_softBodies;

	float m_minx{ +std::numeric_limits<float>::infinity() };
	float m_miny{ +std::numeric_limits<float>::infinity() };
	float m_minz{ +std::numeric_limits<float>::infinity() };
	float m_maxx{ -std::numeric_limits<float>::infinity() };
	float m_maxy{ -std::numeric_limits<float>::infinity() };
	float m_maxz{ -std::numeric_limits<float>::infinity() };

	uint64_t m_revision{};
};
