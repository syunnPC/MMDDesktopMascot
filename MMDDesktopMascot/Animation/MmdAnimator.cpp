#include "MmdAnimator.hpp"
#include "BoneSolver.hpp"
#include "MmdPhysicsWorld.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <DirectXMath.h>

namespace
{
	constexpr double kMaxUpdateGapSeconds = 0.25;
	constexpr int kMaxAnimatorCatchUpStepsMin = 4;
	constexpr int kMaxAnimatorCatchUpStepsMax = 24;

	bool HasPhysicsBodies(const PmxModel* model)
	{
		return model &&
			(!model->RigidBodies().empty() || !model->SoftBodies().empty());
	}

	float NormalizeFrame(float frame, float maxFrame)
	{
		if (maxFrame <= 0.0f) return frame;
		const float cycle = maxFrame;
		frame = std::fmod(frame, cycle);
		if (frame < 0.0f) frame += cycle;
		return frame;
	}

	float EvaluateBezier(float t, float x1, float y1, float x2, float y2)
	{
		if (t <= 0.0f) return 0.0f;
		if (t >= 1.0f) return 1.0f;

		auto cubic = [](float p0, float p1, float p2, float p3, float s) {
			float inv = 1.0f - s;
			return inv * inv * inv * p0 + 3.0f * inv * inv * s * p1 + 3.0f * inv * s * s * p2 + s * s * s * p3;
			};

		float low = 0.0f, high = 1.0f, s = t;
		for (int i = 0; i < 15; ++i)
		{
			s = 0.5f * (low + high);
			float x = cubic(0.0f, x1, x2, 1.0f, s);
			if (x < t)
				low = s;
			else
				high = s;
		}

		return cubic(0.0f, y1, y2, 1.0f, s);
	}

	float EvaluateChannelT(const std::uint8_t* interp, float t)
	{
		float x1 = interp[0] / 127.0f;
		float y1 = interp[4] / 127.0f;
		float x2 = interp[8] / 127.0f;
		float y2 = interp[12] / 127.0f;
		return EvaluateBezier(t, x1, y1, x2, y2);
	}

	float NextBlinkIntervalSeconds()
	{
		thread_local std::mt19937 generator(std::random_device{}());
		thread_local std::uniform_real_distribution<float> distribution(2.0f, 6.0f);
		return distribution(generator);
	}

	DirectX::XMVECTOR QuaternionPow(DirectX::FXMVECTOR quaternionIn, float exponent)
	{
		using namespace DirectX;

		XMVECTOR quaternion = XMQuaternionNormalize(quaternionIn);
		if (XMVectorGetW(quaternion) < 0.0f)
		{
			quaternion = XMVectorNegate(quaternion);
		}

		float w = std::clamp(XMVectorGetW(quaternion), -1.0f, 1.0f);
		const float halfAngle = std::acos(w);
		const float sinHalfAngle = std::sin(halfAngle);
		if (std::abs(sinHalfAngle) < 1.0e-8f)
		{
			return XMQuaternionIdentity();
		}

		const XMVECTOR axis = XMVectorScale(
			XMVectorSet(
				XMVectorGetX(quaternion),
				XMVectorGetY(quaternion),
				XMVectorGetZ(quaternion),
				0.0f),
			1.0f / sinHalfAngle);

		const float scaledHalfAngle = halfAngle * exponent;
		const float scaledSin = std::sin(scaledHalfAngle);
		const float scaledCos = std::cos(scaledHalfAngle);
		return XMQuaternionNormalize(XMVectorSet(
			XMVectorGetX(axis) * scaledSin,
			XMVectorGetY(axis) * scaledSin,
			XMVectorGetZ(axis) * scaledSin,
			scaledCos));
	}
}

MmdAnimator::MmdAnimator()
{
	m_lastUpdate = std::chrono::steady_clock::now();
	DirectX::XMStoreFloat4x4(&m_motionTransform, DirectX::XMMatrixIdentity());
	m_boneSolver = std::make_unique<BoneSolver>();
	m_physicsWorld = std::make_unique<MmdPhysicsWorld>();

	m_nextBlinkInterval = NextBlinkIntervalSeconds();
}

MmdAnimator::~MmdAnimator() = default;

void MmdAnimator::BeginPoseTransitionFromLastPose()
{
	if (!m_hasLastPose)
	{
		return;
	}

	m_transitionPose = m_lastPose;
	m_transitionElapsed = 0.0;
	m_transitionActive = true;
	m_hasTransitionPose = true;
	m_transitionNeedsInit = true;
}

bool MmdAnimator::LoadModel(const std::filesystem::path& pmx)
{
	auto model = std::make_unique<PmxModel>();
	if (model->Load(pmx))
	{
		SetModel(std::move(model));
		return true;
	}
	return false;
}

bool MmdAnimator::LoadMotion(const std::filesystem::path& vmd)
{
	auto motion = std::make_unique<VmdMotion>();
	if (motion->Load(vmd))
	{
		BeginPoseTransitionFromLastPose();
		m_motion = std::move(motion);
		m_time = 0.0;
		m_pose = {};
		m_cachedMotionPtr = nullptr;
		m_poseTranslationTrackSlots.clear();
		m_poseRotationTrackSlots.clear();
		m_poseMorphTrackSlots.clear();
		m_paused = false;
		DirectX::XMStoreFloat4x4(&m_motionTransform, DirectX::XMMatrixIdentity());
		m_prevFrameForPhysicsValid = false;
		m_suspendPhysicsForLoopTransition = false;
		m_resetPhysicsOnNextTick = false;
		m_physicsWorld->Reset();
		ResetUpdateClock();
		return true;
	}
	return false;
}

void MmdAnimator::ClearMotion()
{
	BeginPoseTransitionFromLastPose();
	m_motion.reset();
	m_time = 0.0;
	m_pose = {};
	m_cachedMotionPtr = nullptr;
	m_poseTranslationTrackSlots.clear();
	m_poseRotationTrackSlots.clear();
	m_poseMorphTrackSlots.clear();
	m_paused = false;
	m_hasSkinnedPose = false;
	m_prevFrameForPhysicsValid = false;
	m_suspendPhysicsForLoopTransition = false;
	m_resetPhysicsOnNextTick = false;
	DirectX::XMStoreFloat4x4(&m_motionTransform, DirectX::XMMatrixIdentity());
	m_physicsWorld->Reset();
	ResetUpdateClock();
}

void MmdAnimator::StopMotion()
{
	ClearMotion();
}

void MmdAnimator::Update()
{
	auto now = std::chrono::steady_clock::now();

	if (m_firstUpdate)
	{
		m_lastUpdate = now;
		m_firstUpdate = false;
		return;
	}

	double dt = std::chrono::duration<double>(now - m_lastUpdate).count();
	m_lastUpdate = now;
	if (dt <= 0.0)
	{
		if (!m_hasSkinnedPose && m_model)
		{
			Tick(0.0);
		}
		return;
	}

	if (dt > kMaxUpdateGapSeconds)
	{
		// Ignore large wall-clock gaps from loading, breakpoints, or window drags.
		// Replaying them as a single animation/physics catch-up step destabilizes MMD rigs.
		m_updateAccumulator = 0.0;
		if (!m_hasSkinnedPose && m_model)
		{
			Tick(0.0);
		}
		return;
	}

	const double fixedStep = ComputeFixedUpdateStep();
	if (fixedStep <= 0.0)
	{
		Tick(dt);
		return;
	}

	const int maxCatchUpSteps = std::clamp(
		std::max(kMaxAnimatorCatchUpStepsMin, m_physicsWorld->GetSettings().maxSubSteps * 2),
		kMaxAnimatorCatchUpStepsMin,
		kMaxAnimatorCatchUpStepsMax);
	m_updateAccumulator = std::min(
		m_updateAccumulator + dt,
		fixedStep * static_cast<double>(maxCatchUpSteps));

	int executedSteps = 0;
	while (m_updateAccumulator + 1.0e-9 >= fixedStep &&
		   executedSteps < maxCatchUpSteps)
	{
		Tick(fixedStep);
		m_updateAccumulator -= fixedStep;
		++executedSteps;
	}

	if (executedSteps == 0 && !m_hasSkinnedPose && m_model)
	{
		Tick(0.0);
	}
}

void MmdAnimator::UpdateMotionCache(const VmdMotion* motion)
{
	// キャッシュが有効なら何もしない
	if (motion && m_cachedMotionPtr == motion && m_model &&
		m_boneTrackToBoneIndex.size() == motion->BoneTracks().size() &&
		m_morphKeyCursors.size() == motion->MorphTracks().size() &&
		m_poseTranslationTrackSlots.size() == motion->BoneTracks().size() &&
		m_poseRotationTrackSlots.size() == motion->BoneTracks().size() &&
		m_poseMorphTrackSlots.size() == motion->MorphTracks().size())
	{
		return;
	}

	m_cachedMotionPtr = motion;
	m_boneTrackToBoneIndex.clear();
	m_boneKeyCursors.clear();
	m_morphKeyCursors.clear();
	m_poseTranslationTrackSlots.clear();
	m_poseRotationTrackSlots.clear();
	m_poseMorphTrackSlots.clear();

	if (!motion || !m_model) return;

	// --- ボーンのマッピング ---
	const auto& boneTracks = motion->BoneTracks();
	const auto& bones = m_model->Bones();
	const auto& morphTracks = motion->MorphTracks();

	m_boneTrackToBoneIndex.resize(boneTracks.size(), -1);
	m_boneKeyCursors.resize(boneTracks.size(), 0);
	m_poseTranslationTrackSlots.resize(boneTracks.size(), nullptr);
	m_poseRotationTrackSlots.resize(boneTracks.size(), nullptr);
	m_morphKeyCursors.resize(morphTracks.size(), 0);
	m_poseMorphTrackSlots.resize(morphTracks.size(), nullptr);

	const size_t bonePoseReserveTarget = std::max(
		bones.size(),
		boneTracks.size());
	const size_t morphPoseReserveTarget = std::max(
		m_model->Morphs().size(),
		morphTracks.size());
	m_pose.boneTranslations.reserve(std::max(m_pose.boneTranslations.size(), bonePoseReserveTarget) + 8);
	m_pose.boneRotations.reserve(std::max(m_pose.boneRotations.size(), bonePoseReserveTarget) + 8);
	m_pose.morphWeights.reserve(std::max(m_pose.morphWeights.size(), morphPoseReserveTarget) + 8);

	// 名前検索用マップを作成 (O(N) + O(M))
	std::unordered_map<std::wstring, int> boneMap;
	boneMap.reserve(bones.size());
	for (int i = 0; i < (int)bones.size(); ++i)
	{
		boneMap[bones[i].name] = i;
	}

	for (size_t i = 0; i < boneTracks.size(); ++i)
	{
		auto it = boneMap.find(boneTracks[i].name);
		if (it != boneMap.end())
		{
			m_boneTrackToBoneIndex[i] = it->second;
			auto [translationIt, _] = m_pose.boneTranslations.try_emplace(
				boneTracks[i].name,
				DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f });
			auto [rotationIt, __] = m_pose.boneRotations.try_emplace(
				boneTracks[i].name,
				DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f });
			m_poseTranslationTrackSlots[i] = &translationIt->second;
			m_poseRotationTrackSlots[i] = &rotationIt->second;
		}
	}

	for (size_t i = 0; i < morphTracks.size(); ++i)
	{
		auto [morphIt, _] = m_pose.morphWeights.try_emplace(morphTracks[i].name, 0.0f);
		m_poseMorphTrackSlots[i] = &morphIt->second;
	}
}

void MmdAnimator::CacheMorphIndices()
{
	m_morphNameToIndex.clear();
	m_resolvedMorphWeights.clear();
	m_morphVisitState.clear();

	if (!m_model)
	{
		return;
	}

	const auto& morphs = m_model->Morphs();
	m_morphNameToIndex.reserve(morphs.size());
	m_resolvedMorphWeights.assign(morphs.size(), 0.0f);
	m_morphVisitState.assign(morphs.size(), std::uint8_t{ 0 });
	for (int index = 0; index < static_cast<int>(morphs.size()); ++index)
	{
		m_morphNameToIndex.emplace(morphs[static_cast<size_t>(index)].name, index);
	}
}

void MmdAnimator::AccumulateMorphWeight(int morphIndex, float weight, std::vector<uint8_t>& visitState)
{
	if (!m_model || std::abs(weight) <= 1.0e-5f)
	{
		return;
	}

	const auto& morphs = m_model->Morphs();
	if (morphIndex < 0 || morphIndex >= static_cast<int>(morphs.size()))
	{
		return;
	}

	const size_t index = static_cast<size_t>(morphIndex);
	if (visitState[index] != 0)
	{
		return;
	}

	visitState[index] = 1;
	const auto& morph = morphs[index];
	if (morph.type == PmxModel::Morph::Type::Group)
	{
		for (const auto& offset : morph.groupOffsets)
		{
			AccumulateMorphWeight(offset.morphIndex, weight * offset.weight, visitState);
		}
	}
	else if (morph.type == PmxModel::Morph::Type::Flip)
	{
		for (const auto& offset : morph.flipOffsets)
		{
			AccumulateMorphWeight(offset.morphIndex, weight * offset.weight, visitState);
		}
	}
	else
	{
		m_resolvedMorphWeights[index] += weight;
	}
	visitState[index] = 0;
}

void MmdAnimator::ApplyBoneMorph(const PmxModel::Morph& morph, float weight)
{
	using namespace DirectX;

	if (!m_model || std::abs(weight) <= 1.0e-5f)
	{
		return;
	}

	const auto& bones = m_model->Bones();
	for (const auto& offset : morph.boneOffsets)
	{
		if (offset.boneIndex < 0 || offset.boneIndex >= static_cast<int32_t>(bones.size()))
		{
			continue;
		}

		const auto& boneName = bones[static_cast<size_t>(offset.boneIndex)].name;
		auto& translation = m_pose.boneTranslations[boneName];
		translation.x += offset.translation.x * weight;
		translation.y += offset.translation.y * weight;
		translation.z += offset.translation.z * weight;

		XMVECTOR currentRotation = XMQuaternionIdentity();
		if (const auto rotationIt = m_pose.boneRotations.find(boneName);
			rotationIt != m_pose.boneRotations.end())
		{
			currentRotation = XMLoadFloat4(&rotationIt->second);
		}

		XMVECTOR morphRotation = XMQuaternionNormalize(XMLoadFloat4(&offset.rotation));
		XMVECTOR nextRotation = XMQuaternionMultiply(currentRotation, QuaternionPow(morphRotation, weight));
		XMStoreFloat4(&m_pose.boneRotations[boneName], XMQuaternionNormalize(nextRotation));
	}
}

void MmdAnimator::ResolveMorphsAndApplyBoneMorphs()
{
	if (!m_model)
	{
		m_resolvedMorphWeights.clear();
		return;
	}

	const auto& morphs = m_model->Morphs();
	if (m_resolvedMorphWeights.size() != morphs.size())
	{
		m_resolvedMorphWeights.assign(morphs.size(), 0.0f);
	}
	else
	{
		std::fill(m_resolvedMorphWeights.begin(), m_resolvedMorphWeights.end(), 0.0f);
	}

	if (m_morphVisitState.size() != morphs.size())
	{
		m_morphVisitState.assign(morphs.size(), std::uint8_t{ 0 });
	}
	for (const auto& [name, weight] : m_pose.morphWeights)
	{
		const auto it = m_morphNameToIndex.find(name);
		if (it == m_morphNameToIndex.end())
		{
			continue;
		}

		AccumulateMorphWeight(it->second, weight, m_morphVisitState);
	}

	for (size_t morphIndex = 0; morphIndex < morphs.size(); ++morphIndex)
	{
		const float weight = m_resolvedMorphWeights[morphIndex];
		if (std::abs(weight) <= 1.0e-5f)
		{
			continue;
		}

		if (morphs[morphIndex].type == PmxModel::Morph::Type::Bone)
		{
			ApplyBoneMorph(morphs[morphIndex], weight);
		}
	}
}

void MmdAnimator::CacheLookAtBones()
{
	m_boneIdxHead = -1;
	m_boneIdxNeck = -1;
	m_boneIdxEyeL = -1;
	m_boneIdxEyeR = -1;

	if (!m_model) return;

	const auto& bones = m_model->Bones();
	for (int i = 0; i < (int)bones.size(); ++i)
	{
		const auto& name = bones[i].name;
		if (name == L"頭") m_boneIdxHead = i;
		else if (name == L"首") m_boneIdxNeck = i;
		else if (name == L"左目") m_boneIdxEyeL = i;
		else if (name == L"右目") m_boneIdxEyeR = i;
	}
}

void MmdAnimator::SetPhysicsSettings(const PhysicsSettings& settings)
{
	if (m_physicsWorld->GetSettings() == settings)
	{
		return;
	}

	m_physicsWorld->GetSettings() = settings;
	m_physicsWorld->Reset();
	m_prevFrameForPhysicsValid = false;
	m_suspendPhysicsForLoopTransition = false;
	m_resetPhysicsOnNextTick = false;
	ResetUpdateClock();
}

const PhysicsSettings& MmdAnimator::GetPhysicsSettings() const
{
	return m_physicsWorld->GetSettings();
}

float MmdAnimator::ComputeCurrentFrame(const VmdMotion* motion) const
{
	if (!motion) return 0.0f;

	const float currentFrameRaw = static_cast<float>(m_time * m_fps);
	const float maxFrame = static_cast<float>(motion->MaxFrame() + 1);
	return NormalizeFrame(currentFrameRaw, maxFrame);
}

void MmdAnimator::HandlePhysicsResetForFrame(const VmdMotion* motion, float currentFrame)
{
	bool needsPhysicsReset = false;
	bool loopedMotion = false;
	const bool hasPhysicsBodies = HasPhysicsBodies(m_model.get());

	if (motion && m_prevFrameForPhysicsValid)
	{
		if (currentFrame + 0.5f < m_prevFrameForPhysics)
		{
			loopedMotion = true;
			needsPhysicsReset = true;
		}
		else if (std::abs(currentFrame - m_prevFrameForPhysics) > 10.0f)
		{
			needsPhysicsReset = true;
		}
	}

	if (loopedMotion)
	{
		BeginPoseTransitionFromLastPose();
		if (hasPhysicsBodies && m_physicsEnabled)
		{
			needsPhysicsReset = false;
			m_suspendPhysicsForLoopTransition = true;
			m_resetPhysicsOnNextTick = false;
		}
		else
		{
			m_suspendPhysicsForLoopTransition = false;
		}
	}
	if (needsPhysicsReset)
	{
		m_suspendPhysicsForLoopTransition = false;
		m_resetPhysicsOnNextTick = false;
		m_physicsWorld->Reset();
	}
}

void MmdAnimator::ResetPoseForFrame(float currentFrame)
{
	for (auto& [_, translation] : m_pose.boneTranslations)
	{
		translation = { 0.0f, 0.0f, 0.0f };
	}
	for (auto& [_, rotation] : m_pose.boneRotations)
	{
		rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
	}
	for (auto& [_, weight] : m_pose.morphWeights)
	{
		weight = 0.0f;
	}
	m_pose.frame = currentFrame;
}

void MmdAnimator::ApplyMotionTracksToPose(const VmdMotion& motion, float currentFrame)
{
	// --- ボーンアニメーション適用 ---
	const auto& boneTracks = motion.BoneTracks();
	const size_t numBoneTracks = boneTracks.size();
	auto& poseTranslations = m_pose.boneTranslations;
	auto& poseRotations = m_pose.boneRotations;

	for (size_t i = 0; i < numBoneTracks; ++i)
	{
		if (m_boneTrackToBoneIndex[i] == -1) continue;

		const auto& track = boneTracks[i];
		const auto& keys = track.keys;
		if (keys.empty()) continue;

		size_t kIdx = m_boneKeyCursors[i];
		if (kIdx >= keys.size() - 1) kIdx = 0;
		if (keys[kIdx].frame > currentFrame) kIdx = 0;

		while (kIdx + 1 < keys.size() && keys[kIdx + 1].frame <= currentFrame)
		{
			kIdx++;
		}
		m_boneKeyCursors[i] = kIdx;

		const auto& k0 = keys[kIdx];
		const auto* k1 = (kIdx + 1 < keys.size()) ? &keys[kIdx + 1] : &k0;

		float t = 0.0f;
		if (k1->frame != k0.frame)
		{
			t = (currentFrame - static_cast<float>(k0.frame)) /
				static_cast<float>(k1->frame - k0.frame);
			t = std::clamp(t, 0.0f, 1.0f);
		}

		const std::uint8_t* base = k0.interp;
		float txT = EvaluateChannelT(base + 0, t);
		float tyT = EvaluateChannelT(base + 16, t);
		float tzT = EvaluateChannelT(base + 32, t);
		float rotT = EvaluateChannelT(base + 48, t);

		auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };

		DirectX::XMFLOAT3 trans{
			lerp(k0.tx, k1->tx, txT),
			lerp(k0.ty, k1->ty, tyT),
			lerp(k0.tz, k1->tz, tzT)
		};

		using namespace DirectX;
		XMVECTOR q0 = XMQuaternionNormalize(XMVectorSet(k0.qx, k0.qy, k0.qz, k0.qw));
		XMVECTOR q1 = XMQuaternionNormalize(XMVectorSet(k1->qx, k1->qy, k1->qz, k1->qw));
		XMVECTOR q = XMQuaternionSlerp(q0, q1, rotT);
		XMFLOAT4 rot;
		XMStoreFloat4(&rot, q);

		if (track.name == L"全ての親")
		{
			trans = { 0.0f, 0.0f, 0.0f };
		}
		else if (track.name == L"センター" || track.name == L"グルーブ")
		{
			trans.x = 0.0f;
			trans.z = 0.0f;
		}

		if (i < m_poseTranslationTrackSlots.size() && m_poseTranslationTrackSlots[i] &&
			i < m_poseRotationTrackSlots.size() && m_poseRotationTrackSlots[i])
		{
			*m_poseTranslationTrackSlots[i] = trans;
			*m_poseRotationTrackSlots[i] = rot;
		}
		else
		{
			poseTranslations.insert_or_assign(track.name, trans);
			poseRotations.insert_or_assign(track.name, rot);
		}
	}

	// --- モーフアニメーション適用 ---
	const auto& morphTracks = motion.MorphTracks();
	const size_t numMorphTracks = morphTracks.size();
	auto& poseMorphs = m_pose.morphWeights;

	for (size_t i = 0; i < numMorphTracks; ++i)
	{
		const auto& track = morphTracks[i];
		const auto& keys = track.keys;
		if (keys.empty()) continue;

		size_t kIdx = m_morphKeyCursors[i];
		if (kIdx >= keys.size() - 1) kIdx = 0;
		if (keys[kIdx].frame > currentFrame) kIdx = 0;

		while (kIdx + 1 < keys.size() && keys[kIdx + 1].frame <= currentFrame)
		{
			kIdx++;
		}
		m_morphKeyCursors[i] = kIdx;

		const auto& k0 = keys[kIdx];
		const auto* k1 = (kIdx + 1 < keys.size()) ? &keys[kIdx + 1] : &k0;

		float t = 0.0f;
		if (k1->frame != k0.frame)
		{
			t = (currentFrame - static_cast<float>(k0.frame)) /
				static_cast<float>(k1->frame - k0.frame);
			t = std::clamp(t, 0.0f, 1.0f);
		}

		float w = k0.weight + (k1->weight - k0.weight) * t;
		if (i < m_poseMorphTrackSlots.size() && m_poseMorphTrackSlots[i])
		{
			*m_poseMorphTrackSlots[i] = w;
		}
		else
		{
			poseMorphs.insert_or_assign(track.name, w);
		}
	}
}

void MmdAnimator::ApplyAutoBlinkForCurrentState(double dtSeconds, const VmdMotion* motion)
{
	if (!m_autoBlinkEnabled) return;

	const bool isPlaying = (motion != nullptr && !m_paused);
	if (!isPlaying)
	{
		UpdateAutoBlink(dtSeconds);

		// 既存のモーフ値(一時停止中のポーズなど)と比較し、目が閉じている度合いが大きい方を採用する
		float currentW = m_pose.morphWeights[L"まばたき"];
		m_pose.morphWeights[L"まばたき"] = std::max(currentW, m_blinkWeight);
		return;
	}

	// 再生中は干渉しないように状態をリセット（目は開けておく）
	m_blinkState = 0; // Open
	m_blinkTimer = 0.0f;
	m_blinkWeight = 0.0f;
}

void MmdAnimator::ApplyLookAtOffsetsToPose()
{
	using namespace DirectX;

	const float maxNeckYaw = XMConvertToRadians(50.0f);
	const float maxNeckPitchUp = XMConvertToRadians(25.0f);
	const float maxNeckPitchDown = XMConvertToRadians(35.0f);

	const float maxEyeYaw = XMConvertToRadians(20.0f);
	const float maxEyePitch = XMConvertToRadians(5.0f);

	const float deadZoneYaw = maxEyeYaw;
	const float deadZonePitchUp = maxEyePitch;
	const float deadZonePitchDown = maxEyePitch;

	const float pitchNeckGain = 1.25f;

	// ヘルパー: 目と首の配分計算
	auto ComputeBoneAngles = [&](float target, float deadZone, float maxNeck, float maxEyeLocal, float neckGain, float& outNeck, float& outEye)
		{
			if (std::abs(target) <= deadZone)
			{
				outNeck = 0.0f;
				outEye = std::clamp(target, -maxEyeLocal, maxEyeLocal);
			}
			else
			{
				float sign = (target >= 0.0f) ? 1.0f : -1.0f;
				float excess = target - (sign * deadZone);

				// neckGain を上げるほど、早めに首/頭が動く
				float neck = std::clamp(excess * neckGain, -maxNeck, maxNeck);
				outNeck = neck;

				// 目は残差だけ担当（ただし可動範囲でクランプ）
				outEye = std::clamp(target - neck, -maxEyeLocal, maxEyeLocal);
			}
		};

	float neckYaw, eyeYaw;
	ComputeBoneAngles(m_lookAtYaw, deadZoneYaw, maxNeckYaw, maxEyeYaw, 1.0f, neckYaw, eyeYaw);

	bool isUpward = (m_lookAtPitch > 0.0f);
	float currentDeadZonePitch = isUpward ? deadZonePitchUp : deadZonePitchDown;
	float currentMaxNeckPitch = isUpward ? maxNeckPitchUp : maxNeckPitchDown;

	float neckPitch, eyePitch;
	ComputeBoneAngles(m_lookAtPitch, currentDeadZonePitch, currentMaxNeckPitch, maxEyePitch, pitchNeckGain, neckPitch, eyePitch);

	const float neckYawW = 0.45f;
	const float headYawW = 0.55f;
	const float neckPitchW = 0.30f;
	const float headPitchW = 0.70f;

	XMVECTOR qNeck = XMQuaternionRotationRollPitchYaw(neckPitch * neckPitchW, neckYaw * neckYawW, 0.0f);
	XMVECTOR qHead = XMQuaternionRotationRollPitchYaw(neckPitch * headPitchW, neckYaw * headYawW, 0.0f);
	XMVECTOR qEyes = XMQuaternionRotationRollPitchYaw(eyePitch, eyeYaw, 0.0f);

	auto ApplyRot = [&](int32_t idx, XMVECTOR qOffset) {
		if (idx < 0) return;
		const auto& name = m_model->Bones()[idx].name;

		XMVECTOR current = XMQuaternionIdentity();
		if (m_pose.boneRotations.count(name))
		{
			current = XMLoadFloat4(&m_pose.boneRotations[name]);
		}
		XMVECTOR next = XMQuaternionMultiply(current, qOffset);
		XMStoreFloat4(&m_pose.boneRotations[name], next);
		};

	ApplyRot(m_boneIdxNeck, qNeck);
	ApplyRot(m_boneIdxHead, qHead);
	ApplyRot(m_boneIdxEyeL, qEyes);
	ApplyRot(m_boneIdxEyeR, qEyes);
}

void MmdAnimator::StepPhysicsWorld(double dtSeconds)
{
	if (!m_physicsEnabled || !m_model) return;
	if (m_model->RigidBodies().empty() && m_model->SoftBodies().empty()) return;

	if (!m_physicsWorld->IsBuilt() || m_physicsWorld->BuiltRevision() != m_model->Revision())
	{
		m_physicsWorld->BuildFromModel(*m_model, *m_boneSolver);
	}

	if (m_physicsWorld->IsBuilt())
	{
		m_physicsWorld->Step(dtSeconds, *m_model, *m_boneSolver, m_resolvedMorphWeights);
	}
}

void MmdAnimator::FinalizeTickState(float currentFrame)
{
	m_hasSkinnedPose = true;
	m_prevFrameForPhysics = currentFrame;
	m_prevFrameForPhysicsValid = true;
	m_lastPose = m_pose;
	m_hasLastPose = true;

	DirectX::XMStoreFloat4x4(&m_motionTransform, DirectX::XMMatrixIdentity());
}

void MmdAnimator::Tick(double dtSeconds)
{
	const VmdMotion* motion = m_motion.get();

	if (!m_paused)
	{
		m_time += dtSeconds;
		if (motion)
		{
			const double cycleSeconds =
				static_cast<double>(motion->MaxFrame() + 1) / std::max(1.0, m_fps);
			if (cycleSeconds > 0.0)
			{
				m_time = std::fmod(m_time, cycleSeconds);
				if (m_time < 0.0)
				{
					m_time += cycleSeconds;
				}
			}
		}
	}

	if (!m_model)
	{
		m_hasSkinnedPose = false;
		return;
	}

	// キャッシュ更新
	UpdateMotionCache(motion);
	if (m_resetPhysicsOnNextTick)
	{
		m_physicsWorld->Reset();
		m_prevFrameForPhysicsValid = false;
		m_resetPhysicsOnNextTick = false;
	}

	const float currentFrame = ComputeCurrentFrame(motion);
	HandlePhysicsResetForFrame(motion, currentFrame);
	ResetPoseForFrame(currentFrame);

	bool isMotionActive = (motion != nullptr && !m_paused);

	if (motion)
	{
		ApplyMotionTracksToPose(*motion, currentFrame);
	}

	ApplyAutoBlinkForCurrentState(dtSeconds, motion);

	if (!isMotionActive && m_breathingEnabled)
	{
		UpdateBreath(dtSeconds);
	}
	else if (isMotionActive)
	{
		//何かしてもいいけど
	}

	if (m_lookAtEnabled && m_model)
	{
		ApplyLookAtOffsetsToPose();
	}

	ApplyPoseTransition(dtSeconds);
	ResolveMorphsAndApplyBoneMorphs();

	m_boneSolver->ApplyPose(m_pose);
	const bool usePhysics =
		m_physicsEnabled &&
		m_model &&
		(!m_model->RigidBodies().empty() || !m_model->SoftBodies().empty()) &&
		!m_suspendPhysicsForLoopTransition &&
		!m_resetPhysicsOnNextTick;
	if (usePhysics)
	{
		m_boneSolver->UpdateMatricesBeforePhysics();
		StepPhysicsWorld(dtSeconds);
		m_boneSolver->UpdateMatricesAfterPhysics();
	}
	else
	{
		m_boneSolver->UpdateMatrices();
	}

	FinalizeTickState(currentFrame);
}

const std::vector<DirectX::XMFLOAT4X4>& MmdAnimator::GetSkinningMatrices() const
{
	return m_boneSolver->GetSkinningMatrices();
}

const std::vector<DirectX::XMFLOAT3>& MmdAnimator::SoftBodyVertexPositions() const
{
	return m_physicsWorld->SoftBodyVertexPositions();
}

const std::vector<uint8_t>& MmdAnimator::SoftBodyVertexMask() const
{
	return m_physicsWorld->SoftBodyVertexMask();
}

const std::vector<std::uint32_t>& MmdAnimator::SoftBodyActiveVertexIndices() const
{
	return m_physicsWorld->SoftBodyActiveVertexIndices();
}

bool MmdAnimator::HasSoftBodyVertexOverrides() const
{
	return m_physicsWorld->HasSoftBodyVertexOverrides();
}

size_t MmdAnimator::GetBoneCount() const
{
	return m_boneSolver->BoneCount();
}

bool MmdAnimator::LoadModel(const std::filesystem::path& pmx, std::function<void(float, const wchar_t*)> onProgress)
{
	auto model = std::make_unique<PmxModel>();
	if (model->Load(pmx, onProgress))
	{
		SetModel(std::move(model));
		return true;
	}
	return false;
}

void MmdAnimator::SetModel(std::unique_ptr<PmxModel> model)
{
	m_model = std::move(model);
	m_time = 0.0;
	m_pose = {};
	m_hasSkinnedPose = false;
	m_hasLastPose = false;
	m_hasTransitionPose = false;
	m_transitionActive = false;
	m_prevFrameForPhysicsValid = false;
	m_cachedMotionPtr = nullptr;
	m_boneTrackToBoneIndex.clear();
	m_boneKeyCursors.clear();
	m_morphKeyCursors.clear();
	m_poseTranslationTrackSlots.clear();
	m_poseRotationTrackSlots.clear();
	m_poseMorphTrackSlots.clear();
	m_morphVisitState.clear();
	m_suspendPhysicsForLoopTransition = false;
	m_resetPhysicsOnNextTick = false;
	m_boneSolver->Initialize(m_model.get());
	for (const auto& [key, transform] : m_externalParentTransforms)
	{
		m_boneSolver->SetExternalParentTransform(key, transform);
	}
	m_physicsWorld->Reset();
	CacheLookAtBones();
	CacheMorphIndices();
	ResetUpdateClock();
}

void MmdAnimator::ClearExternalParentTransforms()
{
	m_externalParentTransforms.clear();
	if (m_boneSolver)
	{
		m_boneSolver->ClearExternalParentTransforms();
	}
}

void MmdAnimator::SetExternalParentTransform(std::int32_t key,
											 const DirectX::XMFLOAT3& translation,
											 const DirectX::XMFLOAT4& rotation)
{
	BoneSolver::ExternalParentTransform transform{};
	transform.translation = translation;
	transform.rotation = rotation;
	m_externalParentTransforms.insert_or_assign(key, transform);
	if (m_boneSolver)
	{
		m_boneSolver->SetExternalParentTransform(key, transform);
	}
}

void MmdAnimator::ResetUpdateClock()
{
	m_lastUpdate = std::chrono::steady_clock::now();
	m_updateAccumulator = 0.0;
}

double MmdAnimator::ComputeFixedUpdateStep() const
{
	const double physicsStep = static_cast<double>(m_physicsWorld->GetSettings().fixedTimeStep);
	if (physicsStep > 0.0)
	{
		return physicsStep;
	}

	return 1.0 / std::max(1.0, m_fps);
}

void MmdAnimator::GetBounds(float& minx, float& miny, float& minz, float& maxx, float& maxy, float& maxz) const
{
	if (m_hasSkinnedPose && m_boneSolver)
	{
		DirectX::XMFLOAT3 mn, mx;
		m_boneSolver->GetBoneBounds(mn, mx);
		minx = mn.x; miny = mn.y; minz = mn.z;
		maxx = mx.x; maxy = mx.y; maxz = mx.z;
	}
	else if (m_model)
	{
		m_model->GetBounds(minx, miny, minz, maxx, maxy, maxz);
	}
	else
	{
		minx = miny = minz = -1.0f;
		maxx = maxy = maxz = 1.0f;
	}

	DirectX::XMFLOAT3 softMin{};
	DirectX::XMFLOAT3 softMax{};
	if (!m_physicsWorld->TryGetSoftBodyBounds(softMin, softMax))
	{
		return;
	}

	minx = std::min(minx, softMin.x);
	miny = std::min(miny, softMin.y);
	minz = std::min(minz, softMin.z);
	maxx = std::max(maxx, softMax.x);
	maxy = std::max(maxy, softMax.y);
	maxz = std::max(maxz, softMax.z);
}

void MmdAnimator::SetLookAtState(bool enabled, float yaw, float pitch)
{
	m_lookAtEnabled = enabled;
	m_lookAtYaw = yaw;
	m_lookAtPitch = pitch;

	const float limit = DirectX::XMConvertToRadians(90.0f);
	m_lookAtYaw = std::clamp(m_lookAtYaw, -limit, limit);
	m_lookAtPitch = std::clamp(m_lookAtPitch, -limit, limit);
}

int MmdAnimator::ResolveBoneIndex(const std::wstring& boneName) const
{
	if (!m_boneSolver)
	{
		return -1;
	}

	if (boneName == L"頭" && m_boneIdxHead >= 0)
	{
		return m_boneIdxHead;
	}

	return m_boneSolver->FindBoneIndex(boneName);
}

DirectX::XMFLOAT3 MmdAnimator::GetBoneGlobalPosition(const std::wstring& boneName) const
{
	if (!m_boneSolver || !m_model) return { 0,0,0 };

	const int idx = ResolveBoneIndex(boneName);

	if (idx >= 0 && idx < (int)m_boneSolver->BoneCount())
	{
		const auto& mat = m_boneSolver->GetBoneGlobalMatrix(idx);
		return { mat._41, mat._42, mat._43 };
	}
	return { 0,0,0 };
}

void MmdAnimator::SetLookAtTarget(bool enabled, const DirectX::XMFLOAT3& targetPos)
{
	using namespace DirectX;

	m_lookAtEnabled = enabled;
	if (!enabled)
	{
		m_lookAtYaw = 0.0f;
		m_lookAtPitch = 0.0f;
		return;
	}

	if (!m_boneSolver || !m_model) return;

	// 参照ボーン（首があれば首、無ければ頭）
	int32_t refIdx = (m_boneIdxNeck >= 0) ? m_boneIdxNeck : m_boneIdxHead;
	if (refIdx < 0) return;

	const auto& refM = m_boneSolver->GetBoneGlobalMatrix(refIdx);
	XMVECTOR refPos = XMVectorSet(refM._41, refM._42, refM._43, 1.0f);

	XMVECTOR target = XMLoadFloat3(&targetPos);
	XMVECTOR dir = XMVectorSubtract(target, refPos);
	float dirLenSq = XMVectorGetX(XMVector3LengthSq(dir));
	if (dirLenSq < 1e-8f)
	{
		m_lookAtYaw = 0.0f; m_lookAtPitch = 0.0f; return;
	}
	dir = XMVector3Normalize(dir);

	// ref のローカル基底（行ベクトル想定）
	XMVECTOR right = XMVector3Normalize(XMVectorSet(refM._11, refM._12, refM._13, 0.0f));
	XMVECTOR up = XMVector3Normalize(XMVectorSet(refM._21, refM._22, refM._23, 0.0f));
	XMVECTOR fwd = XMVector3Normalize(XMVectorSet(refM._31, refM._32, refM._33, 0.0f));

	// 「顔の正面」が -Z のモデル対策（目ボーンの位置で符号判定）
	if (m_boneIdxEyeL >= 0 && m_boneIdxEyeR >= 0)
	{
		const auto& mL = m_boneSolver->GetBoneGlobalMatrix(m_boneIdxEyeL);
		const auto& mR = m_boneSolver->GetBoneGlobalMatrix(m_boneIdxEyeR);

		XMVECTOR eyeMid = XMVectorScale(
			XMVectorAdd(
				XMVectorSet(mL._41, mL._42, mL._43, 1.0f),
				XMVectorSet(mR._41, mR._42, mR._43, 1.0f)
			),
			0.5f
		);

		XMVECTOR faceDir = XMVectorSubtract(eyeMid, refPos);
		float faceLenSq = XMVectorGetX(XMVector3LengthSq(faceDir));
		if (faceLenSq > 1e-8f)
		{
			faceDir = XMVector3Normalize(faceDir);
			float sign = XMVectorGetX(XMVector3Dot(faceDir, fwd));
			if (sign < 0.0f)
			{
				// face が -fwd 側 → bone(+Z) を -dir に向けると face(-Z) が target に向く
				dir = XMVectorNegate(dir);
			}
		}
	}

	float x = XMVectorGetX(XMVector3Dot(dir, right));
	float y = XMVectorGetX(XMVector3Dot(dir, up));
	float z = XMVectorGetX(XMVector3Dot(dir, fwd));

	float yaw = std::atan2(x, z);
	float pitch = std::atan2(y, z);

	const float limit = XMConvertToRadians(90.0f);
	m_lookAtYaw = std::clamp(yaw, -limit, limit);
	m_lookAtPitch = std::clamp(pitch, -limit, limit);
}

void MmdAnimator::UpdateAutoBlink(double dt)
{
	const float closeSpeed = 0.1f;  // 閉じるのにかかる時間
	const float keepClosed = 0.05f; // 閉じている時間
	const float openSpeed = 0.15f;  // 開くのにかかる時間

	m_blinkTimer += static_cast<float>(dt);

	switch (m_blinkState)
	{
		case 0: // Open (待機中)
			if (m_blinkTimer >= m_nextBlinkInterval)
			{
				m_blinkState = 1;
				m_blinkTimer = 0.0f;
			}
			m_blinkWeight = 0.0f;
			break;

		case 1: // Closing
		{
			float t = m_blinkTimer / closeSpeed;
			if (t >= 1.0f)
			{
				t = 1.0f;
				m_blinkState = 2;
				m_blinkTimer = 0.0f;
			}
			m_blinkWeight = t;
			break;
		}

		case 2: // Closed (維持)
			if (m_blinkTimer >= keepClosed)
			{
				m_blinkState = 3;
				m_blinkTimer = 0.0f;
			}
			m_blinkWeight = 1.0f;
			break;

		case 3: // Opening
		{
			float t = m_blinkTimer / openSpeed;
			if (t >= 1.0f)
			{
				t = 1.0f;
				m_blinkState = 0;
				m_blinkTimer = 0.0f;
				m_nextBlinkInterval = NextBlinkIntervalSeconds();
			}
			m_blinkWeight = 1.0f - t;
			break;
		}
	}
}

DirectX::XMFLOAT4X4 MmdAnimator::GetBoneGlobalMatrix(const std::wstring& boneName) const
{
	if (!m_boneSolver || !m_model)
	{
		DirectX::XMFLOAT4X4 identity;
		DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
		return identity;
	}

	const int idx = ResolveBoneIndex(boneName);

	if (idx >= 0 && idx < (int)m_boneSolver->BoneCount())
	{
		return m_boneSolver->GetBoneGlobalMatrix(idx);
	}

	DirectX::XMFLOAT4X4 identity;
	DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
	return identity;
}

void MmdAnimator::UpdateBreath(double dt)
{
	m_breathTime += dt;

	// パラメータ設定 (高品質な挙動のための定数)
	// 呼吸の基本周期: 約3.5秒
	const double mainPeriod = 3.5;
	// ゆらぎ周期: 約13秒 (大きくゆっくりした変化)
	const double slowPeriod = 13.0;

	using namespace DirectX;

	// 1. 基本の呼吸波形 (胸の上下)
	// sin^3 を使うことで、「吸って、止めて、吐いて、止めて」の緩急をつける
	const double phase = m_breathTime * (2.0 * XM_PI / mainPeriod);
	const double baseWave = std::pow(std::sin(phase), 3.0); // -1.0 ~ 1.0

	// 2. ゆらぎ成分 (1/fゆらぎ的なゆっくりした変化)
	const double slowWave = std::sin(m_breathTime * (2.0 * XM_PI / slowPeriod));

	// 3. 最終的な強度係数
	// ゆらぎを少し混ぜて、機械的な繰り返し感を消す
	const float intensity = static_cast<float>((baseWave + slowWave * 0.2) * 0.5);

	// 各ボーンへの適用
	auto ApplyBoneRot = [&](const std::wstring& name, float pitch, float yaw, float roll)
		{
			// 既存の回転を取得 (LookAtなどで既に設定されている場合があるため合成する)
			XMVECTOR currentQ = XMQuaternionIdentity();
			if (m_pose.boneRotations.count(name))
			{
				currentQ = XMLoadFloat4(&m_pose.boneRotations[name]);
			}

			// オイラー角から追加回転を作成 (ラジアン)
			XMVECTOR addQ = XMQuaternionRotationRollPitchYaw(pitch, yaw, roll);

			// 現在の回転に適用
			XMVECTOR nextQ = XMQuaternionMultiply(currentQ, addQ);
			XMStoreFloat4(&m_pose.boneRotations[name], nextQ);
		};

	// ボーンごとの微調整 (モデルに合わせて微調整してください)
	// 上半身: 呼吸のメイン。前後にわずかに揺れる (Pitch)
	// 吸うとき(intensity > 0)に少し反り、吐くときに戻る
	ApplyBoneRot(L"上半身", intensity * XMConvertToRadians(1.5f), 0.0f, 0.0f);

	// 上半身2: 上半身の動きを増幅または遅延させる
	// 少し位相をずらすとより有機的になりますが、ここでは単純な連動とします
	ApplyBoneRot(L"上半身2", intensity * XMConvertToRadians(1.8f), 0.0f, 0.0f);

	// 首・頭: 体の動きに対して少し遅れてバランスを取る (逆位相気味に)
	// 体が反ると顎を引くような動きを入れると視線が安定する
	ApplyBoneRot(L"首", intensity * XMConvertToRadians(-0.8f), 0.0f, 0.0f);
	ApplyBoneRot(L"頭", intensity * XMConvertToRadians(-0.5f), 0.0f, 0.0f);

	// 肩: 吸うときにわずかに上がる (Roll) - Z軸
	// 左肩(Z+) 右肩(Z-)
	ApplyBoneRot(L"左肩", 0.0f, 0.0f, intensity * XMConvertToRadians(1.0f));
	ApplyBoneRot(L"右肩", 0.0f, 0.0f, intensity * XMConvertToRadians(-1.0f));
}

void MmdAnimator::ApplyPoseTransition(double dtSeconds)
{
	if (!m_transitionActive || !m_hasTransitionPose)
	{
		return;
	}

	if (m_transitionNeedsInit)
	{
		m_transitionDuration = ComputeAdaptiveTransitionDuration(m_transitionPose, m_pose);
		m_transitionElapsed = 0.0;
		m_transitionNeedsInit = false;
		if (m_transitionDuration <= std::numeric_limits<double>::epsilon())
		{
			m_transitionActive = false;
			return;
		}
	}


	m_transitionElapsed += dtSeconds;
	float t = static_cast<float>(m_transitionElapsed / m_transitionDuration);
	float alpha = EvaluateTransitionAlpha(t);

	auto lerp = [](float a, float b, float s)
		{
			return a + (b - a) * s;
		};

	// --- ボーン平行移動 ---
	for (auto& [name, target] : m_pose.boneTranslations)
	{
		DirectX::XMFLOAT3 from{ 0.0f, 0.0f, 0.0f };
		auto it = m_transitionPose.boneTranslations.find(name);
		if (it != m_transitionPose.boneTranslations.end())
		{
			from = it->second;
		}

		target.x = lerp(from.x, target.x, alpha);
		target.y = lerp(from.y, target.y, alpha);
		target.z = lerp(from.z, target.z, alpha);
	}
	for (const auto& [name, from] : m_transitionPose.boneTranslations)
	{
		if (m_pose.boneTranslations.find(name) != m_pose.boneTranslations.end()) continue;
		m_pose.boneTranslations.insert_or_assign(name, DirectX::XMFLOAT3{
			lerp(from.x, 0.0f, alpha),
			lerp(from.y, 0.0f, alpha),
			lerp(from.z, 0.0f, alpha)
												 });
	}

	// --- ボーン回転 ---
	auto LoadQuatOrIdentity = [](const std::unordered_map<std::wstring, DirectX::XMFLOAT4>& map, const std::wstring& name)
		{
			auto it = map.find(name);
			if (it != map.end())
			{
				return DirectX::XMLoadFloat4(&it->second);
			}
			return DirectX::XMQuaternionIdentity();
		};

	for (auto& [name, target] : m_pose.boneRotations)
	{
		using namespace DirectX;
		XMVECTOR toQ = XMQuaternionNormalize(XMLoadFloat4(&target));
		XMVECTOR fromQ = XMQuaternionNormalize(LoadQuatOrIdentity(m_transitionPose.boneRotations, name));
		XMVECTOR blended = XMQuaternionSlerp(fromQ, toQ, alpha);

		XMStoreFloat4(&target, blended);
	}
	for (const auto& [name, from] : m_transitionPose.boneRotations)
	{
		if (m_pose.boneRotations.find(name) != m_pose.boneRotations.end()) continue;
		using namespace DirectX;
		XMVECTOR fromQ = XMQuaternionNormalize(XMLoadFloat4(&from));
		XMVECTOR blended = XMQuaternionSlerp(fromQ, XMQuaternionIdentity(), alpha);
		DirectX::XMFLOAT4 out{};
		XMStoreFloat4(&out, blended);
		m_pose.boneRotations.insert_or_assign(name, out);
	}

	// --- モーフ ---
	for (auto& [name, target] : m_pose.morphWeights)
	{
		float from = 0.0f;
		auto it = m_transitionPose.morphWeights.find(name);
		if (it != m_transitionPose.morphWeights.end())
		{
			from = it->second;
		}
		target = lerp(from, target, alpha);
	}
	for (const auto& [name, from] : m_transitionPose.morphWeights)
	{
		if (m_pose.morphWeights.find(name) != m_pose.morphWeights.end()) continue;
		m_pose.morphWeights.insert_or_assign(name, lerp(from, 0.0f, alpha));
	}

	if (t >= 1.0f)
	{
		m_transitionActive = false;
		if (m_suspendPhysicsForLoopTransition)
		{
			m_suspendPhysicsForLoopTransition = false;
			m_resetPhysicsOnNextTick = true;
		}
	}
}

double MmdAnimator::ComputeAdaptiveTransitionDuration(const BonePose& from, const BonePose& to) const
{
	using namespace DirectX;

	float maxTranslation = 0.0f;
	float maxMorph = 0.0f;
	float maxRotDeg = 0.0f;

	auto lengthDiff = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
		{
			float dx = a.x - b.x;
			float dy = a.y - b.y;
			float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};

	auto getQuat = [](const std::unordered_map<std::wstring, DirectX::XMFLOAT4>& map, const std::wstring& name)
		{
			auto it = map.find(name);
			if (it != map.end())
			{
				return DirectX::XMLoadFloat4(&it->second);
			}
			return DirectX::XMQuaternionIdentity();
		};

	auto accumulateBoneTranslation = [&](const std::wstring& name, const DirectX::XMFLOAT3& target, bool targetIsInTo)
		{
			DirectX::XMFLOAT3 source{ 0.0f,0.0f,0.0f };
			auto it = from.boneTranslations.find(name);
			if (it != from.boneTranslations.end())
			{
				source = it->second;
			}
			else if (!targetIsInTo)
			{
				return;
			}
			maxTranslation = std::max(maxTranslation, lengthDiff(source, target));
		};

	for (const auto& [name, target] : to.boneTranslations)
	{
		accumulateBoneTranslation(name, target, true);
	}
	for (const auto& entry : from.boneTranslations)
	{
		const auto& name = entry.first;
		if (to.boneTranslations.find(name) == to.boneTranslations.end())
		{
			accumulateBoneTranslation(name, DirectX::XMFLOAT3{}, false);
		}
	}

	auto accumulateBoneRotation = [&](const std::wstring& name, XMVECTOR target, bool targetIsInTo)
		{
			XMVECTOR source = getQuat(from.boneRotations, name);
			if (!targetIsInTo && from.boneRotations.find(name) == from.boneRotations.end())
			{
				return;
			}
			XMVECTOR fromN = XMQuaternionNormalize(source);
			XMVECTOR toN = XMQuaternionNormalize(target);
			float dot = std::abs(XMVectorGetX(XMQuaternionDot(fromN, toN)));
			dot = std::clamp(dot, -1.0f, 1.0f);
			float angleRad = 2.0f * std::acos(dot);
			maxRotDeg = std::max(maxRotDeg, XMConvertToDegrees(angleRad));
		};

	for (const auto& [name, target] : to.boneRotations)
	{
		accumulateBoneRotation(name, DirectX::XMLoadFloat4(&target), true);
	}
	for (const auto& entry : from.boneRotations)
	{
		const auto& name = entry.first;
		if (to.boneRotations.find(name) == to.boneRotations.end())
		{
			accumulateBoneRotation(name, DirectX::XMQuaternionIdentity(), false);
		}
	}

	for (const auto& [name, target] : to.morphWeights)
	{
		float fromWeight = 0.0f;
		auto it = from.morphWeights.find(name);
		if (it != from.morphWeights.end())
		{
			fromWeight = it->second;
		}
		maxMorph = std::max(maxMorph, std::abs(target - fromWeight));
	}
	for (const auto& [name, source] : from.morphWeights)
	{
		if (to.morphWeights.find(name) == to.morphWeights.end())
		{
			maxMorph = std::max(maxMorph, std::abs(source));
		}
	}

	float translationWeight = maxTranslation * 0.5f;
	float rotationWeight = maxRotDeg / 90.0f;
	float morphWeight = maxMorph;
	float dominant = std::max({ translationWeight, rotationWeight, morphWeight });

	const double minDuration = 0.18;
	const double maxDuration = 1.0;
	double duration = minDuration + static_cast<double>(dominant) * 0.45;
	return std::clamp(duration, minDuration, maxDuration);
}

float MmdAnimator::EvaluateTransitionAlpha(float t) const
{
	float alpha = std::clamp(t, 0.0f, 1.0f);
	// smootherstep で立ち上がりを滑らかにする
	return alpha * alpha * alpha * (alpha * (alpha * 6.0f - 15.0f) + 10.0f);
}
