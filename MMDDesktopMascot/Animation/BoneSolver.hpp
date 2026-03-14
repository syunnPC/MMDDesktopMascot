#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <DirectXMath.h>
#include "PmxModel.hpp"

struct BonePose
{
	std::unordered_map<std::wstring, DirectX::XMFLOAT3> boneTranslations;
	std::unordered_map<std::wstring, DirectX::XMFLOAT4> boneRotations;
	std::unordered_map<std::wstring, float> morphWeights;
	float frame{};
};

class BoneSolver
{
public:
	static constexpr size_t MaxBones = 1024;

	struct ExternalParentTransform
	{
		DirectX::XMFLOAT3 translation{};
		DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct BoneState
	{
		DirectX::XMFLOAT3 localTranslation{};
		DirectX::XMFLOAT4 localRotation{ 0.0f, 0.0f, 0.0f, 1.0f };

		DirectX::XMFLOAT4X4 localMatrix{};
		DirectX::XMFLOAT4X4 globalMatrix{};
		DirectX::XMFLOAT4X4 skinningMatrix{};
	};

	BoneSolver() = default;

	void Initialize(const PmxModel* model);
	void ApplyPose(const BonePose& pose);
	void SolveIK();
	void UpdateMatrices();
	void UpdateMatrices(bool solveIK);
	void UpdateMatricesBeforePhysics();
	void UpdateMatricesAfterPhysics();

	void GetBoneBounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const;

	const std::vector<DirectX::XMFLOAT4X4>& GetSkinningMatrices() const
	{
		return m_skinningMatrices;
	}

	size_t BoneCount() const
	{
		return m_bones.size();
	}

	// 物理など外部システムが参照/書き戻しするための最小API
	const DirectX::XMFLOAT4X4& GetBoneGlobalMatrix(size_t boneIndex) const;
	const DirectX::XMFLOAT4X4& GetBoneLocalMatrix(size_t boneIndex) const;
	int FindBoneIndex(const std::wstring& boneName) const;
	void SetBoneLocalPose(size_t boneIndex,
						  const DirectX::XMFLOAT3& translation,
						  const DirectX::XMFLOAT4& rotation);
	void ClearExternalParentTransforms();
	void SetExternalParentTransform(std::int32_t key, const ExternalParentTransform& transform);

	// 物理後にIKを回さずスキニング行列だけ更新したい場合に使用
	void UpdateMatricesNoIK();

private:
	enum class UpdatePhase
	{
		All,
		BeforePhysics,
		AfterPhysics
	};

	void CalculateSkinningMatrix(size_t boneIndex);
	void ComputeBindPoseMatrices();
	void RecomputeBounds();
	void UpdateBoneTransforms(UpdatePhase phase);
	void UpdateSkinningMatrices();
	void SolveIKPhase(UpdatePhase phase);

	void SolveIKBone(size_t boneIndex);

	static DirectX::XMVECTOR ClampAngle(DirectX::XMVECTOR euler,
										const DirectX::XMFLOAT3& minAngle,
										const DirectX::XMFLOAT3& maxAngle);

	const PmxModel* m_model{ nullptr };
	std::vector<PmxModel::Bone> m_bones;
	std::vector<BoneState> m_boneStates;
	std::vector<DirectX::XMFLOAT4X4> m_skinningMatrices;
	std::vector<DirectX::XMFLOAT4X4> m_inverseBindMatrices;

	std::unordered_map<std::wstring, size_t> m_boneNameToIndex;

	std::vector<size_t> m_sortedBoneOrder;
	std::vector<std::vector<size_t>> m_boneChildren;

	void BuildSortedBoneOrder();

	// IK制限のEuler連続性用
	std::vector<DirectX::XMFLOAT3> m_lastIkDominantEuler;
	std::vector<uint8_t>           m_hasLastIkDominantEuler;
	std::vector<DirectX::XMFLOAT3> m_lastIkLimitedEuler;
	std::vector<uint8_t>           m_hasLastIkLimitedEuler;
	DirectX::XMFLOAT3 m_cachedBoundsMin{};
	DirectX::XMFLOAT3 m_cachedBoundsMax{};
	bool m_boundsValid{ false };
	std::unordered_map<std::int32_t, ExternalParentTransform> m_externalParentTransforms;
	std::vector<size_t> m_footIkScratch;
	std::vector<size_t> m_toeIkScratch;
	std::vector<size_t> m_otherIkScratch;

	void UpdateBoneTransform(size_t boneIndex);
};
