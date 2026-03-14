#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <array>
#include <DirectXMath.h>
#include "PmxModel.hpp"
#include "BoneSolver.hpp"
#include "Settings.hpp"

class btCollisionConfiguration;
class btCollisionDispatcher;
class btBroadphaseInterface;
class btConstraintSolver;
class btDiscreteDynamicsWorld;
class btSoftRigidDynamicsWorld;
class btCollisionShape;
struct btDefaultMotionState;
class btRigidBody;
class btTypedConstraint;
class btSoftBody;

class MmdPhysicsWorld
{
public:
	using Settings = PhysicsSettings;

	MmdPhysicsWorld();
	~MmdPhysicsWorld();

	void Reset();

	void BuildFromModel(const PmxModel& model, const BoneSolver& bones);
	void Step(double dtSeconds,
			  const PmxModel& model,
			  BoneSolver& bones,
			  const std::vector<float>& morphWeights);

	bool IsBuilt() const
	{
		return m_isBuilt;
	}
	uint64_t BuiltRevision() const
	{
		return m_builtRevision;
	}
	Settings& GetSettings()
	{
		return m_settings;
	}
	const Settings& GetSettings() const
	{
		return m_settings;
	}
	const std::vector<DirectX::XMFLOAT3>& SoftBodyVertexPositions() const
	{
		return m_softBodyVertexPositions;
	}
	const std::vector<uint8_t>& SoftBodyVertexMask() const
	{
		return m_softBodyVertexMask;
	}
	const std::vector<std::uint32_t>& SoftBodyActiveVertexIndices() const
	{
		return m_softBodyActiveVertexIndices;
	}
	bool HasSoftBodyVertexOverrides() const noexcept
	{
		return m_hasSoftBodyVertexOverrides;
	}
	bool TryGetSoftBodyBounds(DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax) const noexcept
	{
		if (!m_hasSoftBodyVertexOverrides)
		{
			return false;
		}

		outMin = m_softBodyBoundsMin;
		outMax = m_softBodyBoundsMax;
		return true;
	}

private:
	struct Body
	{
		int defIndex{ -1 };
		int boneIndex{ -1 };
		PmxModel::RigidBody::OperationType operation{ PmxModel::RigidBody::OperationType::Static };

		DirectX::XMFLOAT4X4 localFromBone{};

		DirectX::XMFLOAT3 position{};
		DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };

		DirectX::XMFLOAT3 prevPosition{};
		DirectX::XMFLOAT4 prevRotation{ 0.0f, 0.0f, 0.0f, 1.0f };

		DirectX::XMFLOAT3 kinematicTargetPos{};
		DirectX::XMFLOAT4 kinematicTargetRot{};
		DirectX::XMFLOAT3 kinematicStartPos{};
		DirectX::XMFLOAT4 kinematicStartRot{};

		DirectX::XMFLOAT3 linearVelocity{};
		DirectX::XMFLOAT3 angularVelocity{};

		float invMass{ 0.0f };

		PmxModel::RigidBody::ShapeType shapeType{ PmxModel::RigidBody::ShapeType::Sphere };
		DirectX::XMFLOAT3 shapeSize{};
		float capsuleRadius{ 0.0f };
		float capsuleHalfHeight{ 0.0f };



		DirectX::XMFLOAT3 capsuleLocalAxis{ 0.0f, 1.0f, 0.0f };
		int group{ 0 };
		uint16_t groupMask{ 0 };
		float friction{ 0.5f };
		float restitution{ 0.0f };

		float linearDamping{ 0.0f };
		float angularDamping{ 0.0f };
		bool hadContactThisStep{ false };
	};

	struct JointConstraint
	{
		int bodyA{ -1 };
		int bodyB{ -1 };

		DirectX::XMFLOAT3 localAnchorA{};
		DirectX::XMFLOAT3 localAnchorB{};

		DirectX::XMFLOAT4 rotAtoJ{};
		DirectX::XMFLOAT4 rotBtoJ{};

		DirectX::XMFLOAT3 posLower{};
		DirectX::XMFLOAT3 posUpper{};
		DirectX::XMFLOAT3 rotLower{};
		DirectX::XMFLOAT3 rotUpper{};

		DirectX::XMFLOAT3 positionSpring{};
		DirectX::XMFLOAT3 rotationSpring{};

		float lambdaPos{ 0.0f };
	};

	struct SoftBodyInstance
	{
		int defIndex{ -1 };
		std::vector<int> nodeVertexIndices;
	};

	void BuildConstraints(const PmxModel& model);

	void PrecomputeKinematicTargets(
		const PmxModel& model,
		const std::vector<DirectX::XMFLOAT4X4>& startBoneGlobals,
		const std::vector<DirectX::XMFLOAT4X4>& targetBoneGlobals);
	void ApplyImpulseMorphs(const PmxModel& model, const std::vector<float>& morphWeights);
	void UpdateSoftBodyVertexOverrides(const PmxModel& model);

	void WriteBackBones(const PmxModel& model, BoneSolver& bones);
	void BuildWriteBackOrder(const PmxModel& model);
	void CaptureAnimationBoneGlobals(
		const BoneSolver& bones,
		std::vector<DirectX::XMFLOAT4X4>& outGlobals) const;

	static DirectX::XMVECTOR Load3(const DirectX::XMFLOAT3& v);
	static DirectX::XMVECTOR Load4(const DirectX::XMFLOAT4& v);
	static void Store3(DirectX::XMFLOAT3& o, DirectX::XMVECTOR v);
	static void Store4(DirectX::XMFLOAT4& o, DirectX::XMVECTOR v);
	static DirectX::XMMATRIX MatrixFromTR(const DirectX::XMFLOAT3& t, const DirectX::XMFLOAT4& r);
	static void DecomposeTR(const DirectX::XMMATRIX& m, DirectX::XMFLOAT3& outT, DirectX::XMFLOAT4& outR);
	static float ComputeDepth(const std::vector<PmxModel::Bone>& bones, int boneIndex);
	DirectX::XMFLOAT3 ExtractTranslation(const DirectX::XMMATRIX& m);

	Settings m_settings{};

	bool m_isBuilt{ false };
	uint64_t m_builtRevision{ 0 };

	std::vector<Body> m_bodies;
	std::vector<JointConstraint> m_joints;
	std::vector<SoftBodyInstance> m_softBodies;

	void AdvanceSingleFixedStep(
		float stepDt,
		const PmxModel& model,
		const std::vector<DirectX::XMFLOAT4X4>& startBoneGlobals,
		const std::vector<DirectX::XMFLOAT4X4>& targetBoneGlobals);
	void InitializeRealBulletWorld(const PmxModel& model);
	void DestroyRealBulletWorld();
	void RunRealBulletFixedStep(float fixedStepDt);

	struct CollisionShapeCache
	{
		DirectX::XMVECTOR p0;
		DirectX::XMVECTOR p1;
		DirectX::XMVECTOR rotation;
		float radius;
		float ex, ey, ez;
		bool isBox;
	};

	std::vector<int> m_writeBackOrder;
	std::vector<uint8_t> m_keepTranslationFlags;
	std::vector<DirectX::XMFLOAT4X4> m_desiredGlobals;
	std::vector<DirectX::XMFLOAT4X4> m_appliedGlobals;
	std::vector<uint8_t> m_hasDesiredGlobal;
	std::vector<uint8_t> m_hasAppliedGlobal;
	std::vector<DirectX::XMFLOAT3> m_originalLocalTranslation;
	std::vector<DirectX::XMFLOAT4X4> m_currAnimationBoneGlobals;
	std::vector<DirectX::XMFLOAT4X4> m_prevAnimationBoneGlobals;
	bool m_hasPrevAnimationBoneGlobals{ false };
	float m_stepAccumulatorSeconds{ 0.0f };
	bool m_useDirectCollisionMaskSemantics{ false };
	std::vector<float> m_prevImpulseMorphWeights;
	std::vector<DirectX::XMFLOAT3> m_softBodyVertexPositions;
	std::vector<uint8_t> m_softBodyVertexMask;
	std::vector<std::uint32_t> m_softBodyActiveVertexIndices;
	DirectX::XMFLOAT3 m_softBodyBoundsMin{};
	DirectX::XMFLOAT3 m_softBodyBoundsMax{};
	bool m_hasSoftBodyVertexOverrides{ false };

#if defined(_DEBUG)
	struct PhysicsDebugStats
	{
		uint64_t subStepCounter{ 0 };
		uint32_t skippedConnectedPairs{ 0 };
		uint32_t candidatePairs{ 0 };
		uint32_t contactPairs{ 0 };
		float maxPenetration{ 0.0f };
		int maxPenBodyA{ -1 };
		int maxPenBodyB{ -1 };
		uint32_t contactAngularClampCount{ 0 };
		float contactMaxThetaRaw{ 0.0f };
		float contactMaxThetaApplied{ 0.0f };
		std::array<uint32_t, 16 * 16> groupPairContactCount{};
		std::array<float, 16 * 16> groupPairMaxPenetration{};
		uint32_t group02ContactCount{ 0 };
		float group02NormalZSum{ 0.0f };
		float group02DynamicLinearDzSum{ 0.0f };
		uint32_t group02CapsuleBoxCount{ 0 };
		float group02CapsuleBoxDynamicLinearDzSum{ 0.0f };
		uint32_t group02BoxCapsuleCount{ 0 };
		float group02BoxCapsuleDynamicLinearDzSum{ 0.0f };
		std::vector<float> group02BodyDz;
		std::vector<uint32_t> group02BodyContacts;
		std::vector<float> group02StaticDz;
		std::vector<uint32_t> group02StaticContacts;
		float group02MaxPenetration{ 0.0f };
		int group02MaxPenBodyA{ -1 };
		int group02MaxPenBodyB{ -1 };
		float group02MaxPenNz{ 0.0f };
		float group02MaxPenZA{ 0.0f };
		float group02MaxPenZB{ 0.0f };
		float group02MaxPenInvMassA{ 0.0f };
		float group02MaxPenInvMassB{ 0.0f };
		float group02MaxPenWAngA{ 0.0f };
		float group02MaxPenWAngB{ 0.0f };
		float group02MaxPenWTotal{ 0.0f };
		float group02MaxPenDLambda{ 0.0f };
		float group02MaxPenLeverA{ 0.0f };
		float group02MaxPenLeverB{ 0.0f };
		uint32_t group02MaxPenSegInside{ 0u };

		void Reset()
		{
			subStepCounter = 0;
			skippedConnectedPairs = 0;
			candidatePairs = 0;
			contactPairs = 0;
			maxPenetration = 0.0f;
			maxPenBodyA = -1;
			maxPenBodyB = -1;
			contactAngularClampCount = 0;
			contactMaxThetaRaw = 0.0f;
			contactMaxThetaApplied = 0.0f;
			groupPairContactCount.fill(0u);
			groupPairMaxPenetration.fill(0.0f);
			group02ContactCount = 0;
			group02NormalZSum = 0.0f;
			group02DynamicLinearDzSum = 0.0f;
			group02CapsuleBoxCount = 0;
			group02CapsuleBoxDynamicLinearDzSum = 0.0f;
			group02BoxCapsuleCount = 0;
			group02BoxCapsuleDynamicLinearDzSum = 0.0f;
			group02BodyDz.clear();
			group02BodyContacts.clear();
			group02StaticDz.clear();
			group02StaticContacts.clear();
			group02MaxPenetration = 0.0f;
			group02MaxPenBodyA = -1;
			group02MaxPenBodyB = -1;
			group02MaxPenNz = 0.0f;
			group02MaxPenZA = 0.0f;
			group02MaxPenZB = 0.0f;
			group02MaxPenInvMassA = 0.0f;
			group02MaxPenInvMassB = 0.0f;
			group02MaxPenWAngA = 0.0f;
			group02MaxPenWAngB = 0.0f;
			group02MaxPenWTotal = 0.0f;
			group02MaxPenDLambda = 0.0f;
			group02MaxPenLeverA = 0.0f;
			group02MaxPenLeverB = 0.0f;
			group02MaxPenSegInside = 0u;
		}
	};

	PhysicsDebugStats m_debugStats{};
#endif

	std::unique_ptr<btCollisionConfiguration> m_btCollisionConfig;
	std::unique_ptr<btCollisionDispatcher> m_btDispatcher;
	std::unique_ptr<btBroadphaseInterface> m_btBroadphase;
	std::unique_ptr<btConstraintSolver> m_btSolver;
	std::unique_ptr<btDiscreteDynamicsWorld> m_btWorld;
	btSoftRigidDynamicsWorld* m_btSoftWorld{};
	std::vector<std::unique_ptr<btCollisionShape>> m_btShapes;
	std::vector<std::unique_ptr<btDefaultMotionState>> m_btMotionStates;
	std::vector<std::unique_ptr<btRigidBody>> m_btRigidBodies;
	std::vector<std::unique_ptr<btTypedConstraint>> m_btConstraints;
	std::vector<std::unique_ptr<btSoftBody>> m_btSoftBodies;
};
