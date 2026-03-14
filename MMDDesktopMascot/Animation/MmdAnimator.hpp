#pragma once
#include <filesystem>
#include <cstdint>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>
#include "PmxModel.hpp"
#include "VmdMotion.hpp"
#include "BoneSolver.hpp"
#include "Settings.hpp"

class MmdPhysicsWorld;

class MmdAnimator
{
public:
	using Pose = BonePose;

	MmdAnimator();
	~MmdAnimator();

	bool LoadModel(const std::filesystem::path& pmx);
	bool LoadModel(const std::filesystem::path& pmx, std::function<void(float, const wchar_t*)> onProgress);
	void SetModel(std::unique_ptr<PmxModel> model);

	bool LoadMotion(const std::filesystem::path& vmd);
	void ClearMotion();
	void StopMotion();

	void Tick(double dtSeconds);
	void Update();

	bool IsPaused() const
	{
		return m_paused;
	}
	void SetPaused(bool paused)
	{
		m_paused = paused;
	}
	void TogglePause()
	{
		m_paused = !m_paused;
	}

	bool PhysicsEnabled() const
	{
		return m_physicsEnabled;
	}
	void SetPhysicsEnabled(bool enabled)
	{
		m_physicsEnabled = enabled;
	}
	void TogglePhysics()
	{
		m_physicsEnabled = !m_physicsEnabled;
	}

	void SetPhysicsSettings(const PhysicsSettings& settings);
	const PhysicsSettings& GetPhysicsSettings() const;

	// --- LookAt 機能 ---
	void SetLookAtState(bool enabled, float yaw, float pitch);
	void SetLookAtTarget(bool enabled, const DirectX::XMFLOAT3& targetPos);
	DirectX::XMFLOAT3 GetBoneGlobalPosition(const std::wstring& boneName) const;
	DirectX::XMFLOAT4X4 GetBoneGlobalMatrix(const std::wstring& boneName) const;
	// ------------------

	const PmxModel* Model() const
	{
		return m_model.get();
	}
	const VmdMotion* Motion() const
	{
		return m_motion.get();
	}

	const Pose& CurrentPose() const
	{
		return m_pose;
	}
	const std::vector<float>& ResolvedMorphWeights() const
	{
		return m_resolvedMorphWeights;
	}
	const std::vector<DirectX::XMFLOAT3>& SoftBodyVertexPositions() const;
	const std::vector<uint8_t>& SoftBodyVertexMask() const;
	const std::vector<std::uint32_t>& SoftBodyActiveVertexIndices() const;
	bool HasSoftBodyVertexOverrides() const;
	const DirectX::XMFLOAT4X4& MotionTransform() const
	{
		return m_motionTransform;
	}

	double TimeSeconds() const
	{
		return m_time;
	}
	uint64_t ModelRevision() const
	{
		return m_model ? m_model->Revision() : 0;
	}

	const std::vector<DirectX::XMFLOAT4X4>& GetSkinningMatrices() const;
	size_t GetBoneCount() const;
	bool HasSkinnedPose() const
	{
		return m_hasSkinnedPose;
	}

	void GetBounds(float& minx, float& miny, float& minz, float& maxx, float& maxy, float& maxz) const;

	void SetAutoBlinkEnabled(bool enabled)
	{
		m_autoBlinkEnabled = enabled;
	}
	bool AutoBlinkEnabled() const
	{
		return m_autoBlinkEnabled;
	}

	void GetLookAtState(bool& enabled, float& yaw, float& pitch) const
	{
		enabled = m_lookAtEnabled;
		yaw = m_lookAtYaw;
		pitch = m_lookAtPitch;
	}

	void SetBreathingEnabled(bool enabled)
	{
		m_breathingEnabled = enabled;
	}
	bool BreathingEnabled() const
	{
		return m_breathingEnabled;
	}
	void ClearExternalParentTransforms();
	void SetExternalParentTransform(std::int32_t key,
									const DirectX::XMFLOAT3& translation,
									const DirectX::XMFLOAT4& rotation);

private:
	std::unique_ptr<PmxModel> m_model;
	std::unique_ptr<VmdMotion> m_motion;
	std::unique_ptr<BoneSolver> m_boneSolver;

	std::unique_ptr<MmdPhysicsWorld> m_physicsWorld;
	bool m_physicsEnabled{ true };

	double m_time{};
	double m_fps{ 30.0 };

	std::chrono::steady_clock::time_point m_lastUpdate;
	bool m_paused{ false };
	bool m_firstUpdate{ true };
	bool m_hasSkinnedPose{ false };

	Pose m_pose{};
	DirectX::XMFLOAT4X4 m_motionTransform{};

	float m_prevFrameForPhysics{ 0.0f };
	bool  m_prevFrameForPhysicsValid{ false };

	// キャッシュ用メンバ変数
	const VmdMotion* m_cachedMotionPtr = nullptr;
	std::vector<int> m_boneTrackToBoneIndex;
	std::vector<size_t> m_boneKeyCursors;
	std::vector<size_t> m_morphKeyCursors;
	std::vector<DirectX::XMFLOAT3*> m_poseTranslationTrackSlots;
	std::vector<DirectX::XMFLOAT4*> m_poseRotationTrackSlots;
	std::vector<float*> m_poseMorphTrackSlots;
	std::unordered_map<std::wstring, int> m_morphNameToIndex;
	std::vector<float> m_resolvedMorphWeights;
	std::vector<uint8_t> m_morphVisitState;

	// LookAt用
	bool m_lookAtEnabled{ false };
	float m_lookAtYaw{ 0.0f };
	float m_lookAtPitch{ 0.0f };

	// LookAt対象ボーンのインデックスキャッシュ
	int32_t m_boneIdxHead{ -1 };
	int32_t m_boneIdxNeck{ -1 };
	int32_t m_boneIdxEyeL{ -1 };
	int32_t m_boneIdxEyeR{ -1 };

	BonePose m_lastPose{};
	bool m_hasLastPose{ false };
	BonePose m_transitionPose{};
	bool m_hasTransitionPose{ false };
	bool m_transitionActive{ false };
	double m_transitionElapsed{ 0.0 };
	double m_transitionDuration{ 0.3 };
	bool m_transitionNeedsInit{ false };
	bool m_suspendPhysicsForLoopTransition{ false };
	bool m_resetPhysicsOnNextTick{ false };

	void CacheLookAtBones();
	float ComputeCurrentFrame(const VmdMotion* motion) const;
	void HandlePhysicsResetForFrame(const VmdMotion* motion, float currentFrame);
	void ResetPoseForFrame(float currentFrame);
	void ApplyMotionTracksToPose(const VmdMotion& motion, float currentFrame);
	void ApplyAutoBlinkForCurrentState(double dtSeconds, const VmdMotion* motion);
	void ApplyLookAtOffsetsToPose();
	void StepPhysicsWorld(double dtSeconds);
	void FinalizeTickState(float currentFrame);

	void UpdateMotionCache(const VmdMotion* motion);
	int ResolveBoneIndex(const std::wstring& boneName) const;
	void CacheMorphIndices();
	void ResolveMorphsAndApplyBoneMorphs();
	void AccumulateMorphWeight(int morphIndex, float weight, std::vector<uint8_t>& visitState);
	void ApplyBoneMorph(const PmxModel::Morph& morph, float weight);

	bool m_autoBlinkEnabled{ false };
	float m_blinkTimer{ 0.0f };       // 次の動作までのタイマー
	float m_blinkWeight{ 0.0f };      // 現在のまばたきウェイト
	int m_blinkState{ 0 };            // 0:Open, 1:Closing, 2:Closed, 3:Opening
	float m_nextBlinkInterval{ 3.0f }; // 次のまばたきまでの時間

	void UpdateAutoBlink(double dt);

	bool m_breathingEnabled = false;
	double m_breathTime = 0.0;
	void UpdateBreath(double dt);
	void ApplyPoseTransition(double dtSeconds);
	void BeginPoseTransitionFromLastPose();
	double ComputeAdaptiveTransitionDuration(const BonePose& from, const BonePose& to) const;
	float EvaluateTransitionAlpha(float t) const;
	void ResetUpdateClock();
	double ComputeFixedUpdateStep() const;
	std::unordered_map<std::int32_t, BoneSolver::ExternalParentTransform> m_externalParentTransforms;
	double m_updateAccumulator{ 0.0 };
};
