#include "BoneSolver.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <functional>
#include <cmath>
#include <string>

using namespace DirectX;

namespace
{
	constexpr bool kDisableFootIk = false;
	constexpr bool kDisableToeIk = false;
	constexpr float kMaxIkStepRad = 0.35f;
	constexpr float kMaxKneeDeltaPerFrameRad = 0.65f;
	constexpr size_t kMaxIkUpdateChainDepth = 16;

	// クォータニオン -> オイラー角 (XYZ順)
	DirectX::XMFLOAT3 QuaternionToEulerXYZ(DirectX::FXMVECTOR q)
	{
		DirectX::XMFLOAT4 f{};
		DirectX::XMStoreFloat4(&f, q);

		// X-axis rotation
		float sinr_cosp = 2.0f * (f.w * f.x + f.y * f.z);
		float cosr_cosp = 1.0f - 2.0f * (f.x * f.x + f.y * f.y);
		float rx = std::atan2(sinr_cosp, cosr_cosp);

		// Y-axis rotation
		float sinp = 2.0f * (f.w * f.y - f.z * f.x);
		float ry = 0.0f;
		if (std::abs(sinp) >= 1.0f)
			ry = std::copysign(DirectX::XM_PIDIV2, sinp);
		else
			ry = std::asin(sinp);

		// Z-axis rotation
		float siny_cosp = 2.0f * (f.w * f.z + f.x * f.y);
		float cosy_cosp = 1.0f - 2.0f * (f.y * f.y + f.z * f.z);
		float rz = std::atan2(siny_cosp, cosy_cosp);

		return { rx, ry, rz };
	}

	// オイラー角 (XYZ順) -> クォータニオン
	DirectX::XMVECTOR EulerXYZToQuaternion(DirectX::FXMVECTOR euler)
	{
		DirectX::XMFLOAT3 e;
		DirectX::XMStoreFloat3(&e, euler);

		const float hx = e.x * 0.5f;
		const float hy = e.y * 0.5f;
		const float hz = e.z * 0.5f;

		const float cx = std::cos(hx);
		const float sx = std::sin(hx);
		const float cy = std::cos(hy);
		const float sy = std::sin(hy);
		const float cz = std::cos(hz);
		const float sz = std::sin(hz);

		DirectX::XMFLOAT4 q{};
		q.w = cx * cy * cz + sx * sy * sz;
		q.x = sx * cy * cz - cx * sy * sz;
		q.y = cx * sy * cz + sx * cy * sz;
		q.z = cx * cy * sz - sx * sy * cz;

		return DirectX::XMLoadFloat4(&q);
	}

	// 角度 angle を、reference から ±π 以内の表現に巻き戻す
	float WrapAngleNear(float angle, float reference)
	{
		const float twoPi = DirectX::XM_2PI;
		float diff = angle - reference;

		diff = std::fmod(diff + DirectX::XM_PI, twoPi);
		if (diff < 0.0f) diff += twoPi;
		diff -= DirectX::XM_PI;

		return reference + diff;
	}

	// PMXの角度値（度数法かもしれない）をラジアンに正規化
	DirectX::XMFLOAT3 MaybeDegreesToRadians(const DirectX::XMFLOAT3& v)
	{
		float ax = std::fabs(v.x);
		float ay = std::fabs(v.y);
		float az = std::fabs(v.z);
		float maxAbs = std::max({ ax, ay, az });

		// 値が大きすぎる場合は度数法とみなして変換
		if (maxAbs > DirectX::XM_PI * 2.2f && maxAbs < DirectX::XM_PI * 360.0f)
		{
			const float s = DirectX::XM_PI / 180.0f;
			return { v.x * s, v.y * s, v.z * s };
		}
		return v;
	}

	float MaybeDegreesToRadians(float v)
	{
		float av = std::fabs(v);
		if (av == 0.0f) return v;
		if (av > DirectX::XM_PI * 2.2f && av < DirectX::XM_PI * 360.0f)
		{
			return v * (DirectX::XM_PI / 180.0f);
		}
		return v;
	}

	// 0.5rad(28.6度) を超える値は「度」とみなしてラジアンに変換する（既にラジアンの小値はそのまま）。
	float NormalizeIkUnitAngle(float v)
	{
		float av = std::fabs(v);
		if (!(av > 0.0f) || !std::isfinite(av)) return 0.0f;

		// PMX内部はラジアンが基本（表示は度のことが多い）。
		// ただし一部ツールが「度の数値」をそのまま格納するケースがあるため、
		// 2π を大きく超える値だけ度→ラジアンへ変換する（2.0 や 4.0 はラジアンとして扱う）。
		if (av > DirectX::XM_2PI && av <= 360.0f)
		{
			av = av * (DirectX::XM_PI / 180.0f);
		}



		// 1ステップ角を上限で抑える（過大だとスナップしやすい）
		av = std::min(av, kMaxIkStepRad);
		return av;
	}

	inline bool IsToeIKName(const std::wstring& name)
	{
		return (name.find(L"つま先ＩＫ") != std::wstring::npos) || (name.find(L"つま先IK") != std::wstring::npos);
	}
	inline bool IsFootIKName(const std::wstring& name)
	{
		if (IsToeIKName(name)) return false;
		return (name.find(L"足ＩＫ") != std::wstring::npos) || (name.find(L"足IK") != std::wstring::npos);
	}

	DirectX::XMMATRIX BuildGlobalMatrix(
		const PmxModel::Bone& bone,
		const std::vector<PmxModel::Bone>& bones,
		const std::vector<BoneSolver::BoneState>& boneStates,
		DirectX::FXMMATRIX localMatrix)
	{
		using namespace DirectX;

		if (bone.parentIndex >= 0 && bone.parentIndex < static_cast<int32_t>(bones.size()))
		{
			const auto& parentBone = bones[bone.parentIndex];
			const auto& parentState = boneStates[bone.parentIndex];
			const XMVECTOR bonePos = XMLoadFloat3(&bone.position);
			const XMVECTOR parentPos = XMLoadFloat3(&parentBone.position);
			const XMVECTOR relativePos = XMVectorSubtract(bonePos, parentPos);
			const XMMATRIX parentGlobal = XMLoadFloat4x4(&parentState.globalMatrix);
			return localMatrix * XMMatrixTranslationFromVector(relativePos) * parentGlobal;
		}

		return localMatrix * XMMatrixTranslationFromVector(XMLoadFloat3(&bone.position));
	}

	// クォータニオンの指数（回転を weight 倍する）。Slerp(identity, q, t) は t<0 で崩れるためこちらを使う。
	DirectX::XMVECTOR QuaternionPow(DirectX::FXMVECTOR qIn, float t)
	{
		using namespace DirectX;

		XMVECTOR q = XMQuaternionNormalize(qIn);

		// 同一回転の符号反転を正規化（w>=0）して連続性を上げる
		if (XMVectorGetW(q) < 0.0f)
		{
			q = XMVectorNegate(q);
		}

		// q = [v*sin(a), cos(a)]  (a = θ/2)
		float w = XMVectorGetW(q);
		w = std::clamp(w, -1.0f, 1.0f);

		const float a = std::acos(w);
		const float sinA = std::sin(a);

		// ほぼ単位（角度0）
		if (std::fabs(sinA) < 1.0e-8f)
		{
			return XMQuaternionIdentity();
		}

		XMVECTOR v = XMVectorSet(XMVectorGetX(q), XMVectorGetY(q), XMVectorGetZ(q), 0.0f);
		XMVECTOR axis = XMVectorScale(v, 1.0f / sinA); // 正規化軸

		const float a2 = a * t;
		const float sinA2 = std::sin(a2);
		const float cosA2 = std::cos(a2);

		XMVECTOR out = XMVectorSet(
			XMVectorGetX(axis) * sinA2,
			XMVectorGetY(axis) * sinA2,
			XMVectorGetZ(axis) * sinA2,
			cosA2);

		return XMQuaternionNormalize(out);
	}


	inline DirectX::XMVECTOR ClampIKRotationRobust(
		DirectX::XMVECTOR q,
		const DirectX::XMFLOAT3& limitMinIn,
		const DirectX::XMFLOAT3& limitMaxIn)
	{
		DirectX::XMFLOAT3 limitMin = MaybeDegreesToRadians(limitMinIn);
		DirectX::XMFLOAT3 limitMax = MaybeDegreesToRadians(limitMaxIn);

		// 現在の回転を XYZオイラー角に変換
		DirectX::XMFLOAT3 e = QuaternionToEulerXYZ(q);

		// 各軸ごとにクランプ処理を行う
		auto processAxis = [&](float angle, float minLim, float maxLim) -> float {
			if (std::abs(maxLim - minLim) < 1.0e-3f && std::abs(minLim) < 1.0e-3f)
			{
				// 強制的に 0 にする（軸ブレ防止）
				return 0.0f;
			}

			// それ以外は、範囲の中心に近い位相を選んでクランプ
			float center = (minLim + maxLim) * 0.5f;
			float a = WrapAngleNear(angle, center);
			return std::clamp(a, minLim, maxLim);
			};

		e.x = processAxis(e.x, limitMin.x, limitMax.x);
		e.y = processAxis(e.y, limitMin.y, limitMax.y);
		e.z = processAxis(e.z, limitMin.z, limitMax.z);

		// 再びクォータニオンに戻す
		return DirectX::XMQuaternionNormalize(EulerXYZToQuaternion(DirectX::XMLoadFloat3(&e)));
	}

	// axisXOnly 制限向け: クォータニオンからX軸ツイスト角を抽出（[-π, π]）
	float ExtractTwistAngleX(DirectX::FXMVECTOR q)
	{
		DirectX::XMFLOAT4 f{};
		DirectX::XMStoreFloat4(&f, DirectX::XMQuaternionNormalize(q));

		// X軸まわり成分のみ（ツイスト）を取り出す
		const float twistLenSq = f.x * f.x + f.w * f.w;
		if (!std::isfinite(twistLenSq) || twistLenSq <= 1.0e-12f)
		{
			return 0.0f;
		}

		DirectX::XMVECTOR twist = DirectX::XMVectorSet(f.x, 0.0f, 0.0f, f.w);
		twist = DirectX::XMQuaternionNormalize(twist);

		DirectX::XMFLOAT4 t{};
		DirectX::XMStoreFloat4(&t, twist);

		return 2.0f * std::atan2(t.x, t.w);
	}

}

void BoneSolver::Initialize(const PmxModel* model)
{
	m_model = model;

	m_bones.clear();
	m_boneStates.clear();
	m_skinningMatrices.clear();
	m_inverseBindMatrices.clear();
	m_boneNameToIndex.clear();
	m_sortedBoneOrder.clear();
	m_boneChildren.clear();
	m_footIkScratch.clear();
	m_toeIkScratch.clear();
	m_otherIkScratch.clear();

	m_lastIkDominantEuler.clear();
	m_hasLastIkDominantEuler.clear();
	m_lastIkLimitedEuler.clear();
	m_hasLastIkLimitedEuler.clear();
	m_boundsValid = false;
	m_externalParentTransforms.clear();

	if (!model) return;

	m_bones = model->Bones();

	const size_t n = m_bones.size();

	m_boneStates.resize(n);
	m_skinningMatrices.resize(n);
	m_inverseBindMatrices.resize(n);

	m_lastIkDominantEuler.assign(n, { 0.0f, 0.0f, 0.0f });
	m_hasLastIkDominantEuler.assign(n, 0);
	m_lastIkLimitedEuler.assign(n, { 0.0f, 0.0f, 0.0f });
	m_hasLastIkLimitedEuler.assign(n, 0);
	m_boneChildren.assign(n, {});

	m_boneNameToIndex.reserve(n);
	for (size_t i = 0; i < n; ++i)
	{
		m_boneNameToIndex[m_bones[i].name] = i;

		m_boneStates[i].localTranslation = { 0.0f, 0.0f, 0.0f };
		m_boneStates[i].localRotation = { 0.0f, 0.0f, 0.0f, 1.0f };

		DirectX::XMStoreFloat4x4(&m_boneStates[i].localMatrix, DirectX::XMMatrixIdentity());
		DirectX::XMStoreFloat4x4(&m_boneStates[i].globalMatrix, DirectX::XMMatrixIdentity());

		DirectX::XMStoreFloat4x4(&m_skinningMatrices[i], DirectX::XMMatrixIdentity());
		DirectX::XMStoreFloat4x4(&m_inverseBindMatrices[i], DirectX::XMMatrixIdentity());
	}

	for (size_t i = 0; i < n; ++i)
	{
		const int parentIndex = m_bones[i].parentIndex;
		if (parentIndex >= 0 && parentIndex < static_cast<int>(n))
		{
			m_boneChildren[parentIndex].push_back(i);
		}
	}

	BuildSortedBoneOrder();
	ComputeBindPoseMatrices();
}

void BoneSolver::BuildSortedBoneOrder()
{
	m_sortedBoneOrder.clear();
	m_sortedBoneOrder.reserve(m_bones.size());

	std::vector<size_t> indices(m_bones.size());
	for (size_t i = 0; i < m_bones.size(); ++i) indices[i] = i;

	std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
		const bool aAfterPhysics = m_bones[a].IsAfterPhysics();
		const bool bAfterPhysics = m_bones[b].IsAfterPhysics();
		if (aAfterPhysics != bAfterPhysics)
		{
			return !aAfterPhysics && bAfterPhysics;
		}
		if (m_bones[a].layer != m_bones[b].layer)
		{
			return m_bones[a].layer < m_bones[b].layer;
		}
		return a < b;
					 });

	m_sortedBoneOrder = std::move(indices);
}

void BoneSolver::UpdateBoneTransforms(UpdatePhase phase)
{
	for (size_t idx : m_sortedBoneOrder)
	{
		const bool afterPhysics = m_bones[idx].IsAfterPhysics();
		if (phase == UpdatePhase::BeforePhysics && afterPhysics)
		{
			continue;
		}
		if (phase == UpdatePhase::AfterPhysics && !afterPhysics)
		{
			continue;
		}
		UpdateBoneTransform(idx);
	}
}

void BoneSolver::ApplyPose(const BonePose& pose)
{
	m_boundsValid = false;
	const int n = static_cast<int>(m_boneStates.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if(n >= 256)
#endif
	for (int i = 0; i < n; ++i)
	{
		m_boneStates[i].localTranslation = { 0.0f, 0.0f, 0.0f };
		m_boneStates[i].localRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
	}

	for (const auto& [name, trans] : pose.boneTranslations)
	{
		auto it = m_boneNameToIndex.find(name);
		if (it != m_boneNameToIndex.end())
		{
			m_boneStates[it->second].localTranslation = trans;
		}
	}

	for (const auto& [name, rot] : pose.boneRotations)
	{
		auto it = m_boneNameToIndex.find(name);
		if (it != m_boneNameToIndex.end())
		{
			m_boneStates[it->second].localRotation = rot;
		}
	}
}

void BoneSolver::CalculateSkinningMatrix(size_t boneIndex)
{
	auto& state = m_boneStates[boneIndex];

	XMMATRIX globalMat = XMLoadFloat4x4(&state.globalMatrix);
	XMMATRIX bindInverse = XMLoadFloat4x4(&m_inverseBindMatrices[boneIndex]);

	XMMATRIX skinningMat = bindInverse * globalMat;
	XMStoreFloat4x4(&state.skinningMatrix, skinningMat);
	XMStoreFloat4x4(&m_skinningMatrices[boneIndex], skinningMat);
}

void BoneSolver::ComputeBindPoseMatrices()
{
	const int boneCount = static_cast<int>(m_bones.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if(boneCount >= 256)
#endif
	for (int i = 0; i < boneCount; ++i)
	{
		XMStoreFloat4x4(&m_boneStates[i].localMatrix, XMMatrixIdentity());
	}

	for (size_t idx : m_sortedBoneOrder)
	{
		auto& state = m_boneStates[idx];
		const XMMATRIX localMat = XMLoadFloat4x4(&state.localMatrix);
		const XMMATRIX globalMat = BuildGlobalMatrix(m_bones[idx], m_bones, m_boneStates, localMat);
		XMStoreFloat4x4(&state.globalMatrix, globalMat);
	}

#ifdef _OPENMP
#pragma omp parallel for schedule(static) if(boneCount >= 256)
#endif
	for (int i = 0; i < boneCount; ++i)
	{
		XMMATRIX bindMat = XMLoadFloat4x4(&m_boneStates[i].globalMatrix);
		XMMATRIX inv = XMMatrixInverse(nullptr, bindMat);
		XMStoreFloat4x4(&m_inverseBindMatrices[i], inv);
	}
}

void BoneSolver::SolveIKPhase(UpdatePhase phase)
{
	// IKを依存関係と並列化の可否に基づいて分類
	auto& footIKs = m_footIkScratch;
	auto& toeIKs = m_toeIkScratch;
	auto& otherIKs = m_otherIkScratch;
	footIKs.clear();
	toeIKs.clear();
	otherIKs.clear();
	if (footIKs.capacity() < 2)
	{
		footIKs.reserve(2);
	}
	if (toeIKs.capacity() < 2)
	{
		toeIKs.reserve(2);
	}
	if (otherIKs.capacity() < 8)
	{
		otherIKs.reserve(8);
	}

	// m_sortedBoneOrder（親順）を走査して分類
	// これにより、各リスト内でも基本的な親順（ID順）が保たれる
	for (size_t idx : m_sortedBoneOrder)
	{
		const auto& bone = m_bones[idx];
		if (!bone.IsIK()) continue;
		const bool afterPhysics = bone.IsAfterPhysics();
		if (phase == UpdatePhase::BeforePhysics && afterPhysics)
		{
			continue;
		}
		if (phase == UpdatePhase::AfterPhysics && !afterPhysics)
		{
			continue;
		}

		if (IsToeIKName(bone.name))
		{
			toeIKs.push_back(idx);
		}
		else if (IsFootIKName(bone.name))
		{
			footIKs.push_back(idx);
		}
		else
		{
			otherIKs.push_back(idx);
		}
	}

	if constexpr (kDisableFootIk) footIKs.clear();
	if constexpr (kDisableToeIk) toeIKs.clear();

	for (size_t idx : otherIKs)
	{
		SolveIKBone(idx);
	}

	int nFoot = (int)footIKs.size();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) if(nFoot > 1)
#endif
	for (int i = 0; i < nFoot; ++i)
	{
		SolveIKBone(footIKs[i]);
	}

	int nToe = (int)toeIKs.size();
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) if(nToe > 1)
#endif
	for (int i = 0; i < nToe; ++i)
	{
		SolveIKBone(toeIKs[i]);
	}
}

void BoneSolver::SolveIK()
{
	SolveIKPhase(UpdatePhase::All);
}

XMVECTOR BoneSolver::ClampAngle(XMVECTOR euler,
								const XMFLOAT3& minAngle,
								const XMFLOAT3& maxAngle)
{
	DirectX::XMFLOAT3 limitMin = MaybeDegreesToRadians(minAngle);
	DirectX::XMFLOAT3 limitMax = MaybeDegreesToRadians(maxAngle);

	DirectX::XMFLOAT3 e;
	DirectX::XMStoreFloat3(&e, euler);

	auto processAxis = [&](float angle, float minLim, float maxLim) -> float {
		if (std::abs(maxLim - minLim) < 1.0e-3f && std::abs(minLim) < 1.0e-3f) return 0.0f;
		float center = (minLim + maxLim) * 0.5f;
		float a = WrapAngleNear(angle, center);
		return std::clamp(a, minLim, maxLim);
		};

	e.x = processAxis(e.x, limitMin.x, limitMax.x);
	e.y = processAxis(e.y, limitMin.y, limitMax.y);
	e.z = processAxis(e.z, limitMin.z, limitMax.z);

	return XMLoadFloat3(&e);
}

void BoneSolver::UpdateBoneTransform(size_t boneIndex)
{
	const auto& bone = m_bones[boneIndex];
	auto& state = m_boneStates[boneIndex];

	XMVECTOR translation = XMLoadFloat3(&state.localTranslation);
	XMVECTOR rotation = XMLoadFloat4(&state.localRotation);

	if ((bone.HasRotationGrant() || bone.HasTranslationGrant()) &&
		bone.grantParentIndex >= 0 &&
		bone.grantParentIndex < static_cast<int32_t>(m_bones.size()) &&
		bone.grantParentIndex != static_cast<int32_t>(boneIndex))
	{
		const auto& grantState = m_boneStates[bone.grantParentIndex];

		if (bone.HasRotationGrant())
		{
			XMVECTOR grantRot = XMLoadFloat4(&grantState.localRotation);
			XMVECTOR grantRotScaled = QuaternionPow(grantRot, bone.grantWeight);
			rotation = XMQuaternionMultiply(rotation, grantRotScaled);
			rotation = XMQuaternionNormalize(rotation);
		}

		if (bone.HasTranslationGrant())
		{
			XMVECTOR grantTrans = XMLoadFloat3(&grantState.localTranslation);
			XMVECTOR grantTransScaled = XMVectorScale(grantTrans, bone.grantWeight);
			translation = XMVectorAdd(translation, grantTransScaled);
		}
	}

	XMMATRIX localMat = XMMatrixRotationQuaternion(rotation) *
		XMMatrixTranslationFromVector(translation);
	XMStoreFloat4x4(&state.localMatrix, localMat);

	const XMMATRIX globalMat = BuildGlobalMatrix(bone, m_bones, m_boneStates, localMat);
	XMMATRIX finalGlobal = globalMat;
	if (bone.IsExternalParent())
	{
		const auto it = m_externalParentTransforms.find(bone.externalParentKey);
		if (it != m_externalParentTransforms.end())
		{
			const XMVECTOR externalTranslation = XMLoadFloat3(&it->second.translation);
			const XMVECTOR externalRotation = XMLoadFloat4(&it->second.rotation);
			const XMMATRIX externalMatrix =
				XMMatrixRotationQuaternion(externalRotation) *
				XMMatrixTranslationFromVector(externalTranslation);
			finalGlobal = finalGlobal * externalMatrix;
		}
	}
	XMStoreFloat4x4(&state.globalMatrix, finalGlobal);
}

void BoneSolver::SolveIKBone(size_t boneIndex)
{
	const auto& ikBone = m_bones[boneIndex];
	if (!ikBone.IsIK() || ikBone.ikTargetIndex < 0) return;

	const size_t targetIdx = static_cast<size_t>(ikBone.ikTargetIndex);
	if (targetIdx >= m_bones.size()) return;

	// IKループ設定
	float limitAngle = NormalizeIkUnitAngle(ikBone.ikLimitAngle);
	if (limitAngle <= 0.0f) limitAngle = DirectX::XM_PI;
	const int loopCount = ikBone.ikLoopCount;

	auto UpdateToTargetFrom = [&](size_t changedIdx) -> bool
		{
			std::array<int, kMaxIkUpdateChainDepth> chain{};
			size_t chainCount = 0;
			for (int childCursor = m_bones[targetIdx].parentIndex;
				childCursor >= 0 && chainCount < chain.size();
				childCursor = m_bones[static_cast<size_t>(childCursor)].parentIndex)
			{
				if (childCursor == static_cast<int>(changedIdx))
				{
					UpdateBoneTransform(changedIdx);
					for (size_t i = chainCount; i > 0; --i)
					{
						UpdateBoneTransform(static_cast<size_t>(chain[i - 1]));
					}
					UpdateBoneTransform(targetIdx);
					return true;
				}

				chain[chainCount++] = childCursor;
			}

			return false;
		};

	// --- 解析的2ボーンIK（膝/肘などの1軸制限チェーン） ---
	// 2ボーン + 中間が明確な1軸制限(例: 膝)のケースは、CCD反復より解析解の方が安定しやすい。
	auto TrySolveAnalyticTwoBoneHinge = [&]() -> bool
		{
			if (ikBone.ikLinks.size() != 2) return false;

			const auto& link0 = ikBone.ikLinks[0];
			const auto& link1 = ikBone.ikLinks[1];

			auto isAxisXOnly = [&](const auto& link) -> bool
				{
					if (!link.hasLimit) return false;
					// YZがほぼゼロ（±0.1deg程度以内）ならXのみとみなす
					const float eps = 0.00174533f; // 0.1deg in rad
					const float ymin = std::fabs(MaybeDegreesToRadians(link.limitMin.y));
					const float ymax = std::fabs(MaybeDegreesToRadians(link.limitMax.y));
					const float zmin = std::fabs(MaybeDegreesToRadians(link.limitMin.z));
					const float zmax = std::fabs(MaybeDegreesToRadians(link.limitMax.z));
					return (ymin < eps && ymax < eps && zmin < eps && zmax < eps);
				};

			const bool axis0 = isAxisXOnly(link0);
			const bool axis1 = isAxisXOnly(link1);
			if (axis0 == axis1) return false; // 0個 or 2個

			const auto& hingeLink = axis0 ? link0 : link1; // 1軸制限（膝/肘）
			const auto& rootLink = axis0 ? link1 : link0;  // 付け根側（腿/上腕）

			const int midIdxI = hingeLink.boneIndex;
			const int rootIdxI = rootLink.boneIndex;
			if (midIdxI < 0 || rootIdxI < 0) return false;

			const size_t midIdx = (size_t)midIdxI;
			const size_t rootIdx = (size_t)rootIdxI;

			if (rootIdx >= m_bones.size() || midIdx >= m_bones.size() || targetIdx >= m_bones.size()) return false;

			// 2ボーン(root -> mid -> target) の形になっていること
			if (m_bones[midIdx].parentIndex != (int)rootIdx) return false;
			if (m_bones[targetIdx].parentIndex != (int)midIdx) return false;

			// 現在の位置
			XMMATRIX rootGlobal0 = XMLoadFloat4x4(&m_boneStates[rootIdx].globalMatrix);
			XMMATRIX midGlobal0 = XMLoadFloat4x4(&m_boneStates[midIdx].globalMatrix);
			XMMATRIX tgtGlobal0 = XMLoadFloat4x4(&m_boneStates[targetIdx].globalMatrix);

			XMVECTOR A = rootGlobal0.r[3];
			XMVECTOR B = midGlobal0.r[3];
			XMVECTOR C = tgtGlobal0.r[3];

			XMVECTOR destPos = XMLoadFloat4x4(&m_boneStates[boneIndex].globalMatrix).r[3];

			XMVECTOR AB = XMVectorSubtract(B, A);
			XMVECTOR BC = XMVectorSubtract(C, B);

			const float L1 = std::sqrt(std::max(0.0f, XMVectorGetX(XMVector3LengthSq(AB))));
			const float L2 = std::sqrt(std::max(0.0f, XMVectorGetX(XMVector3LengthSq(BC))));
			if (L1 < 1.0e-5f || L2 < 1.0e-5f) return false;

			// rootの親空間（rootローカル回転を適用する空間）
			XMMATRIX parentGlobal = XMMatrixIdentity();
			const int parentIdx = m_bones[rootIdx].parentIndex;
			if (parentIdx >= 0 && parentIdx < (int)m_bones.size())
			{
				parentGlobal = XMLoadFloat4x4(&m_boneStates[parentIdx].globalMatrix);
			}
			XMMATRIX parentInv = XMMatrixInverse(nullptr, parentGlobal);

			// ターゲット距離を、膝の制限範囲で達成可能な距離にクランプ（届かない場合は最も近い距離へ）
			XMVECTOR AD = XMVectorSubtract(destPos, A);
			float dDesired = std::sqrt(std::max(0.0f, XMVectorGetX(XMVector3LengthSq(AD))));
			if (dDesired < 1.0e-6f) return false;

			XMVECTOR dirAD = XMVector3Normalize(AD);

			float minReach = std::fabs(L1 - L2);
			float maxReach = L1 + L2;

			if (hingeLink.hasLimit)
			{
				float aMin = MaybeDegreesToRadians(hingeLink.limitMin.x);
				float aMax = MaybeDegreesToRadians(hingeLink.limitMax.x);
				if (aMin > aMax) std::swap(aMin, aMax);
				aMin = std::clamp(aMin, -XM_PI, XM_PI);
				aMax = std::clamp(aMax, -XM_PI, XM_PI);

				auto distFrom = [&](float ang) -> float
					{
						const float c = std::cos(ang);
						const float v = L1 * L1 + L2 * L2 + 2.0f * L1 * L2 * c;
						return std::sqrt(std::max(0.0f, v));
					};
				const float r0 = distFrom(aMin);
				const float r1 = distFrom(aMax);
				minReach = std::min(r0, r1);
				maxReach = std::max(r0, r1);
			}

			const float epsReach = 1.0e-4f;
			float d = std::clamp(dDesired, minReach + epsReach, maxReach - epsReach);
			XMVECTOR destClampedPos = XMVectorAdd(A, XMVectorScale(dirAD, d));


			XMVECTOR u = XMVector3Normalize(XMVectorSubtract(destClampedPos, A)); // root->target 方向
			if (XMVectorGetX(XMVector3LengthSq(u)) < 1.0e-12f) return false;

			XMVECTOR rootX = XMLoadFloat4x4(&m_boneStates[rootIdx].globalMatrix).r[0]; // World X axis of Root
			XMVECTOR rootY = XMLoadFloat4x4(&m_boneStates[rootIdx].globalMatrix).r[1]; // World Y axis of Root
			XMVECTOR rootZ = XMLoadFloat4x4(&m_boneStates[rootIdx].globalMatrix).r[2]; // World Z axis of Root

			XMVECTOR planeN = XMVectorSubtract(rootX, XMVectorScale(u, XMVectorGetX(XMVector3Dot(rootX, u))));

			if (XMVectorGetX(XMVector3LengthSq(planeN)) < 1.0e-5f)
			{
				// 万が一、足が真横(X軸方向)に伸びて軸と重なった場合のフォールバック
				// u に最も直交する軸を選んで安定した法線を作る
				const XMVECTOR axes[] =
				{
					rootY,
					rootZ,
					XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
					XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
				};

				float bestLenSq = 0.0f;
				XMVECTOR best = XMVectorZero();
				for (const XMVECTOR& axis : axes)
				{
					XMVECTOR c = XMVector3Cross(u, axis);
					float lenSq = XMVectorGetX(XMVector3LengthSq(c));
					if (lenSq > bestLenSq)
					{
						bestLenSq = lenSq;
						best = c;
					}
				}

				if (bestLenSq < 1.0e-6f) return false;
				planeN = best;
			}
			planeN = XMVector3Normalize(planeN);

			XMVECTOR poleW = rootZ;

			// 逆関節(鳥足)設定の場合は反転
			if (hingeLink.hasLimit)
			{
				float minRad = MaybeDegreesToRadians(hingeLink.limitMin.x);
				float maxRad = MaybeDegreesToRadians(hingeLink.limitMax.x);
				if ((minRad + maxRad) * 0.5f < 0.0f)
				{
					poleW = XMVectorNegate(poleW);
				}
			}

			// 平面上の直交ベクトル v (膝が出る方向)
			XMVECTOR v = XMVector3Cross(planeN, u);
			v = XMVector3Normalize(v);

			const float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
			float h2 = L1 * L1 - a * a;
			if (h2 < 0.0f) h2 = 0.0f;
			const float h = std::sqrt(h2);

			XMVECTOR base = XMVectorAdd(A, XMVectorScale(u, a));
			XMVECTOR B1 = XMVectorAdd(base, XMVectorScale(v, h));
			XMVECTOR B2 = XMVectorSubtract(base, XMVectorScale(v, h));

			// 2解のうち「膝が poleW 方向へ出る」方を選ぶ
			const float s1 = XMVectorGetX(XMVector3Dot(XMVectorSubtract(B1, base), poleW));
			const float s2 = XMVectorGetX(XMVector3Dot(XMVectorSubtract(B2, base), poleW));
			XMVECTOR Bdes = (s1 >= s2) ? B1 : B2;

			XMVECTOR upperCur = XMVector3Normalize(AB);
			XMVECTOR upperDes = XMVector3Normalize(XMVectorSubtract(Bdes, A));

			// 1) root swing: upperCur -> upperDes
			{
				XMVECTOR axisW = XMVector3Cross(upperCur, upperDes);
				const float axisLenSq = XMVectorGetX(XMVector3LengthSq(axisW));
				const float dot = std::clamp(XMVectorGetX(XMVector3Dot(upperCur, upperDes)), -1.0f, 1.0f);
				if (axisLenSq > 1.0e-10f)
				{
					const float axisLen = std::sqrt(std::max(axisLenSq, 0.0f));
					const float angle = std::atan2(axisLen, dot);
					if (angle > 1.0e-6f)
					{
						XMVECTOR axisLocal = XMVector3TransformNormal(axisW, parentInv);
						if (XMVectorGetX(XMVector3LengthSq(axisLocal)) > 1.0e-10f)
						{
							axisLocal = XMVector3Normalize(axisLocal);

							XMVECTOR deltaRot = XMQuaternionRotationAxis(axisLocal, angle);

							auto& rootState = m_boneStates[rootIdx];
							XMVECTOR q = XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&rootState.localRotation), deltaRot));

							if (rootLink.hasLimit) q = ClampIKRotationRobust(q, rootLink.limitMin, rootLink.limitMax);

							XMStoreFloat4(&rootState.localRotation, q);
							if (!UpdateToTargetFrom(rootIdx)) return false;
						}
					}
				}
			}

			// 【修正2】root twist: 無効化 (コメントアウト)
			// 足の内股ねじれの原因になるため削除
			/*
			{
				XMMATRIX rootGlobal = XMLoadFloat4x4(&m_boneStates[rootIdx].globalMatrix);
				XMVECTOR xAxisW = XMVector3Normalize(rootGlobal.r[0]);
				... (省略)
			}
			*/

			// 3) hinge(middle) angle: law of cos (解析)
			{
				const float cosInternal = std::clamp((L1 * L1 + L2 * L2 - d * d) / (2.0f * L1 * L2), -1.0f, 1.0f);
				const float internal = std::acos(cosInternal);
				float flex = XM_PI - internal; // 0: 伸び, +: 曲げ

				// 【修正1】符号決定ロジック
				// 位置関係から符号を推測すると、足を前に出した時などに反転してしまうため、
				// リンクの「可動域制限(Limit)」を見て、可動範囲が広い方向に曲げるように固定します。
				float sign = 1.0f;
				if (hingeLink.hasLimit)
				{
					float minRad = MaybeDegreesToRadians(hingeLink.limitMin.x);
					float maxRad = MaybeDegreesToRadians(hingeLink.limitMax.x);

					// 可動域の絶対値が大きい方を「曲がる方向」とみなす
					if (std::abs(minRad) > std::abs(maxRad))
					{
						sign = -1.0f;
					}
					else
					{
						sign = 1.0f;
					}
				}
				else
				{
					if (m_hasLastIkLimitedEuler[midIdx]) sign = (m_lastIkLimitedEuler[midIdx].x >= 0.0f) ? 1.0f : -1.0f;
				}

				float targetAngle = sign * flex;

				float minRad = -XM_PI, maxRad = XM_PI;
				if (hingeLink.hasLimit)
				{
					minRad = MaybeDegreesToRadians(hingeLink.limitMin.x);
					maxRad = MaybeDegreesToRadians(hingeLink.limitMax.x);
					if (minRad > maxRad) std::swap(minRad, maxRad);
				}

				// wrap / clamp / spike suppression
				const float center = 0.5f * (minRad + maxRad);
				float ref = center;
				if (m_hasLastIkLimitedEuler[midIdx]) ref = m_lastIkLimitedEuler[midIdx].x;
				targetAngle = WrapAngleNear(targetAngle, ref);
				targetAngle = std::clamp(targetAngle, minRad, maxRad);

				if (m_hasLastIkLimitedEuler[midIdx])
				{
					const float prev = m_lastIkLimitedEuler[midIdx].x;
					float ta = WrapAngleNear(targetAngle, prev);

					XMVECTOR tgtPosW = XMLoadFloat4x4(&m_boneStates[targetIdx].globalMatrix).r[3];
					const float err = std::sqrt(std::max(0.0f, XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(destPos, tgtPosW)))));

					float maxDelta = kMaxKneeDeltaPerFrameRad;
					if (err > 0.05f) maxDelta = DirectX::XM_PI;
					else if (err > 0.02f) maxDelta = 1.2f;
					else if (err > 0.01f) maxDelta = 0.9f;

					float dd = ta - prev;
					dd = std::clamp(dd, -maxDelta, +maxDelta);
					targetAngle = std::clamp(prev + dd, minRad, maxRad);
				}

				XMVECTOR kneeQ = XMQuaternionRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), targetAngle);
				auto& midState = m_boneStates[midIdx];
				XMStoreFloat4(&midState.localRotation, kneeQ);
				m_lastIkLimitedEuler[midIdx].x = targetAngle;
				m_hasLastIkLimitedEuler[midIdx] = 1;

				if (!UpdateToTargetFrom(midIdx)) return false;
			}

			return true;
		};

	if (TrySolveAnalyticTwoBoneHinge()) return;



	for (int loop = 0; loop < loopCount; ++loop)
	{
		for (size_t linkIdx = 0; linkIdx < ikBone.ikLinks.size(); ++linkIdx)
		{
			const auto& link = ikBone.ikLinks[linkIdx];
			if (link.boneIndex < 0) continue;

			const size_t currIdx = static_cast<size_t>(link.boneIndex);
			if (currIdx >= m_bones.size()) continue;

			// --- 現在の状態取得 ---
			XMMATRIX destGlobal = XMLoadFloat4x4(&m_boneStates[boneIndex].globalMatrix);
			XMVECTOR destPos = destGlobal.r[3];

			XMMATRIX targetGlobal = XMLoadFloat4x4(&m_boneStates[targetIdx].globalMatrix);
			XMVECTOR currPos = targetGlobal.r[3];

			XMMATRIX linkGlobal = XMLoadFloat4x4(&m_boneStates[currIdx].globalMatrix);
			XMVECTOR linkPos = linkGlobal.r[3];

			XMMATRIX parentGlobal = XMMatrixIdentity();
			int parentIndex = m_bones[currIdx].parentIndex;
			if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < m_bones.size())
			{
				parentGlobal = XMLoadFloat4x4(&m_boneStates[parentIndex].globalMatrix);
			}
			XMMATRIX parentInv = XMMatrixInverse(nullptr, parentGlobal);

			// --- 1軸制限（膝など）の判定 ---
			bool hasLimit = link.hasLimit;
			XMFLOAT3 limMin = link.limitMin;
			XMFLOAT3 limMax = link.limitMax;
			const float eps = 1.0e-3f;

			// X軸のみに制限がある関節か判定
			bool isAxisXOnly = hasLimit &&
				(std::abs(limMin.y) < eps && std::abs(limMax.y) < eps) &&
				(std::abs(limMin.z) < eps && std::abs(limMax.z) < eps);

			if (isAxisXOnly)
			{
				XMVECTOR toDest = XMVectorSubtract(destPos, linkPos);
				XMVECTOR toCurr = XMVectorSubtract(currPos, linkPos);

				// 親空間（ボーンローカル空間）へ変換
				XMVECTOR localDirDest = XMVector3TransformNormal(toDest, parentInv);
				XMVECTOR localDirCurr = XMVector3TransformNormal(toCurr, parentInv);

				// X成分を無視してYZ平面に投影し、正規化
				localDirDest = XMVectorSetX(localDirDest, 0.0f);
				localDirCurr = XMVectorSetX(localDirCurr, 0.0f);

				{
					// 伸び切り付近では投影ベクトルの長さが小さくなりやすい。
					// 正規化するとノイズが増幅して角度が跳ぶので、正規化せずに atan2 で角度を得る。
					const float lenDestSq = XMVectorGetX(XMVector3LengthSq(localDirDest));
					const float lenCurrSq = XMVectorGetX(XMVector3LengthSq(localDirCurr));
					if (lenDestSq <= 1.0e-12f || lenCurrSq <= 1.0e-12f)
					{
						// ここで全方向IKに落とすと不要な回転が混入してスパイクになりやすいので、リンク回転をスキップする。
						continue;
					}

					// Cross積(X成分) = Y*Z' - Z*Y'
					float crossX = XMVectorGetX(XMVector3Cross(localDirCurr, localDirDest));
					float dot = XMVectorGetX(XMVector3Dot(localDirCurr, localDirDest));
					float deltaAngle = std::atan2(crossX, dot);
					deltaAngle = std::clamp(deltaAngle, -limitAngle, limitAngle);

					// 2. 現在の角度(X軸)を取得 (スカラー)
					auto& linkState = m_boneStates[currIdx];
					XMVECTOR currentQ = XMLoadFloat4(&linkState.localRotation);
					float currentAngle = ExtractTwistAngleX(currentQ);

					// 3. 角度のUnwrapping 
					float minRad = MaybeDegreesToRadians(limMin.x);
					float maxRad = MaybeDegreesToRadians(limMax.x);
					if (minRad > maxRad) std::swap(minRad, maxRad);

					float center = (minRad + maxRad) * 0.5f;
					float ref = center;
					if (m_hasLastIkLimitedEuler[currIdx]) ref = m_lastIkLimitedEuler[currIdx].x;
					currentAngle = WrapAngleNear(currentAngle, ref);

					// 4. 足し算して次の角度を求める
					float targetAngle = currentAngle + deltaAngle;

					// 5. 制限範囲でクランプ
					targetAngle = std::clamp(targetAngle, minRad, maxRad);
					// フレーム間のスパイク抑制
					if (m_hasLastIkLimitedEuler[currIdx])
					{
						const float prev = m_lastIkLimitedEuler[currIdx].x;
						float ta = WrapAngleNear(targetAngle, prev);
						const float err = std::sqrt(std::max(0.0f, XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(destPos, currPos)))));
						float maxDelta = kMaxKneeDeltaPerFrameRad;
						if (err > 0.05f) maxDelta = DirectX::XM_PI;
						else if (err > 0.02f) maxDelta = 1.2f;
						else if (err > 0.01f) maxDelta = 0.9f;
						float d = ta - prev;
						d = std::clamp(d, -maxDelta, +maxDelta);
						targetAngle = std::clamp(prev + d, minRad, maxRad);
					}

					// 6. 新しい角度からクォータニオンを作成して適用
					XMVECTOR newRot = XMQuaternionRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), targetAngle);
					XMStoreFloat4(&linkState.localRotation, newRot);
					m_lastIkLimitedEuler[currIdx].x = targetAngle;
					m_hasLastIkLimitedEuler[currIdx] = 1;

					if (!UpdateToTargetFrom(currIdx)) continue;

					// 収束判定
					targetGlobal = XMLoadFloat4x4(&m_boneStates[targetIdx].globalMatrix);
					currPos = targetGlobal.r[3];
					if (XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(destPos, currPos))) < 1.0e-6f)
					{
						loop = loopCount;
						break;
					}

					continue;
				}

			}
			// --- 通常の全方向IK (標準ロジック) ---
			XMVECTOR toDest = XMVectorSubtract(destPos, linkPos);
			XMVECTOR toCurr = XMVectorSubtract(currPos, linkPos);

			float distDest = XMVectorGetX(XMVector3Length(toDest));
			float distCurr = XMVectorGetX(XMVector3Length(toCurr));

			if (distDest < 1e-4f || distCurr < 1e-4f) continue;

			toDest = XMVectorScale(toDest, 1.0f / distDest);
			toCurr = XMVectorScale(toCurr, 1.0f / distCurr);

			float dot = XMVectorGetX(XMVector3Dot(toDest, toCurr));
			dot = std::clamp(dot, -1.0f, 1.0f);

			XMVECTOR axis = XMVector3Cross(toCurr, toDest);
			const float axisLenSq = XMVectorGetX(XMVector3LengthSq(axis));
			const float axisLen = std::sqrt(std::max(axisLenSq, 0.0f));
			float angle = std::atan2(axisLen, dot);

			if (angle < 1e-4f) continue;
			if (angle > limitAngle) angle = limitAngle;

			if (axisLenSq < 1.0e-10f)
			{
				if (dot > 0.0f) continue; // ほぼ同方向

				XMVECTOR base = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
				if (std::fabs(XMVectorGetX(XMVector3Dot(toCurr, base))) > 0.99f)
				{
					base = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
				}

				axis = XMVector3Cross(toCurr, base);
				if (XMVectorGetX(XMVector3LengthSq(axis)) < 1.0e-12f) continue;
				axis = XMVector3Normalize(axis);
			}
			else
			{
				axis = XMVector3Normalize(axis);
			}
			XMVECTOR localAxis = XMVector3TransformNormal(axis, parentInv);
			if (XMVectorGetX(XMVector3LengthSq(localAxis)) < 1e-8f) continue;
			localAxis = XMVector3Normalize(localAxis);

			XMVECTOR deltaRot = XMQuaternionRotationAxis(localAxis, angle);

			// --- 回転の適用と制限 ---
			auto& linkState = m_boneStates[currIdx];
			XMVECTOR currentRot = XMLoadFloat4(&linkState.localRotation);

			currentRot = XMQuaternionMultiply(currentRot, deltaRot);
			currentRot = XMQuaternionNormalize(currentRot);

			if (hasLimit)
			{
				currentRot = ClampIKRotationRobust(currentRot, link.limitMin, link.limitMax);
			}

			XMStoreFloat4(&linkState.localRotation, currentRot);

			if (!UpdateToTargetFrom(currIdx)) continue;

			// 収束判定
			targetGlobal = XMLoadFloat4x4(&m_boneStates[targetIdx].globalMatrix);
			currPos = targetGlobal.r[3];
			if (XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(destPos, currPos))) < 1.0e-6f)
			{
				loop = loopCount;
				break;
			}
		}
	}
}

void BoneSolver::UpdateMatrices()
{
	UpdateMatrices(true);
}

void BoneSolver::UpdateMatrices(bool solveIK)
{
	UpdateBoneTransforms(UpdatePhase::All);

	if (solveIK)
	{
		SolveIKPhase(UpdatePhase::All);

		UpdateBoneTransforms(UpdatePhase::All);
	}

	UpdateSkinningMatrices();
}

void BoneSolver::UpdateMatricesBeforePhysics()
{
	UpdateBoneTransforms(UpdatePhase::All);
	SolveIKPhase(UpdatePhase::BeforePhysics);
	UpdateBoneTransforms(UpdatePhase::All);
	UpdateSkinningMatrices();
}

void BoneSolver::UpdateMatricesAfterPhysics()
{
	UpdateBoneTransforms(UpdatePhase::All);
	SolveIKPhase(UpdatePhase::AfterPhysics);
	UpdateBoneTransforms(UpdatePhase::All);
	UpdateSkinningMatrices();
}

void BoneSolver::UpdateSkinningMatrices()
{
	const int n = static_cast<int>(m_bones.size());
#pragma omp parallel for schedule(static) if(n >= 256)
	for (int i = 0; i < n; ++i) CalculateSkinningMatrix(i);
	RecomputeBounds();
}

void BoneSolver::UpdateMatricesNoIK()
{
	UpdateBoneTransforms(UpdatePhase::All);
	UpdateSkinningMatrices();
}

const DirectX::XMFLOAT4X4& BoneSolver::GetBoneGlobalMatrix(size_t boneIndex) const
{
	if (boneIndex >= m_boneStates.size())
	{
		throw std::out_of_range("Bone index out of range");
	}
	return m_boneStates[boneIndex].globalMatrix;
}

const DirectX::XMFLOAT4X4& BoneSolver::GetBoneLocalMatrix(size_t boneIndex) const
{
	if (boneIndex >= m_boneStates.size())
	{
		throw std::out_of_range("BoneSolver::GetBoneLocalMatrix: boneIndex out of range");
	}
	return m_boneStates[boneIndex].localMatrix;
}

int BoneSolver::FindBoneIndex(const std::wstring& boneName) const
{
	const auto it = m_boneNameToIndex.find(boneName);
	if (it == m_boneNameToIndex.end())
	{
		return -1;
	}
	return static_cast<int>(it->second);
}

void BoneSolver::SetBoneLocalPose(size_t boneIndex,
								  const DirectX::XMFLOAT3& translation,
								  const DirectX::XMFLOAT4& rotation)
{
	if (boneIndex >= m_boneStates.size())
	{
		throw std::out_of_range("Bone index out of range");
	}

	auto& s = m_boneStates[boneIndex];
	s.localTranslation = translation;
	s.localRotation = rotation;
	m_boundsValid = false;
}

void BoneSolver::ClearExternalParentTransforms()
{
	m_externalParentTransforms.clear();
	m_boundsValid = false;
}

void BoneSolver::SetExternalParentTransform(std::int32_t key, const ExternalParentTransform& transform)
{
	m_externalParentTransforms.insert_or_assign(key, transform);
	m_boundsValid = false;
}

void BoneSolver::GetBoneBounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const
{
	if (m_boundsValid)
	{
		outMin = m_cachedBoundsMin;
		outMax = m_cachedBoundsMax;
		return;
	}

	float minx = std::numeric_limits<float>::max();
	float miny = std::numeric_limits<float>::max();
	float minz = std::numeric_limits<float>::max();
	float maxx = std::numeric_limits<float>::lowest();
	float maxy = std::numeric_limits<float>::lowest();
	float maxz = std::numeric_limits<float>::lowest();

	bool hasBone = false;
	for (const auto& state : m_boneStates)
	{
		float x = state.globalMatrix._41;
		float y = state.globalMatrix._42;
		float z = state.globalMatrix._43;

		if (x < minx) minx = x;
		if (x > maxx) maxx = x;
		if (y < miny) miny = y;
		if (y > maxy) maxy = y;
		if (z < minz) minz = z;
		if (z > maxz) maxz = z;
		hasBone = true;
	}

	if (!hasBone)
	{
		outMin = { 0, 0, 0 };
		outMax = { 0, 0, 0 };
	}
	else
	{
		outMin = { minx, miny, minz };
		outMax = { maxx, maxy, maxz };
	}
}

void BoneSolver::RecomputeBounds()
{
	float minx = std::numeric_limits<float>::max();
	float miny = std::numeric_limits<float>::max();
	float minz = std::numeric_limits<float>::max();
	float maxx = std::numeric_limits<float>::lowest();
	float maxy = std::numeric_limits<float>::lowest();
	float maxz = std::numeric_limits<float>::lowest();

	bool hasBone = false;
	for (const auto& state : m_boneStates)
	{
		const float x = state.globalMatrix._41;
		const float y = state.globalMatrix._42;
		const float z = state.globalMatrix._43;

		minx = std::min(minx, x);
		miny = std::min(miny, y);
		minz = std::min(minz, z);
		maxx = std::max(maxx, x);
		maxy = std::max(maxy, y);
		maxz = std::max(maxz, z);
		hasBone = true;
	}

	if (!hasBone)
	{
		m_cachedBoundsMin = { 0.0f, 0.0f, 0.0f };
		m_cachedBoundsMax = { 0.0f, 0.0f, 0.0f };
	}
	else
	{
		m_cachedBoundsMin = { minx, miny, minz };
		m_cachedBoundsMax = { maxx, maxy, maxz };
	}

	m_boundsValid = true;
}
