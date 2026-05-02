#include "MmdPhysicsWorld.hpp"
#include "BulletPhysicsAdapter.hpp"
#include "PhysicsDebugLog.hpp"
#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpringConstraint.h>
#include <BulletSoftBody/btSoftBody.h>
#include <BulletSoftBody/btSoftBodyHelpers.h>
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#include <BulletSoftBody/btSoftRigidDynamicsWorld.h>

using namespace DirectX;

namespace
{
	static constexpr float kEps = 1.0e-6f;
	static constexpr float kBigEps = 1.0e-4f;
	static constexpr float kPmxBoxHalfExtentScale = 1.0f;
	static constexpr float kViewerCompatConstraintStopErp = 0.475f;
	static constexpr std::uint8_t kSupportedSoftBodyShapeTriMesh = 0;
	static constexpr std::uint8_t kSupportedSoftBodyShapeRope = 1;
	static constexpr std::uint8_t kSoftBodyFlagBendingLinks = 0x01;
	static constexpr std::uint8_t kSoftBodyFlagClusters = 0x02;
	static constexpr std::uint8_t kSoftBodyFlagSelfCollision = 0x20;

	bool HasMeaningfulTransformDelta(
		const XMFLOAT3& fromPos,
		const XMFLOAT4& fromRot,
		const XMFLOAT3& toPos,
		const XMFLOAT4& toRot,
		float posThresholdSq = 1.0e-12f,
		float rotThreshold = 1.0e-8f)
	{
		const float dx = toPos.x - fromPos.x;
		const float dy = toPos.y - fromPos.y;
		const float dz = toPos.z - fromPos.z;
		const float posDeltaSq = dx * dx + dy * dy + dz * dz;

		const float rotDot =
			std::fabs(fromRot.x * toRot.x +
					  fromRot.y * toRot.y +
					  fromRot.z * toRot.z +
					  fromRot.w * toRot.w);

		// Bone-driven kinematic bodies can move only a tiny amount per 120 Hz physics step.
		// If we filter those deltas too aggressively, colliders lag behind the animation,
		// then catch up in larger jumps that show up as clipping and jitter.
		return posDeltaSq > posThresholdSq || (1.0f - rotDot) > rotThreshold;
	}

	bool HasMeaningfulPositionDelta(
		const XMFLOAT3& fromPos,
		const XMFLOAT3& toPos,
		float posThresholdSq = 1.0e-10f)
	{
		const float dx = toPos.x - fromPos.x;
		const float dy = toPos.y - fromPos.y;
		const float dz = toPos.z - fromPos.z;
		return (dx * dx + dy * dy + dz * dz) > posThresholdSq;
	}

	class BindPoseGlobalCache
	{
	public:
		explicit BindPoseGlobalCache(const std::vector<PmxModel::Bone>& bones)
			: m_bones(bones)
			, m_bindGlobals(bones.size())
			, m_bindDone(bones.size(), 0)
		{
		}

		DirectX::XMMATRIX Get(int boneIndex)
		{
			if (boneIndex < 0 || boneIndex >= static_cast<int>(m_bones.size()))
			{
				return DirectX::XMMatrixIdentity();
			}

			const size_t index = static_cast<size_t>(boneIndex);
			if (m_bindDone[index])
			{
				return DirectX::XMLoadFloat4x4(&m_bindGlobals[index]);
			}

			const auto& bone = m_bones[index];
			DirectX::XMMATRIX parentGlobal = DirectX::XMMatrixIdentity();
			if (bone.parentIndex >= 0)
			{
				parentGlobal = Get(bone.parentIndex);
			}

			DirectX::XMVECTOR relative = DirectX::XMLoadFloat3(&bone.position);
			if (bone.parentIndex >= 0)
			{
				relative = DirectX::XMVectorSubtract(
					relative,
					DirectX::XMLoadFloat3(&m_bones[static_cast<size_t>(bone.parentIndex)].position));
			}

			const DirectX::XMMATRIX global = DirectX::XMMatrixTranslationFromVector(relative) * parentGlobal;
			DirectX::XMStoreFloat4x4(&m_bindGlobals[index], global);
			m_bindDone[index] = 1;
			return global;
		}

	private:
		const std::vector<PmxModel::Bone>& m_bones;
		std::vector<DirectX::XMFLOAT4X4> m_bindGlobals;
		std::vector<uint8_t> m_bindDone;
	};


	struct SapNode
	{
		float minX; int index;
	};

	struct SapPair
	{
		int a; int b;
	};

	inline static float Length3(XMVECTOR v)
	{
		return XMVectorGetX(XMVector3Length(v));
	}

	inline static bool IsVectorFinite3(XMVECTOR v)
	{
		XMFLOAT3 f; XMStoreFloat3(&f, v);
		return std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z);
	}

	inline static bool IsVectorFinite4(XMVECTOR v)
	{
		XMFLOAT4 f; XMStoreFloat4(&f, v);
		return std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && std::isfinite(f.w);
	}

	inline static XMVECTOR SafeNormalize3(XMVECTOR v)
	{
		float len = Length3(v);
		if (len < kBigEps || !std::isfinite(len)) return XMVectorZero();
		return XMVectorScale(v, 1.0f / len);
	}

	inline static int PopCount16(uint16_t x)
	{
		return std::popcount(x);
	}

	inline static bool HasSoftBodyFlag(std::uint8_t flags, std::uint8_t mask)
	{
		return (flags & mask) != 0;
	}

	inline static bool IsSupportedSoftBodyShape(std::uint8_t shape)
	{
		return shape == kSupportedSoftBodyShapeTriMesh ||
			   shape == kSupportedSoftBodyShapeRope;
	}

	inline static const char* SoftBodyShapeToString(std::uint8_t shape)
	{
		switch (shape)
		{
			case kSupportedSoftBodyShapeTriMesh: return "TriMesh";
			case kSupportedSoftBodyShapeRope: return "Rope";
			default: return "Unknown";
		}
	}

	inline static XMVECTOR QuaternionFromAngularVelocity(FXMVECTOR w, float dt)
	{
		XMVECTOR axisLen = XMVector3Length(w);
		float angle = XMVectorGetX(axisLen) * dt;
		if (angle < kEps || !std::isfinite(angle)) return XMQuaternionIdentity();

		XMVECTOR axis = XMVector3Normalize(w);
		if (!IsVectorFinite3(axis)) return XMQuaternionIdentity();

		return XMQuaternionRotationAxis(axis, angle);
	}

	inline static XMVECTOR QuaternionFromRotationVector(FXMVECTOR rv)
	{
		float angle = Length3(rv);
		if (angle < kEps || !std::isfinite(angle)) return XMQuaternionIdentity();

		XMVECTOR axis = XMVectorScale(rv, 1.0f / angle);
		if (!IsVectorFinite3(axis)) return XMQuaternionIdentity();

		return XMQuaternionRotationAxis(axis, angle);
	}

	inline static XMVECTOR SafeQuaternionRotationAxis(FXMVECTOR axis, float angle)
	{
		if (!std::isfinite(angle) || std::abs(angle) < kEps) return XMQuaternionIdentity();
		float len = Length3(axis);
		if (!std::isfinite(len) || len < kEps) return XMQuaternionIdentity();
		XMVECTOR nAxis = XMVectorScale(axis, 1.0f / len);
		return XMQuaternionRotationAxis(nAxis, angle);
	}

	inline static XMVECTOR QuaternionDeltaToAngularVelocity(FXMVECTOR dqIn, float dt)
	{
		dt = std::max(dt, kEps);
		XMVECTOR dq = dqIn;
		if (!IsVectorFinite4(dq)) return XMVectorZero();

		if (XMVectorGetW(dq) < 0.0f) dq = XMVectorNegate(dq);

		float w = std::clamp(XMVectorGetW(dq), -1.0f, 1.0f);
		float angle = 2.0f * std::acos(w);
		if (!std::isfinite(angle)) angle = 0.0f;
		if (angle > XM_PI) angle -= XM_2PI;

		float s = std::sqrt(std::max(0.0f, 1.0f - w * w));
		XMVECTOR axis;
		if (s < 1.0e-5f || std::abs(angle) < 1.0e-5f)
		{
			axis = XMVectorZero();
			angle = 0.0f;
		}
		else
		{
			axis = XMVectorScale(XMVectorSet(XMVectorGetX(dq), XMVectorGetY(dq), XMVectorGetZ(dq), 0.0f), 1.0f / s);
			axis = XMVector3Normalize(axis);
		}

		if (!IsVectorFinite3(axis)) return XMVectorZero();

		XMVECTOR omega = XMVectorScale(axis, angle / dt);
		return omega;
	}

	inline static XMVECTOR QuaternionToRotationVector(FXMVECTOR qIn)
	{
		XMVECTOR q = XMQuaternionNormalize(qIn);
		if (!IsVectorFinite4(q)) return XMVectorZero();
		if (XMVectorGetW(q) < 0.0f) q = XMVectorNegate(q);

		const float w = std::clamp(XMVectorGetW(q), -1.0f, 1.0f);
		float angle = 2.0f * std::acos(w);
		if (!std::isfinite(angle) || angle < kEps) return XMVectorZero();
		if (angle > XM_PI) angle -= XM_2PI;

		const float s = std::sqrt(std::max(0.0f, 1.0f - w * w));
		if (s < 1.0e-6f) return XMVectorZero();

		XMVECTOR axis = XMVectorSet(XMVectorGetX(q), XMVectorGetY(q), XMVectorGetZ(q), 0.0f);
		axis = XMVectorScale(axis, 1.0f / s);
		axis = SafeNormalize3(axis);
		if (XMVector3Equal(axis, XMVectorZero())) return XMVectorZero();
		return XMVectorScale(axis, angle);
	}

	inline static XMVECTOR RotateVector(FXMVECTOR v, FXMVECTOR q)
	{
		return XMVector3Rotate(v, q);
	}

	inline static float Dot3(XMVECTOR a, XMVECTOR b)
	{
		return XMVectorGetX(XMVector3Dot(a, b));
	}

	inline static float Clamp01(float v)
	{
		return std::min(1.0f, std::max(0.0f, v));
	}

	inline static float Lerp01(float a, float b, float t)
	{
		return a + (b - a) * Clamp01(t);
	}

	inline static void ClosestPtSegmentSegment(
		FXMVECTOR p1, FXMVECTOR q1,
		FXMVECTOR p2, FXMVECTOR q2,
		XMVECTOR& outC1, XMVECTOR& outC2)
	{
		XMVECTOR d1 = XMVectorSubtract(q1, p1);
		XMVECTOR d2 = XMVectorSubtract(q2, p2);
		XMVECTOR r = XMVectorSubtract(p1, p2);

		float a = Dot3(d1, d1);
		float e = Dot3(d2, d2);
		float f = Dot3(d2, r);

		float s = 0.0f, t = 0.0f;

		if (a <= kEps && e <= kEps)
		{
			outC1 = p1; outC2 = p2; return;
		}

		if (a <= kEps)
		{
			s = 0.0f;
			t = Clamp01(f / std::max(e, kEps));
		}
		else
		{
			float c = Dot3(d1, r);
			if (e <= kEps)
			{
				t = 0.0f;
				s = Clamp01(-c / std::max(a, kEps));
			}
			else
			{
				float b = Dot3(d1, d2);
				float denom = a * e - b * b;
				s = (std::abs(denom) > kEps) ? Clamp01((b * f - c * e) / denom) : 0.0f;
				float tnom = b * s + f;
				if (tnom <= 0.0f)
				{
					t = 0.0f; s = Clamp01(-c / std::max(a, kEps));
				}
				else if (tnom >= e)
				{
					t = 1.0f; s = Clamp01((b - c) / std::max(a, kEps));
				}
				else
				{
					t = tnom / e;
				}
			}
		}
		outC1 = XMVectorAdd(p1, XMVectorScale(d1, s));
		outC2 = XMVectorAdd(p2, XMVectorScale(d2, t));
	}



	inline static DirectX::XMMATRIX MatrixRotationEulerXYZ(float rx, float ry, float rz)
	{

		return DirectX::XMMatrixRotationX(rx)
			* DirectX::XMMatrixRotationY(ry)
			* DirectX::XMMatrixRotationZ(rz);
	}

	inline static btVector3 ToBtVector3(const DirectX::XMFLOAT3& v)
	{
		return btVector3(v.x, v.y, v.z);
	}

	inline static btQuaternion ToBtQuaternion(const DirectX::XMFLOAT4& q)
	{
		return btQuaternion(q.x, q.y, q.z, q.w);
	}

	inline static DirectX::XMFLOAT3 ToXmFloat3(const btVector3& v)
	{
		return DirectX::XMFLOAT3(v.x(), v.y(), v.z());
	}

	inline static DirectX::XMFLOAT4 ToXmFloat4(const btQuaternion& q)
	{
		return DirectX::XMFLOAT4(q.x(), q.y(), q.z(), q.w());
	}

	inline static btTransform ToBtTransform(const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT4& q)
	{
		btTransform t;
		t.setIdentity();
		t.setOrigin(ToBtVector3(p));
		t.setRotation(ToBtQuaternion(q));
		return t;
	}

	inline static uint16_t BitInvert16(uint16_t mask)
	{
		return static_cast<uint16_t>(~mask);
	}

	inline static uint16_t ToCollisionGroupBit(int groupIndex)
	{
		if (std::getenv("MMD_PHYSICS_GROUP_ONE_BASED"))
		{
			--groupIndex;
		}
		return static_cast<uint16_t>(1u << std::clamp(groupIndex, 0, 15));
	}

	inline static uint16_t ToBulletCollisionMask(uint16_t pmxMask, bool useDirectSemantics)
	{
		return useDirectSemantics ? pmxMask : BitInvert16(pmxMask);
	}

	static bool ResolveDirectCollisionMaskSemantics(
		const std::vector<PmxModel::RigidBody>& rigidBodies,
		const std::vector<PmxModel::SoftBody>& softBodies,
		uint64_t& outDirectPairCount,
		uint64_t& outInversePairCount)
	{
		auto countAcceptedPairs = [&](bool useDirectSemantics) -> uint64_t
		{
			uint64_t acceptedPairCount = 0;

			for (size_t i = 0; i < rigidBodies.size(); ++i)
			{
				const uint16_t groupA = ToCollisionGroupBit(rigidBodies[i].groupIndex);
				const uint16_t maskA = ToBulletCollisionMask(rigidBodies[i].collisionGroupMask, useDirectSemantics);
				for (size_t j = i + 1; j < rigidBodies.size(); ++j)
				{
					const uint16_t groupB = ToCollisionGroupBit(rigidBodies[j].groupIndex);
					const uint16_t maskB = ToBulletCollisionMask(rigidBodies[j].collisionGroupMask, useDirectSemantics);
					if ((groupA & maskB) != 0 && (groupB & maskA) != 0)
					{
						++acceptedPairCount;
					}
				}
			}

			for (const auto& softBody : softBodies)
			{
				const uint16_t softGroup = ToCollisionGroupBit(softBody.groupIndex);
				const uint16_t softMask = ToBulletCollisionMask(softBody.collisionGroupMask, useDirectSemantics);
				for (const auto& rigidBody : rigidBodies)
				{
					const uint16_t rigidGroup = ToCollisionGroupBit(rigidBody.groupIndex);
					const uint16_t rigidMask = ToBulletCollisionMask(rigidBody.collisionGroupMask, useDirectSemantics);
					if ((softGroup & rigidMask) != 0 && (rigidGroup & softMask) != 0)
					{
						++acceptedPairCount;
					}
				}
			}

			return acceptedPairCount;
		};

		outDirectPairCount = countAcceptedPairs(true);
		outInversePairCount = countAcceptedPairs(false);
		if (outDirectPairCount != outInversePairCount)
		{
			return outDirectPairCount > outInversePairCount;
		}

		size_t fullMaskCount = 0;
		size_t zeroMaskCount = 0;
		for (const auto& rigidBody : rigidBodies)
		{
			if (rigidBody.collisionGroupMask == 0xffffu) ++fullMaskCount;
			if (rigidBody.collisionGroupMask == 0x0000u) ++zeroMaskCount;
		}
		for (const auto& softBody : softBodies)
		{
			if (softBody.collisionGroupMask == 0xffffu) ++fullMaskCount;
			if (softBody.collisionGroupMask == 0x0000u) ++zeroMaskCount;
		}

		return fullMaskCount >= zeroMaskCount;
	}
}

MmdPhysicsWorld::MmdPhysicsWorld() = default;

MmdPhysicsWorld::~MmdPhysicsWorld()
{
	Reset();
}

void MmdPhysicsWorld::Reset()
{
	DestroyRealBulletWorld();

	m_isBuilt = false;
	m_builtRevision = 0;
	m_bodies.clear();
	m_joints.clear();
	m_softBodies.clear();
	m_writeBackOrder.clear();
	m_keepTranslationFlags.clear();
	m_desiredGlobals.clear();
	m_appliedGlobals.clear();
	m_hasDesiredGlobal.clear();
	m_hasAppliedGlobal.clear();
	m_originalLocalTranslation.clear();
	m_currAnimationBoneGlobals.clear();
	m_prevAnimationBoneGlobals.clear();
	m_hasPrevAnimationBoneGlobals = false;
	m_stepAccumulatorSeconds = 0.0f;
	m_useDirectCollisionMaskSemantics = false;
	m_prevImpulseMorphWeights.clear();
	m_softBodyVertexPositions.clear();
	m_softBodyVertexMask.clear();
	m_softBodyActiveVertexIndices.clear();
	m_softBodyBoundsMin = {};
	m_softBodyBoundsMax = {};
	m_hasSoftBodyVertexOverrides = false;
#if defined(_DEBUG)
	m_debugStats.Reset();
#endif
}

void MmdPhysicsWorld::BuildFromModel(const PmxModel& model, const BoneSolver& bones)
{
	Reset();
	mmd::physics::debuglog::Truncate();
	{
		std::ostringstream oss;
		oss << "BuildFromModel begin revision=" << model.Revision()
			<< " backend=BulletPhysics";
		mmd::physics::debuglog::AppendLine(oss.str());
	}

	const auto& rbDefs = model.RigidBodies();
	const auto& softBodyDefs = model.SoftBodies();
	m_prevImpulseMorphWeights.assign(model.Morphs().size(), 0.0f);
	m_softBodyVertexPositions.assign(model.Vertices().size(), {});
	m_softBodyVertexMask.assign(model.Vertices().size(), 0);
	m_softBodyActiveVertexIndices.clear();
	m_softBodyBoundsMin = {};
	m_softBodyBoundsMax = {};
	m_hasSoftBodyVertexOverrides = false;
	{
		size_t unsupportedSoftBodyShapeCount = 0;
		unsigned softBodyFlagsMask = 0;
		for (const auto& softBody : softBodyDefs)
		{
			softBodyFlagsMask |= softBody.flags;
			if (!IsSupportedSoftBodyShape(softBody.shape))
			{
				++unsupportedSoftBodyShapeCount;
			}
		}

		std::ostringstream oss;
		oss << "ModelFeatureSummary"
			<< " rigidBodies=" << rbDefs.size()
			<< " softBodies=" << softBodyDefs.size()
			<< " softBodyFlagsMask=0x" << std::hex << softBodyFlagsMask << std::dec
			<< " unsupportedSoftBodyShapes=" << unsupportedSoftBodyShapeCount;
		mmd::physics::debuglog::AppendLine(oss.str());
	}

	if (rbDefs.empty() && softBodyDefs.empty())
	{
		m_isBuilt = true;
		m_builtRevision = model.Revision();
		return;
	}

	const auto& bonesDef = model.Bones();


	{
		uint64_t directPairCount = 0;
		uint64_t inversePairCount = 0;
		const bool resolvedDirectMaskSemantics = ResolveDirectCollisionMaskSemantics(
			rbDefs,
			softBodyDefs,
			directPairCount,
			inversePairCount);
		m_useDirectCollisionMaskSemantics = true;

		std::ostringstream oss;
		oss << "GroupMaskSemantics resolved="
			<< (m_useDirectCollisionMaskSemantics ? "collideMask(pmx)" : "nonCollideMask(~pmx)")
			<< " groupIndexBase=zeroBased"
			<< " resolvedDirect=" << (resolvedDirectMaskSemantics ? 1 : 0)
			<< " directBetter=" << ((directPairCount > inversePairCount) ? 1 : 0)
			<< " directPairs=" << directPairCount
			<< " inversePairs=" << inversePairCount;
		mmd::physics::debuglog::AppendLine(oss.str());
	}

	BindPoseGlobalCache bindPoseGlobals(bonesDef);

	m_bodies.reserve(rbDefs.size());
	m_bodies.resize(rbDefs.size());

	for (size_t i = 0; i < rbDefs.size(); ++i)
	{
		const auto& def = rbDefs[i];
		Body b{};
		b.defIndex = static_cast<int>(i);
		b.boneIndex = def.boneIndex;
		b.operation = def.operation;
		b.shapeType = def.shapeType;
		b.shapeSize = def.shapeSize;


		if (b.shapeType == PmxModel::RigidBody::ShapeType::Box)
		{
			float hx = def.shapeSize.x * kPmxBoxHalfExtentScale;
			float hy = def.shapeSize.y * kPmxBoxHalfExtentScale;
			float hz = def.shapeSize.z * kPmxBoxHalfExtentScale;

			float longHalf = hy;
			float o1 = hx, o2 = hz;
			b.capsuleLocalAxis = { 0.0f, 1.0f, 0.0f };

			if (hx >= hy && hx >= hz)
			{
				longHalf = hx; o1 = hy; o2 = hz;
				b.capsuleLocalAxis = { 1.0f, 0.0f, 0.0f };
			}
			else if (hz >= hy && hz >= hx)
			{
				longHalf = hz; o1 = hx; o2 = hy;
				b.capsuleLocalAxis = { 0.0f, 0.0f, 1.0f };
			}

			float radius = std::sqrt(o1 * o1 + o2 * o2);

			b.capsuleRadius = std::max(radius, 1.0e-4f);
			b.capsuleHalfHeight = std::max(0.0f, longHalf - b.capsuleRadius);
		}
		else if (b.shapeType == PmxModel::RigidBody::ShapeType::Capsule)
		{
			b.capsuleLocalAxis = { 0.0f, 1.0f, 0.0f };
			b.capsuleRadius = def.shapeSize.x;
			b.capsuleHalfHeight = def.shapeSize.y * 0.5f;
		}
		else
		{
			b.capsuleLocalAxis = { 0.0f, 1.0f, 0.0f };
			b.capsuleRadius = def.shapeSize.x;
			b.capsuleHalfHeight = 0.0f;
		}

		const BulletRigidBodyParams bulletParams = BulletPhysicsAdapter::ApproximateRigidBody(def);

		b.linearDamping = bulletParams.linearDamping;
		b.angularDamping = bulletParams.angularDamping;
		b.group = def.groupIndex;
		b.groupMask = def.collisionGroupMask;
		b.friction = bulletParams.friction;
		b.restitution = bulletParams.restitution;
		b.invMass = bulletParams.invMass;
		b.linearVelocity = { 0.0f, 0.0f, 0.0f };
		b.angularVelocity = { 0.0f, 0.0f, 0.0f };

		DirectX::XMMATRIX rb0 =
			MatrixRotationEulerXYZ(def.rotation.x, def.rotation.y, def.rotation.z) *
			DirectX::XMMatrixTranslation(def.position.x, def.position.y, def.position.z);

		DirectX::XMMATRIX localFromBone = DirectX::XMMatrixIdentity();
		if (def.boneIndex >= 0 && def.boneIndex < static_cast<int>(bonesDef.size()))
		{
			DirectX::XMMATRIX bindBoneG = bindPoseGlobals.Get(def.boneIndex);
			DirectX::XMMATRIX invBind = DirectX::XMMatrixInverse(nullptr, bindBoneG);
			localFromBone = rb0 * invBind;
		}
		DirectX::XMStoreFloat4x4(&b.localFromBone, localFromBone);

		DecomposeTR(rb0, b.position, b.rotation);
		b.prevPosition = b.position;
		b.prevRotation = b.rotation;
		b.kinematicStartPos = b.position;
		b.kinematicStartRot = b.rotation;
		b.kinematicTargetPos = b.position;
		b.kinematicTargetRot = b.rotation;

		m_bodies[i] = b;
	}


	{
		int staticCount = 0;
		int dynamicCount = 0;
		int generatedCount = 0;
		for (const Body& b : m_bodies)
		{
			if (b.invMass <= 0.0f) ++staticCount;
			else ++dynamicCount;
			if (b.defIndex < 0) ++generatedCount;
		}
		std::ostringstream oss;
		oss << "BodySummary total=" << m_bodies.size()
			<< " modelBodies=" << (m_bodies.size() - static_cast<size_t>(generatedCount))
			<< " generatedBodies=" << generatedCount
			<< " staticBodies=" << staticCount
			<< " dynamicBodies=" << dynamicCount;
		mmd::physics::debuglog::AppendLine(oss.str());
	}
	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		const Body& b = m_bodies[i];
		const bool isStatic = (b.invMass <= 0.0f);
		const char* opText = "Unknown";
		const char* shapeText = "Unknown";
		std::string bodyName = "<generated>";
		if (b.defIndex >= 0 && static_cast<size_t>(b.defIndex) < rbDefs.size())
		{
			bodyName = mmd::physics::debuglog::ToUtf8Lossy(rbDefs[static_cast<size_t>(b.defIndex)].name);
		}
		switch (b.operation)
		{
			case PmxModel::RigidBody::OperationType::Static: opText = "Static"; break;
			case PmxModel::RigidBody::OperationType::Dynamic: opText = "Dynamic"; break;
			case PmxModel::RigidBody::OperationType::DynamicAndPositionAdjust: opText = "DynamicAndPosAdjust"; break;
			default: break;
		}
		switch (b.shapeType)
		{
			case PmxModel::RigidBody::ShapeType::Sphere: shapeText = "Sphere"; break;
			case PmxModel::RigidBody::ShapeType::Box: shapeText = "Box"; break;
			case PmxModel::RigidBody::ShapeType::Capsule: shapeText = "Capsule"; break;
			default: break;
		}
			std::ostringstream oss;
			oss << "BodyMap idx=" << i
				<< " defIndex=" << b.defIndex
				<< " name=\"" << bodyName << "\""
			<< " boneIndex=" << b.boneIndex
			<< " op=" << opText
			<< " shape=" << shapeText
			<< " size=(" << b.shapeSize.x << "," << b.shapeSize.y << "," << b.shapeSize.z << ")"
			<< " static=" << (isStatic ? 1 : 0)
				<< " invMass=" << b.invMass
				<< " linDamp=" << b.linearDamping
				<< " angDamp=" << b.angularDamping
				<< " group=" << b.group
				<< " mask=0x" << std::hex << static_cast<unsigned>(b.groupMask) << std::dec
				<< " initPos=(" << b.position.x << "," << b.position.y << "," << b.position.z << ")";
			if (b.shapeType == PmxModel::RigidBody::ShapeType::Box)
			{
				const XMVECTOR q = Load4(b.rotation);
				const XMVECTOR axisZ = XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), q);
				oss << " axisZ=("
					<< XMVectorGetX(axisZ) << ","
					<< XMVectorGetY(axisZ) << ","
					<< XMVectorGetZ(axisZ) << ")";
			}
			else if (b.shapeType == PmxModel::RigidBody::ShapeType::Capsule)
			{
				const XMVECTOR q = Load4(b.rotation);
				const XMVECTOR axisLocal = Load3(b.capsuleLocalAxis);
				const XMVECTOR axis = XMVector3Rotate(axisLocal, q);
				oss << " axisCap=("
					<< XMVectorGetX(axis) << ","
					<< XMVectorGetY(axis) << ","
					<< XMVectorGetZ(axis) << ")";
			}
			mmd::physics::debuglog::AppendLine(oss.str());
		}

	BuildConstraints(model);

	const int n = static_cast<int>(m_bodies.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if(n >= 512)
#endif
	for (int i = 0; i < n; ++i)
	{
		Body& b = m_bodies[i];

		DirectX::XMMATRIX rbCurrent = MatrixFromTR(b.position, b.rotation);

		const int boneIndex = b.boneIndex;
		if (boneIndex >= 0 && boneIndex < static_cast<int>(bonesDef.size()))
		{
			const auto& boneGlobalF = bones.GetBoneGlobalMatrix(static_cast<size_t>(boneIndex));
			DirectX::XMMATRIX boneG = DirectX::XMLoadFloat4x4(&boneGlobalF);
			DirectX::XMMATRIX localFromBone = DirectX::XMLoadFloat4x4(&b.localFromBone);
			rbCurrent = localFromBone * boneG;
		}

		DecomposeTR(rbCurrent, b.position, b.rotation);
		b.prevPosition = b.position;
		b.prevRotation = b.rotation;
		b.kinematicStartPos = b.position;
		b.kinematicStartRot = b.rotation;
		b.kinematicTargetPos = b.position;
		b.kinematicTargetRot = b.rotation;
	}

	BuildWriteBackOrder(model);
	InitializeRealBulletWorld(model);

	if (m_settings.warmupSteps > 0 &&
		m_settings.fixedTimeStep > 0.0f &&
		m_btWorld)
	{
		std::vector<XMFLOAT4X4> warmupBoneGlobals;
		CaptureAnimationBoneGlobals(bones, warmupBoneGlobals);

		const int warmupSteps = std::clamp(m_settings.warmupSteps, 0, 240);
		for (int i = 0; i < warmupSteps; ++i)
		{
			PrecomputeKinematicTargets(model, warmupBoneGlobals, warmupBoneGlobals);
			RunRealBulletFixedStep(m_settings.fixedTimeStep);
		}

		for (size_t i = 0; i < m_bodies.size() && i < m_btRigidBodies.size(); ++i)
		{
			Body& body = m_bodies[i];
			btRigidBody* rigidBody = m_btRigidBodies[i].get();
			if (!rigidBody || body.invMass <= 0.0f)
			{
				continue;
			}

			rigidBody->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
			rigidBody->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
			rigidBody->clearForces();
			if (rigidBody->getMotionState())
			{
				btTransform currentTransform;
				rigidBody->getMotionState()->getWorldTransform(currentTransform);
				rigidBody->setInterpolationWorldTransform(currentTransform);
			}
			body.linearVelocity = { 0.0f, 0.0f, 0.0f };
			body.angularVelocity = { 0.0f, 0.0f, 0.0f };
		}

		std::ostringstream oss;
		oss << "WarmupComplete"
			<< " steps=" << warmupSteps
			<< " fixedTimeStep=" << m_settings.fixedTimeStep;
		mmd::physics::debuglog::AppendLine(oss.str());
	}
	CaptureAnimationBoneGlobals(bones, m_prevAnimationBoneGlobals);
	m_hasPrevAnimationBoneGlobals = true;
	m_stepAccumulatorSeconds = 0.0f;
	m_isBuilt = true;
	m_builtRevision = model.Revision();
}

void MmdPhysicsWorld::BuildConstraints(const PmxModel& model)
{
	m_joints.clear();

	const auto& joints = model.Joints();
	if (joints.empty()) return;
	m_joints.reserve(joints.size());
	double defAnchorErrSum = 0.0;
	float defAnchorErrMax = 0.0f;
	int defAnchorErrMaxJoint = -1;
	int defAnchorErrMaxBodyA = -1;
	int defAnchorErrMaxBodyB = -1;

	for (const auto& j : joints)
	{
		if (j.rigidBodyA < 0 || j.rigidBodyB < 0) continue;
		if (j.rigidBodyA >= static_cast<int>(m_bodies.size())) continue;
		if (j.rigidBodyB >= static_cast<int>(m_bodies.size())) continue;

		const Body& a0 = m_bodies[static_cast<size_t>(j.rigidBodyA)];
		const Body& b0 = m_bodies[static_cast<size_t>(j.rigidBodyB)];

		XMMATRIX Ta0 = MatrixFromTR(a0.position, a0.rotation);
		XMMATRIX Tb0 = MatrixFromTR(b0.position, b0.rotation);

		XMMATRIX Tj0 = MatrixRotationEulerXYZ(j.rotation.x, j.rotation.y, j.rotation.z) *
			XMMatrixTranslation(j.position.x, j.position.y, j.position.z);

		XMMATRIX invTa0 = XMMatrixInverse(nullptr, Ta0);
		XMMATRIX invTb0 = XMMatrixInverse(nullptr, Tb0);

		XMMATRIX J_in_A = Tj0 * invTa0;
		XMMATRIX J_in_B = Tj0 * invTb0;

		XMFLOAT3 tJA, tJB;
		XMFLOAT4 rJA, rJB;
		DecomposeTR(J_in_A, tJA, rJA);
		DecomposeTR(J_in_B, tJB, rJB);

		JointConstraint c{};
		c.bodyA = j.rigidBodyA;
		c.bodyB = j.rigidBodyB;
		c.localAnchorA = tJA;
		c.localAnchorB = tJB;
		c.rotAtoJ = rJA;
		c.rotBtoJ = rJB;

		c.posLower = j.positionLower;
		c.posUpper = j.positionUpper;
		c.rotLower = j.rotationLower;
		c.rotUpper = j.rotationUpper;
		c.positionSpring = j.springPosition;
		c.rotationSpring = j.springRotation;
		c.lambdaPos = 0.0f;

		{
			const XMVECTOR pA = Load3(a0.position);
			const XMVECTOR qA = Load4(a0.rotation);
			const XMVECTOR pB = Load3(b0.position);
			const XMVECTOR qB = Load4(b0.rotation);
			const XMVECTOR wA = XMVectorAdd(pA, RotateVector(Load3(c.localAnchorA), qA));
			const XMVECTOR wB = XMVectorAdd(pB, RotateVector(Load3(c.localAnchorB), qB));
			const XMVECTOR jPos = XMVectorSet(j.position.x, j.position.y, j.position.z, 0.0f);
			const float errA = Length3(XMVectorSubtract(wA, jPos));
			const float errB = Length3(XMVectorSubtract(wB, jPos));
			const float err = std::max(errA, errB);
			if (std::isfinite(err))
			{
				defAnchorErrSum += static_cast<double>(err);
				if (err > defAnchorErrMax)
				{
					defAnchorErrMax = err;
					defAnchorErrMaxJoint = static_cast<int>(m_joints.size());
					defAnchorErrMaxBodyA = c.bodyA;
					defAnchorErrMaxBodyB = c.bodyB;
				}
			}
		}

		m_joints.push_back(c);

		if (mmd::physics::debuglog::IsEnabled())
		{
			std::ostringstream oss;
			oss << "JointMap idx=" << (m_joints.size() - 1)
				<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(j.name) << "\""
				<< " bodyA=" << c.bodyA
				<< " bodyB=" << c.bodyB
				<< " posLow=(" << c.posLower.x << "," << c.posLower.y << "," << c.posLower.z << ")"
				<< " posUp=(" << c.posUpper.x << "," << c.posUpper.y << "," << c.posUpper.z << ")"
				<< " rotLow=(" << c.rotLower.x << "," << c.rotLower.y << "," << c.rotLower.z << ")"
				<< " rotUp=(" << c.rotUpper.x << "," << c.rotUpper.y << "," << c.rotUpper.z << ")"
				<< " springPos=(" << c.positionSpring.x << "," << c.positionSpring.y << "," << c.positionSpring.z << ")"
				<< " springRot=(" << c.rotationSpring.x << "," << c.rotationSpring.y << "," << c.rotationSpring.z << ")";
			mmd::physics::debuglog::AppendLine(oss.str());
		}

	}

	uint32_t dynamicStaticPairs = 0;
	uint32_t dynamicDynamicPairs = 0;
	uint32_t staticStaticPairs = 0;
	uint32_t dynamicStaticLogged = 0;
	uint32_t jointsWithLinearSpring = 0;
	float maxLinearSpring = 0.0f;
	double initPosErrSum = 0.0;
	double initAngErrSum = 0.0;
	uint32_t initErrCount = 0;
	float initPosErrMax = 0.0f;
	float initAngErrMax = 0.0f;
	int initPosErrMaxJoint = -1;
	int initAngErrMaxJoint = -1;
	for (size_t jointIndex = 0; jointIndex < m_joints.size(); ++jointIndex)
	{
		const auto& c = m_joints[jointIndex];
		const bool staticA = (m_bodies[static_cast<size_t>(c.bodyA)].invMass <= 0.0f);
		const bool staticB = (m_bodies[static_cast<size_t>(c.bodyB)].invMass <= 0.0f);
		int groupA = m_bodies[static_cast<size_t>(c.bodyA)].group;
		int groupB = m_bodies[static_cast<size_t>(c.bodyB)].group;
		const float linearSpring = std::max({
			std::abs(c.positionSpring.x),
			std::abs(c.positionSpring.y),
			std::abs(c.positionSpring.z) });
		maxLinearSpring = std::max(maxLinearSpring, linearSpring);
		if (linearSpring > kEps)
		{
			++jointsWithLinearSpring;
		}
		if (staticA == staticB)
		{
			if (staticA) ++staticStaticPairs;
			else ++dynamicDynamicPairs;
		}
		else
		{
			++dynamicStaticPairs;
			if (dynamicStaticLogged < 128)
			{
				++dynamicStaticLogged;
				std::ostringstream oss;
				oss << "JointDynamicStatic jointIndex=" << jointIndex
					<< " bodyA=" << c.bodyA
					<< " bodyB=" << c.bodyB
					<< " staticA=" << (staticA ? 1 : 0)
					<< " staticB=" << (staticB ? 1 : 0)
					<< " groupA=" << groupA
					<< " groupB=" << groupB;
				mmd::physics::debuglog::AppendLine(oss.str());
			}
		}

		const Body& A = m_bodies[static_cast<size_t>(c.bodyA)];
		const Body& B = m_bodies[static_cast<size_t>(c.bodyB)];
		const XMVECTOR pA = Load3(A.position);
		const XMVECTOR pB = Load3(B.position);
		const XMVECTOR qA = Load4(A.rotation);
		const XMVECTOR qB = Load4(B.rotation);
		const XMVECTOR rA = RotateVector(Load3(c.localAnchorA), qA);
		const XMVECTOR rB = RotateVector(Load3(c.localAnchorB), qB);
		const XMVECTOR wA = XMVectorAdd(pA, rA);
		const XMVECTOR wB = XMVectorAdd(pB, rB);
		const float posErr = Length3(XMVectorSubtract(wA, wB));
		if (std::isfinite(posErr))
		{
			initPosErrSum += static_cast<double>(posErr);
			if (posErr > initPosErrMax)
			{
				initPosErrMax = posErr;
				initPosErrMaxJoint = static_cast<int>(jointIndex);
			}
		}

		XMVECTOR qJwA = XMQuaternionMultiply(Load4(c.rotAtoJ), qA);
		XMVECTOR qJwB = XMQuaternionMultiply(Load4(c.rotBtoJ), qB);
		XMVECTOR qDiff = XMQuaternionMultiply(XMQuaternionConjugate(qJwA), qJwB);
		if (XMVectorGetW(qDiff) < 0.0f)
		{
			qDiff = XMVectorNegate(qDiff);
		}
		const float w = std::clamp(XMVectorGetW(qDiff), -1.0f, 1.0f);
		float angErr = 2.0f * std::acos(w);
		if (angErr > XM_PI) angErr = XM_2PI - angErr;
		if (std::isfinite(angErr))
		{
			initAngErrSum += static_cast<double>(angErr);
			if (angErr > initAngErrMax)
			{
				initAngErrMax = angErr;
				initAngErrMaxJoint = static_cast<int>(jointIndex);
			}
		}
		++initErrCount;
	}
	{
		std::ostringstream oss;
		oss << "JointSummary total=" << m_joints.size()
			<< " dynamicStatic=" << dynamicStaticPairs
			<< " dynamicDynamic=" << dynamicDynamicPairs
			<< " staticStatic=" << staticStaticPairs
			<< " linearSpringJoints=" << jointsWithLinearSpring
			<< " maxLinearSpring=" << maxLinearSpring;
		mmd::physics::debuglog::AppendLine(oss.str());
	}
	if (!m_joints.empty())
	{
		std::ostringstream oss;
		oss << "JointDefAnchorError avg=" << (defAnchorErrSum / static_cast<double>(m_joints.size()))
			<< " max=" << defAnchorErrMax
			<< " maxJoint=" << defAnchorErrMaxJoint
			<< " maxPair=(" << defAnchorErrMaxBodyA << "," << defAnchorErrMaxBodyB << ")";
		mmd::physics::debuglog::AppendLine(oss.str());
	}
	if (initErrCount > 0)
	{
		std::ostringstream oss;
		oss << "JointInitError posAvg=" << (initPosErrSum / static_cast<double>(initErrCount))
			<< " posMax=" << initPosErrMax
			<< " posMaxJoint=" << initPosErrMaxJoint
			<< " angAvgRad=" << (initAngErrSum / static_cast<double>(initErrCount))
			<< " angMaxRad=" << initAngErrMax
			<< " angMaxJoint=" << initAngErrMaxJoint;
		mmd::physics::debuglog::AppendLine(oss.str());
	}

}

void MmdPhysicsWorld::BuildWriteBackOrder(const PmxModel& model)
{
	const auto& rbDefs = model.RigidBodies();
	const auto& bonesDef = model.Bones();

	m_writeBackOrder.clear();
	m_keepTranslationFlags.assign(bonesDef.size(), 0);

	if (rbDefs.empty() || bonesDef.empty() || m_bodies.empty())
	{
		return;
	}

	const size_t bodyCount = std::min(rbDefs.size(), m_bodies.size());

	std::vector<int> nodes;
	nodes.reserve(bodyCount);
	std::vector<uint8_t> addedBone(bonesDef.size(), 0);
	auto ShouldWriteBack = [&](const PmxModel::RigidBody& def, const Body& b) -> bool
	{
		if (def.operation == PmxModel::RigidBody::OperationType::Static) return false;
		if (b.invMass <= 0.0f) return false;
		return true;
	};

	for (size_t i = 0; i < bodyCount; ++i)
	{
		const auto& def = rbDefs[i];

		const int boneIndex = def.boneIndex;
		if (boneIndex < 0 || boneIndex >= static_cast<int>(bonesDef.size())) continue;

		const Body& b = m_bodies[i];
		if (!ShouldWriteBack(def, b)) continue;

		if (!addedBone[static_cast<size_t>(boneIndex)])
		{
			nodes.push_back(boneIndex);
			addedBone[static_cast<size_t>(boneIndex)] = 1;
		}

		const auto& boneDef = bonesDef[static_cast<size_t>(boneIndex)];
		const bool keepTranslation =
			(def.operation == PmxModel::RigidBody::OperationType::DynamicAndPositionAdjust) ||
			!boneDef.CanTranslate();
		if (keepTranslation)
		{
			m_keepTranslationFlags[static_cast<size_t>(boneIndex)] = 1;
		}
	}

	if (nodes.empty())
	{
		return;
	}

	std::sort(nodes.begin(), nodes.end(), [&](int a, int b)
	{
		const bool aAfter = bonesDef[static_cast<size_t>(a)].IsAfterPhysics();
		const bool bAfter = bonesDef[static_cast<size_t>(b)].IsAfterPhysics();
		if (aAfter != bAfter) return aAfter < bAfter;

		const int depthA = static_cast<int>(ComputeDepth(bonesDef, a));
		const int depthB = static_cast<int>(ComputeDepth(bonesDef, b));
		if (depthA != depthB) return depthA < depthB;
		return a < b;
	});

	m_writeBackOrder = std::move(nodes);
	m_desiredGlobals.resize(bonesDef.size());
	m_appliedGlobals.resize(bonesDef.size());
	m_hasDesiredGlobal.assign(bonesDef.size(), 0);
	m_hasAppliedGlobal.assign(bonesDef.size(), 0);
	m_originalLocalTranslation.resize(bonesDef.size());
}

void MmdPhysicsWorld::CaptureAnimationBoneGlobals(
	const BoneSolver& bones,
	std::vector<XMFLOAT4X4>& outGlobals) const
{
	const size_t boneCount = bones.BoneCount();
	outGlobals.resize(boneCount);
	for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
	{
		outGlobals[boneIndex] = bones.GetBoneGlobalMatrix(boneIndex);
	}
}

void MmdPhysicsWorld::ApplyImpulseMorphs(const PmxModel& model, const std::vector<float>& morphWeights)
{
	if (!m_btWorld || m_btRigidBodies.size() != m_bodies.size())
	{
		m_prevImpulseMorphWeights.assign(model.Morphs().size(), 0.0f);
		return;
	}

	const auto& morphs = model.Morphs();
	if (m_prevImpulseMorphWeights.size() != morphs.size())
	{
		m_prevImpulseMorphWeights.assign(morphs.size(), 0.0f);
	}

	for (size_t morphIndex = 0; morphIndex < morphs.size(); ++morphIndex)
	{
		if (morphs[morphIndex].type != PmxModel::Morph::Type::Impulse)
		{
			continue;
		}

		const float currentWeight =
			(morphIndex < morphWeights.size()) ? morphWeights[morphIndex] : 0.0f;
		const float deltaWeight = currentWeight - m_prevImpulseMorphWeights[morphIndex];
		m_prevImpulseMorphWeights[morphIndex] = currentWeight;
		if (std::abs(deltaWeight) <= 1.0e-5f)
		{
			continue;
		}

		for (const auto& offset : morphs[morphIndex].impulseOffsets)
		{
			if (offset.rigidBodyIndex < 0)
			{
				continue;
			}

			const size_t rigidBodyIndex = static_cast<size_t>(offset.rigidBodyIndex);
			if (rigidBodyIndex >= m_btRigidBodies.size())
			{
				continue;
			}

			btRigidBody* rigidBody = m_btRigidBodies[rigidBodyIndex].get();
			if (!rigidBody || rigidBody->isStaticObject() || rigidBody->isKinematicObject())
			{
				continue;
			}

			btVector3 linearVelocity = ToBtVector3(offset.velocity);
			btVector3 angularVelocity = ToBtVector3(offset.torque);
			if (offset.localFlag != 0)
			{
				const btQuaternion rotation = rigidBody->getWorldTransform().getRotation();
				linearVelocity = quatRotate(rotation, linearVelocity);
				angularVelocity = quatRotate(rotation, angularVelocity);
			}

			rigidBody->activate(true);
			rigidBody->setLinearVelocity(
				rigidBody->getLinearVelocity() + linearVelocity * deltaWeight);
			rigidBody->setAngularVelocity(
				rigidBody->getAngularVelocity() + angularVelocity * deltaWeight);
		}
	}
}

void MmdPhysicsWorld::UpdateSoftBodyVertexOverrides(const PmxModel& model)
{
	const size_t vertexCount = model.Vertices().size();
	if (m_softBodyVertexPositions.size() != vertexCount)
	{
		m_softBodyVertexPositions.resize(vertexCount);
	}
	if (m_softBodyVertexMask.size() != vertexCount)
	{
		m_softBodyVertexMask.assign(vertexCount, 0);
	}
	else
	{
		for (const std::uint32_t vertexIndex : m_softBodyActiveVertexIndices)
		{
			const size_t index = static_cast<size_t>(vertexIndex);
			if (index >= vertexCount)
			{
				continue;
			}

			m_softBodyVertexPositions[index] = {};
			m_softBodyVertexMask[index] = 0;
		}
	}
	m_softBodyActiveVertexIndices.clear();
	m_softBodyBoundsMin = {};
	m_softBodyBoundsMax = {};
	m_hasSoftBodyVertexOverrides = false;

	if (m_softBodies.empty() || m_btSoftBodies.size() != m_softBodies.size())
	{
		return;
	}

	float minx = (std::numeric_limits<float>::max)();
	float miny = (std::numeric_limits<float>::max)();
	float minz = (std::numeric_limits<float>::max)();
	float maxx = (std::numeric_limits<float>::lowest)();
	float maxy = (std::numeric_limits<float>::lowest)();
	float maxz = (std::numeric_limits<float>::lowest)();

	for (size_t softBodyIndex = 0; softBodyIndex < m_softBodies.size(); ++softBodyIndex)
	{
		const auto& instance = m_softBodies[softBodyIndex];
		const btSoftBody* softBody = m_btSoftBodies[softBodyIndex].get();
		if (!softBody)
		{
			continue;
		}

		const int nodeCount = std::min(
			static_cast<int>(instance.nodeVertexIndices.size()),
			softBody->m_nodes.size());
		for (int nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
		{
			const int vertexIndex = instance.nodeVertexIndices[static_cast<size_t>(nodeIndex)];
			if (vertexIndex < 0 || vertexIndex >= static_cast<int>(m_softBodyVertexPositions.size()))
			{
				continue;
			}

			const size_t resolvedVertexIndex = static_cast<size_t>(vertexIndex);
			auto& outPosition = m_softBodyVertexPositions[resolvedVertexIndex];
			outPosition = ToXmFloat3(softBody->m_nodes[nodeIndex].m_x);
			if (m_softBodyVertexMask[resolvedVertexIndex] == 0)
			{
				m_softBodyVertexMask[resolvedVertexIndex] = 1;
				m_softBodyActiveVertexIndices.push_back(static_cast<std::uint32_t>(vertexIndex));
			}
			minx = std::min(minx, outPosition.x);
			miny = std::min(miny, outPosition.y);
			minz = std::min(minz, outPosition.z);
			maxx = std::max(maxx, outPosition.x);
			maxy = std::max(maxy, outPosition.y);
			maxz = std::max(maxz, outPosition.z);
			m_hasSoftBodyVertexOverrides = true;
		}
	}

	if (m_hasSoftBodyVertexOverrides)
	{
		m_softBodyBoundsMin = { minx, miny, minz };
		m_softBodyBoundsMax = { maxx, maxy, maxz };
	}
}

void MmdPhysicsWorld::Step(double dtSeconds,
						   const PmxModel& model,
						   BoneSolver& bones,
						   const std::vector<float>& morphWeights)
{
	if (!m_isBuilt || m_builtRevision != model.Revision())
	{
		BuildFromModel(model, bones);
	}
	if (m_bodies.empty() && m_btSoftBodies.empty()) return;

	ApplyImpulseMorphs(model, morphWeights);

	CaptureAnimationBoneGlobals(bones, m_currAnimationBoneGlobals);
	const float dt = static_cast<float>(std::max(0.0, dtSeconds));
	const std::vector<XMFLOAT4X4>& pendingStartGlobals =
		(m_hasPrevAnimationBoneGlobals &&
		 m_prevAnimationBoneGlobals.size() == m_currAnimationBoneGlobals.size())
		? m_prevAnimationBoneGlobals
		: m_currAnimationBoneGlobals;

	auto BuildInterpolatedGlobals =
		[&](const std::vector<XMFLOAT4X4>& fromGlobals,
			const std::vector<XMFLOAT4X4>& toGlobals,
			float alpha,
			std::vector<XMFLOAT4X4>& outGlobals)
	{
		const float t = std::clamp(alpha, 0.0f, 1.0f);
		if (t <= 1.0e-6f)
		{
			outGlobals = fromGlobals;
			return;
		}
		if (t >= 1.0f - 1.0e-6f)
		{
			outGlobals = toGlobals;
			return;
		}

		const size_t boneCount = std::min(fromGlobals.size(), toGlobals.size());
		outGlobals.resize(boneCount);
		for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
		{
			const XMMATRIX mA = XMLoadFloat4x4(&fromGlobals[boneIndex]);
			const XMMATRIX mB = XMLoadFloat4x4(&toGlobals[boneIndex]);

			XMFLOAT3 pA{}, pB{};
			XMFLOAT4 qA{}, qB{};
			DecomposeTR(mA, pA, qA);
			DecomposeTR(mB, pB, qB);

			const XMVECTOR tA = XMLoadFloat3(&pA);
			const XMVECTOR tB = XMLoadFloat3(&pB);
			const XMVECTOR blendedT = XMVectorLerp(tA, tB, t);

			XMVECTOR qa = XMQuaternionNormalize(XMLoadFloat4(&qA));
			XMVECTOR qb = XMQuaternionNormalize(XMLoadFloat4(&qB));
			const float dot = XMVectorGetX(XMVector4Dot(qa, qb));
			if (dot < 0.0f)
			{
				qb = XMVectorNegate(qb);
			}

			const XMVECTOR blendedQ = XMQuaternionNormalize(XMQuaternionSlerp(qa, qb, t));
			const XMMATRIX blendedM =
				XMMatrixRotationQuaternion(blendedQ) *
				XMMatrixTranslationFromVector(blendedT);

			XMStoreFloat4x4(&outGlobals[boneIndex], blendedM);
		}
	};

	const float fixedTimeStep = std::max(1.0e-4f, m_settings.fixedTimeStep);
	const int configuredMaxSteps = std::max(1, m_settings.maxSubSteps);
	const int effectiveMaxSteps =
		m_btSoftBodies.empty()
		? configuredMaxSteps
		: std::max(configuredMaxSteps, 4);

	if (dt > 0.0f)
	{
		const float accumulatorBeforeStep = m_stepAccumulatorSeconds;
		const float fullWindowSeconds = accumulatorBeforeStep + dt;
		const float maxRetainedWindow = fixedTimeStep * static_cast<float>(effectiveMaxSteps);
		m_stepAccumulatorSeconds = std::min(fullWindowSeconds, maxRetainedWindow);
		const float retainedWindowSeconds = m_stepAccumulatorSeconds;

		int plannedSteps = 0;
		float simulatedAccumulator = m_stepAccumulatorSeconds;
		while (simulatedAccumulator + 1.0e-7f >= fixedTimeStep &&
			   plannedSteps < effectiveMaxSteps)
		{
			simulatedAccumulator -= fixedTimeStep;
			++plannedSteps;
		}

		if (plannedSteps > 0)
		{
			std::vector<XMFLOAT4X4> retainedStartGlobals;
			if (fullWindowSeconds > 1.0e-7f && retainedWindowSeconds + 1.0e-7f < fullWindowSeconds)
			{
				const float discardAlpha =
					(fullWindowSeconds - retainedWindowSeconds) / fullWindowSeconds;
				BuildInterpolatedGlobals(
					pendingStartGlobals,
					m_currAnimationBoneGlobals,
					discardAlpha,
					retainedStartGlobals);
			}
			else
			{
				retainedStartGlobals = pendingStartGlobals;
			}

			std::vector<XMFLOAT4X4> stepStartGlobals;
			std::vector<XMFLOAT4X4> stepEndGlobals;
			for (int stepIndex = 0; stepIndex < plannedSteps; ++stepIndex)
			{
				const float stepBoundaryStart = static_cast<float>(stepIndex) * fixedTimeStep;
				const float stepBoundaryEnd = static_cast<float>(stepIndex + 1) * fixedTimeStep;
				const float alphaStart =
					(retainedWindowSeconds > 1.0e-7f) ? (stepBoundaryStart / retainedWindowSeconds) : 1.0f;
				const float alphaEnd =
					(retainedWindowSeconds > 1.0e-7f) ? (stepBoundaryEnd / retainedWindowSeconds) : 1.0f;
				BuildInterpolatedGlobals(
					retainedStartGlobals,
					m_currAnimationBoneGlobals,
					alphaStart,
					stepStartGlobals);
				BuildInterpolatedGlobals(
					retainedStartGlobals,
					m_currAnimationBoneGlobals,
					alphaEnd,
					stepEndGlobals);
				AdvanceSingleFixedStep(
					fixedTimeStep,
					model,
					stepStartGlobals,
					stepEndGlobals);
				m_stepAccumulatorSeconds -= fixedTimeStep;
			}

			const float simulatedSeconds = static_cast<float>(plannedSteps) * fixedTimeStep;
			const float consumedAlpha =
				(retainedWindowSeconds > 1.0e-7f) ? (simulatedSeconds / retainedWindowSeconds) : 1.0f;
			BuildInterpolatedGlobals(
				retainedStartGlobals,
				m_currAnimationBoneGlobals,
				consumedAlpha,
				m_prevAnimationBoneGlobals);
		}
		else if (retainedWindowSeconds <= 1.0e-7f)
		{
			m_prevAnimationBoneGlobals = m_currAnimationBoneGlobals;
		}
	}

	UpdateSoftBodyVertexOverrides(model);
	m_hasPrevAnimationBoneGlobals = true;
	if (!std::getenv("MMD_PHYSICS_DISABLE_WRITEBACK"))
	{
		WriteBackBones(model, bones);
	}
}

void MmdPhysicsWorld::AdvanceSingleFixedStep(
	float stepDt,
	const PmxModel& model,
	const std::vector<XMFLOAT4X4>& startBoneGlobals,
	const std::vector<XMFLOAT4X4>& targetBoneGlobals)
{
	PrecomputeKinematicTargets(model, startBoneGlobals, targetBoneGlobals);
	RunRealBulletFixedStep(stepDt);
}

void MmdPhysicsWorld::DestroyRealBulletWorld()
{
	if (m_btWorld)
	{
		if (m_btSoftWorld)
		{
			for (const auto& softBody : m_btSoftBodies)
			{
				if (softBody) m_btSoftWorld->removeSoftBody(softBody.get());
			}
		}
		for (const auto& c : m_btConstraints)
		{
			if (c) m_btWorld->removeConstraint(c.get());
		}
		for (const auto& rb : m_btRigidBodies)
		{
			if (rb) m_btWorld->removeRigidBody(rb.get());
		}
	}

	m_btConstraints.clear();
	m_btRigidBodies.clear();
	m_btMotionStates.clear();
	m_btShapes.clear();
	m_btSoftBodies.clear();
	m_btWorld.reset();
	m_btSoftWorld = nullptr;
	m_btSolver.reset();
	m_btBroadphase.reset();
	m_btDispatcher.reset();
	m_btCollisionConfig.reset();
}

void MmdPhysicsWorld::InitializeRealBulletWorld(const PmxModel& model)
{
	DestroyRealBulletWorld();

	if (m_bodies.empty() && model.SoftBodies().empty())
	{
		return;
	}

	const bool hasSoftBodies = !model.SoftBodies().empty();
	if (hasSoftBodies)
	{
		m_btCollisionConfig = std::make_unique<btSoftBodyRigidBodyCollisionConfiguration>();
	}
	else
	{
		m_btCollisionConfig = std::make_unique<btDefaultCollisionConfiguration>();
	}

	m_btDispatcher = std::make_unique<btCollisionDispatcher>(m_btCollisionConfig.get());
	m_btBroadphase = std::make_unique<btDbvtBroadphase>();
	m_btSolver = std::make_unique<btSequentialImpulseConstraintSolver>();
	if (hasSoftBodies)
	{
		auto softWorld = std::make_unique<btSoftRigidDynamicsWorld>(
			m_btDispatcher.get(),
			m_btBroadphase.get(),
			m_btSolver.get(),
			m_btCollisionConfig.get());
		btSoftBodyWorldInfo& worldInfo = softWorld->getWorldInfo();
		worldInfo.m_dispatcher = m_btDispatcher.get();
		worldInfo.m_broadphase = m_btBroadphase.get();
		worldInfo.m_gravity = btVector3(m_settings.gravity.x, m_settings.gravity.y, m_settings.gravity.z);
		worldInfo.m_sparsesdf.Initialize();
		m_btSoftWorld = softWorld.get();
		m_btWorld = std::move(softWorld);
	}
	else
	{
		m_btSoftWorld = nullptr;
		m_btWorld = std::make_unique<btDiscreteDynamicsWorld>(
			m_btDispatcher.get(),
			m_btBroadphase.get(),
			m_btSolver.get(),
			m_btCollisionConfig.get());
	}

	const XMFLOAT3 gravity = m_settings.gravity;
	m_btWorld->setGravity(btVector3(gravity.x, gravity.y, gravity.z));

	btContactSolverInfo& solverInfo = m_btWorld->getSolverInfo();
	solverInfo.m_splitImpulse = 1;
	solverInfo.m_splitImpulsePenetrationThreshold = -0.02f;
	solverInfo.m_numIterations = std::max(solverInfo.m_numIterations, 30);

	const auto& rbDefs = model.RigidBodies();
	const float collisionScale = 1.0f;

	m_btShapes.reserve(m_bodies.size());
	m_btMotionStates.reserve(m_bodies.size());
	m_btRigidBodies.reserve(m_bodies.size());

	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		const Body& b = m_bodies[i];
		const PmxModel::RigidBody* def = nullptr;
		if (b.defIndex >= 0 && static_cast<size_t>(b.defIndex) < rbDefs.size())
		{
			def = &rbDefs[static_cast<size_t>(b.defIndex)];
		}

		std::unique_ptr<btCollisionShape> shape;
		switch (b.shapeType)
		{
		case PmxModel::RigidBody::ShapeType::Sphere:
		{
			const float r = std::max(1.0e-4f, b.shapeSize.x * collisionScale);
			shape = std::make_unique<btSphereShape>(r);
			break;
		}
		case PmxModel::RigidBody::ShapeType::Box:
		{
			const float ex = std::max(1.0e-4f, b.shapeSize.x * collisionScale);
			const float ey = std::max(1.0e-4f, b.shapeSize.y * collisionScale);
			const float ez = std::max(1.0e-4f, b.shapeSize.z * collisionScale);
			shape = std::make_unique<btBoxShape>(btVector3(ex, ey, ez));
			break;
		}
		case PmxModel::RigidBody::ShapeType::Capsule:
		default:
		{
			const float r = std::max(1.0e-4f, b.shapeSize.x * collisionScale);
			const float h = std::max(0.0f, b.shapeSize.y * collisionScale);
			shape = std::make_unique<btCapsuleShape>(r, h);
			break;
		}
		}
		float mass = 0.0f;
		if (def && def->operation != PmxModel::RigidBody::OperationType::Static && b.invMass > 0.0f)
		{
			mass = 1.0f / b.invMass;
		}

		btVector3 localInertia(0.0f, 0.0f, 0.0f);
		if (mass > 0.0f)
		{
			shape->calculateLocalInertia(mass, localInertia);
		}

		btTransform transform = ToBtTransform(b.position, b.rotation);
		auto motionState = std::make_unique<btDefaultMotionState>(transform);

		btRigidBody::btRigidBodyConstructionInfo ci(mass, motionState.get(), shape.get(), localInertia);
		ci.m_friction = std::max(0.0f, def ? def->friction : b.friction);
		ci.m_restitution = std::max(0.0f, def ? def->restitution : b.restitution);
		auto rb = std::make_unique<btRigidBody>(ci);
		rb->setUserIndex(static_cast<int>(i));

		float linDamping = std::clamp(def ? def->linearDamping : b.linearDamping, 0.0f, 1.0f);
		float angDamping = std::clamp(def ? def->angularDamping : b.angularDamping, 0.0f, 1.0f);
		rb->setDamping(linDamping, angDamping);
		rb->setLinearVelocity(ToBtVector3(b.linearVelocity));
		rb->setAngularVelocity(ToBtVector3(b.angularVelocity));
		rb->setSleepingThresholds(
			std::max(0.0f, m_settings.sleepLinearThreshold),
			std::max(0.0f, m_settings.sleepAngularThreshold));

		const bool hasBone = (b.boneIndex >= 0 && b.boneIndex < static_cast<int>(model.Bones().size()));
		if (mass <= 0.0f && hasBone)
		{
			rb->setCollisionFlags(rb->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
			rb->setActivationState(DISABLE_DEACTIVATION);
		}
		else if (mass > 0.0f)
		{
			float ccdRadius = 0.0f;
			switch (b.shapeType)
			{
			case PmxModel::RigidBody::ShapeType::Sphere:
			case PmxModel::RigidBody::ShapeType::Capsule:
				ccdRadius = std::max(1.0e-4f, b.shapeSize.x * collisionScale * 0.5f);
				break;
			case PmxModel::RigidBody::ShapeType::Box:
			default:
			{
				const float minHalfExtent =
					std::min({ b.shapeSize.x, b.shapeSize.y, b.shapeSize.z }) * collisionScale;
				ccdRadius = std::max(1.0e-4f, minHalfExtent * 0.5f);
				break;
			}
			}

			rb->setCcdSweptSphereRadius(ccdRadius);
			rb->setCcdMotionThreshold(std::max(1.0e-4f, ccdRadius * m_settings.ccdThresholdScale));
		}

		const int groupIndex = std::clamp(b.group, 0, 15);
		const short group = static_cast<short>(ToCollisionGroupBit(groupIndex));
		short mask = static_cast<short>(ToBulletCollisionMask(b.groupMask, m_useDirectCollisionMaskSemantics));
		if (std::getenv("MMD_PHYSICS_DISABLE_RIGID_COLLISION"))
		{
			mask = 0;
		}

		m_btWorld->addRigidBody(rb.get(), group, mask);

		m_btShapes.push_back(std::move(shape));
		m_btMotionStates.push_back(std::move(motionState));
		m_btRigidBodies.push_back(std::move(rb));
	}

	const auto& joints = model.Joints();
	m_btConstraints.reserve(joints.size());
	for (const auto& j : joints)
	{
		if (j.rigidBodyA < 0 || j.rigidBodyB < 0) continue;
		if (j.rigidBodyA >= static_cast<int>(m_btRigidBodies.size())) continue;
		if (j.rigidBodyB >= static_cast<int>(m_btRigidBodies.size())) continue;

		btRigidBody* rbA = m_btRigidBodies[static_cast<size_t>(j.rigidBodyA)].get();
		btRigidBody* rbB = m_btRigidBodies[static_cast<size_t>(j.rigidBodyB)].get();
		if (!rbA || !rbB) continue;

		const Body& bodyA = m_bodies[static_cast<size_t>(j.rigidBodyA)];
		const Body& bodyB = m_bodies[static_cast<size_t>(j.rigidBodyB)];

		const XMMATRIX worldA = MatrixFromTR(bodyA.position, bodyA.rotation);
		const XMMATRIX worldB = MatrixFromTR(bodyB.position, bodyB.rotation);
		const XMMATRIX worldJ =
			MatrixRotationEulerXYZ(j.rotation.x, j.rotation.y, j.rotation.z) *
			XMMatrixTranslation(j.position.x, j.position.y, j.position.z);

		const XMMATRIX frameA = worldJ * XMMatrixInverse(nullptr, worldA);
		const XMMATRIX frameB = worldJ * XMMatrixInverse(nullptr, worldB);

		XMFLOAT3 frameAPos{}, frameBPos{};
		XMFLOAT4 frameARot{}, frameBRot{};
		DecomposeTR(frameA, frameAPos, frameARot);
		DecomposeTR(frameB, frameBPos, frameBRot);

		btTransform btFrameA = ToBtTransform(frameAPos, frameARot);
		btTransform btFrameB = ToBtTransform(frameBPos, frameBRot);

		auto constraint = std::make_unique<btGeneric6DofSpringConstraint>(*rbA, *rbB, btFrameA, btFrameB, true);
		constraint->setLinearLowerLimit(ToBtVector3(j.positionLower));
		constraint->setLinearUpperLimit(ToBtVector3(j.positionUpper));
		constraint->setAngularLowerLimit(ToBtVector3(j.rotationLower));
		constraint->setAngularUpperLimit(ToBtVector3(j.rotationUpper));
		for (int axis = 0; axis < 6; ++axis)
		{
			constraint->setParam(BT_CONSTRAINT_STOP_ERP, m_settings.jointStopErp, axis);
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			const float k = std::abs((&j.springPosition.x)[axis]);
			if (k > kEps)
			{
				constraint->enableSpring(axis, true);
				constraint->setStiffness(axis, k);
			}
		}
		for (int axis = 0; axis < 3; ++axis)
		{
			const float k = std::abs((&j.springRotation.x)[axis]);
			if (k > kEps)
			{
				constraint->enableSpring(axis + 3, true);
				constraint->setStiffness(axis + 3, k);
			}
		}

		m_btWorld->addConstraint(constraint.get(), true);
		m_btConstraints.push_back(std::move(constraint));
	}

	// Suppress collisions between a static anchor and nearby box descendants in the joint graph.
	// This targets skirt-like box chains while avoiding side effects on hair/chest capsule-sphere rigs.
	if (!m_joints.empty() && !m_btRigidBodies.empty())
	{
		const size_t bodyCount = m_bodies.size();
		std::vector<std::vector<int>> jointNeighbors(bodyCount);
		jointNeighbors.reserve(bodyCount);
		for (const JointConstraint& c : m_joints)
		{
			if (c.bodyA < 0 || c.bodyB < 0) continue;
			if (c.bodyA >= static_cast<int>(bodyCount) || c.bodyB >= static_cast<int>(bodyCount)) continue;
			jointNeighbors[static_cast<size_t>(c.bodyA)].push_back(c.bodyB);
			jointNeighbors[static_cast<size_t>(c.bodyB)].push_back(c.bodyA);
		}

		auto pairKey = [](int a, int b) -> uint64_t
		{
			const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
			const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
			return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
		};

		std::unordered_set<uint64_t> ignoredPairs;
		uint32_t suppressedPairCount = 0;

		static constexpr int kStaticAnchorSuppressDepth = 4;
		for (size_t staticIndex = 0; staticIndex < bodyCount; ++staticIndex)
		{
			if (m_bodies[staticIndex].invMass > 0.0f) continue;
			btRigidBody* staticBody = m_btRigidBodies[staticIndex].get();
			if (!staticBody) continue;

			std::vector<int> bestDepth(bodyCount, std::numeric_limits<int>::max());
			std::queue<int> queue;
			bestDepth[staticIndex] = 0;
			queue.push(static_cast<int>(staticIndex));
			while (!queue.empty())
			{
				const int current = queue.front();
				queue.pop();
				const int currentDepth = bestDepth[static_cast<size_t>(current)];
				if (currentDepth >= kStaticAnchorSuppressDepth) continue;

				for (int neighbor : jointNeighbors[static_cast<size_t>(current)])
				{
					if (neighbor < 0 || neighbor >= static_cast<int>(bodyCount)) continue;
					const int nextDepth = currentDepth + 1;
					if (nextDepth >= bestDepth[static_cast<size_t>(neighbor)]) continue;

					bestDepth[static_cast<size_t>(neighbor)] = nextDepth;
					if (nextDepth < kStaticAnchorSuppressDepth)
					{
						queue.push(neighbor);
					}

					if (nextDepth < 2) continue;
					if (m_bodies[static_cast<size_t>(neighbor)].invMass <= 0.0f) continue;
					if (m_bodies[static_cast<size_t>(neighbor)].shapeType != PmxModel::RigidBody::ShapeType::Box) continue;

					const uint64_t key = pairKey(static_cast<int>(staticIndex), neighbor);
					if (!ignoredPairs.insert(key).second) continue;

					btRigidBody* dynamicBody = m_btRigidBodies[static_cast<size_t>(neighbor)].get();
					if (!dynamicBody) continue;

					staticBody->setIgnoreCollisionCheck(dynamicBody, true);
					dynamicBody->setIgnoreCollisionCheck(staticBody, true);
					++suppressedPairCount;
				}
			}
		}

		if (suppressedPairCount > 0u)
		{
			std::ostringstream oss;
			oss << "CollisionSuppression staticToDescendants=" << suppressedPairCount;
			mmd::physics::debuglog::AppendLine(oss.str());
		}
	}

	if (m_btSoftWorld)
	{
		const auto& softBodyDefs = model.SoftBodies();
		const auto& materials = model.Materials();
		const auto& vertices = model.Vertices();
		const auto& indices = model.Indices();

		m_btSoftBodies.reserve(softBodyDefs.size());
		m_softBodies.reserve(softBodyDefs.size());

			for (size_t softBodyIndex = 0; softBodyIndex < softBodyDefs.size(); ++softBodyIndex)
			{
				const auto& definition = softBodyDefs[softBodyIndex];
				const bool isTriMeshShape = (definition.shape == kSupportedSoftBodyShapeTriMesh);
				const bool isRopeShape = (definition.shape == kSupportedSoftBodyShapeRope);
				if (!isTriMeshShape && !isRopeShape)
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=unsupportedShape"
						<< " shape=" << static_cast<unsigned>(definition.shape)
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " flags=0x" << std::hex << static_cast<unsigned>(definition.flags) << std::dec
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
			}

			if (definition.materialIndex < 0 ||
				definition.materialIndex >= static_cast<int32_t>(materials.size()))
			{
				std::ostringstream oss;
				oss << "SoftBodySkip index=" << softBodyIndex
					<< " reason=invalidMaterial"
					<< " materialIndex=" << definition.materialIndex
					<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
				mmd::physics::debuglog::AppendLine(oss.str());
				continue;
				}

				const auto& material = materials[static_cast<size_t>(definition.materialIndex)];
				const int minimumIndexCount = isTriMeshShape ? 3 : 2;
				if (material.indexCount < minimumIndexCount)
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=materialHasInsufficientIndices"
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " requiredIndexCount=" << minimumIndexCount
						<< " indexCount=" << material.indexCount
						<< " materialIndex=" << definition.materialIndex
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
			}

			if (material.indexOffset >= indices.size())
			{
				std::ostringstream oss;
				oss << "SoftBodySkip index=" << softBodyIndex
					<< " reason=materialIndexOffsetOutOfRange"
					<< " indexOffset=" << material.indexOffset
					<< " indexCount=" << material.indexCount
					<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
				mmd::physics::debuglog::AppendLine(oss.str());
				continue;
			}

			std::unordered_map<uint32_t, int> vertexRemap;
			std::vector<btScalar> nodePositions;
			std::vector<int> triangleIndices;
			std::vector<int> nodeVertexIndices;
			vertexRemap.reserve(static_cast<size_t>(material.indexCount));
			nodePositions.reserve(static_cast<size_t>(material.indexCount));
			triangleIndices.reserve(static_cast<size_t>(material.indexCount));
			nodeVertexIndices.reserve(static_cast<size_t>(material.indexCount));

				const size_t begin = static_cast<size_t>(material.indexOffset);
				const size_t availableIndexCount = std::min(
					indices.size() - begin,
					static_cast<size_t>(material.indexCount));
				const size_t usableIndexCount = isTriMeshShape
					? (availableIndexCount - (availableIndexCount % 3))
					: availableIndexCount;
				if (usableIndexCount < static_cast<size_t>(minimumIndexCount))
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=noUsableIndices"
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " availableIndexCount=" << availableIndexCount
						<< " indexCount=" << material.indexCount
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
				}

				const size_t end = begin + usableIndexCount;
				for (size_t indexCursor = begin; indexCursor < end; ++indexCursor)
				{
					const uint32_t vertexIndex = indices[indexCursor];
					if (vertexIndex >= vertices.size())
				{
					continue;
				}

				const auto [it, inserted] = vertexRemap.emplace(vertexIndex, static_cast<int>(nodeVertexIndices.size()));
				if (inserted)
				{
					const auto& vertex = vertices[vertexIndex];
					nodePositions.push_back(vertex.px);
						nodePositions.push_back(vertex.py);
						nodePositions.push_back(vertex.pz);
						nodeVertexIndices.push_back(static_cast<int>(vertexIndex));
					}

					if (isTriMeshShape)
					{
						triangleIndices.push_back(it->second);
					}
				}

				if (isTriMeshShape && ((triangleIndices.size() / 3) == 0 || nodeVertexIndices.empty()))
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=noValidVertex"
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
				}
				if (isRopeShape && nodeVertexIndices.size() < 2)
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=noValidRopeVertex"
						<< " nodeCount=" << nodeVertexIndices.size()
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
				}

				std::unique_ptr<btSoftBody> softBody;
				if (isTriMeshShape)
				{
					softBody = std::unique_ptr<btSoftBody>(btSoftBodyHelpers::CreateFromTriMesh(
						m_btSoftWorld->getWorldInfo(),
						nodePositions.data(),
						triangleIndices.data(),
						static_cast<int>(triangleIndices.size() / 3)));
				}
				else
				{
					float minX = std::numeric_limits<float>::max();
					float minY = std::numeric_limits<float>::max();
					float minZ = std::numeric_limits<float>::max();
					float maxX = std::numeric_limits<float>::lowest();
					float maxY = std::numeric_limits<float>::lowest();
					float maxZ = std::numeric_limits<float>::lowest();
					for (int vertexIndex : nodeVertexIndices)
					{
						const auto& vertex = vertices[static_cast<size_t>(vertexIndex)];
						minX = std::min(minX, vertex.px);
						minY = std::min(minY, vertex.py);
						minZ = std::min(minZ, vertex.pz);
						maxX = std::max(maxX, vertex.px);
						maxY = std::max(maxY, vertex.py);
						maxZ = std::max(maxZ, vertex.pz);
					}

					int sortAxis = 0;
					const float extentX = maxX - minX;
					const float extentY = maxY - minY;
					const float extentZ = maxZ - minZ;
					if (extentY > extentX && extentY >= extentZ)
					{
						sortAxis = 1;
					}
					else if (extentZ > extentX && extentZ > extentY)
					{
						sortAxis = 2;
					}

					auto axisValue = [&](int vertexIndex) -> float
					{
						const auto& vertex = vertices[static_cast<size_t>(vertexIndex)];
						switch (sortAxis)
						{
							case 1: return vertex.py;
							case 2: return vertex.pz;
							default: return vertex.px;
						}
					};
					std::sort(nodeVertexIndices.begin(), nodeVertexIndices.end(),
						[&](int lhs, int rhs)
						{
							const float lhsValue = axisValue(lhs);
							const float rhsValue = axisValue(rhs);
							if (lhsValue == rhsValue)
							{
								return lhs < rhs;
							}
							return lhsValue < rhsValue;
						});

					const auto& startVertex = vertices[static_cast<size_t>(nodeVertexIndices.front())];
					const auto& endVertex = vertices[static_cast<size_t>(nodeVertexIndices.back())];
					const int ropeResolution = std::max(0, static_cast<int>(nodeVertexIndices.size()) - 2);
					softBody = std::unique_ptr<btSoftBody>(btSoftBodyHelpers::CreateRope(
						m_btSoftWorld->getWorldInfo(),
						btVector3(startVertex.px, startVertex.py, startVertex.pz),
						btVector3(endVertex.px, endVertex.py, endVertex.pz),
						ropeResolution,
						0));

					if (softBody)
					{
						const int nodeCount = std::min(
							static_cast<int>(nodeVertexIndices.size()),
							softBody->m_nodes.size());
						for (int nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
						{
							const auto& vertex = vertices[static_cast<size_t>(nodeVertexIndices[static_cast<size_t>(nodeIndex)])];
							const btVector3 nodePosition(vertex.px, vertex.py, vertex.pz);
							softBody->m_nodes[nodeIndex].m_x = nodePosition;
							softBody->m_nodes[nodeIndex].m_q = nodePosition;
						}

						if (nodeCount < static_cast<int>(nodeVertexIndices.size()))
						{
							nodeVertexIndices.resize(static_cast<size_t>(nodeCount));
						}

						vertexRemap.clear();
						vertexRemap.reserve(nodeVertexIndices.size());
						for (int nodeIndex = 0; nodeIndex < static_cast<int>(nodeVertexIndices.size()); ++nodeIndex)
						{
							vertexRemap[static_cast<uint32_t>(nodeVertexIndices[static_cast<size_t>(nodeIndex)])] = nodeIndex;
						}
					}
				}
				if (!softBody)
				{
					std::ostringstream oss;
					oss << "SoftBodySkip index=" << softBodyIndex
						<< " reason=createFailed"
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " triangleCount=" << (triangleIndices.size() / 3)
						<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
					mmd::physics::debuglog::AppendLine(oss.str());
					continue;
				}

			softBody->m_cfg.aeromodel = static_cast<btSoftBody::eAeroModel::_>(
				std::clamp(definition.aeroModel, 0, static_cast<int>(btSoftBody::eAeroModel::END) - 1));
			softBody->m_cfg.kVCF = definition.config.velocityCorrectionFactor;
			softBody->m_cfg.kDP = definition.config.dampingCoefficient;
			softBody->m_cfg.kDG = definition.config.dragCoefficient;
			softBody->m_cfg.kLF = definition.config.liftCoefficient;
			softBody->m_cfg.kPR = definition.config.pressureCoefficient;
			softBody->m_cfg.kVC = definition.config.volumeConversationCoefficient;
			softBody->m_cfg.kDF = definition.config.dynamicFrictionCoefficient;
			softBody->m_cfg.kMT = definition.config.poseMatchingCoefficient;
			softBody->m_cfg.kCHR = definition.config.rigidContactHardness;
			softBody->m_cfg.kKHR = definition.config.kineticContactHardness;
			softBody->m_cfg.kSHR = definition.config.softContactHardness;
			softBody->m_cfg.kAHR = definition.config.anchorHardness;
			softBody->m_cfg.kSRHR_CL = definition.cluster.softVsRigidHardness;
			softBody->m_cfg.kSKHR_CL = definition.cluster.softVsKineticHardness;
			softBody->m_cfg.kSSHR_CL = definition.cluster.softVsSoftHardness;
			softBody->m_cfg.kSR_SPLT_CL = definition.cluster.softVsRigidImpulseSplit;
			softBody->m_cfg.kSK_SPLT_CL = definition.cluster.softVsKineticImpulseSplit;
			softBody->m_cfg.kSS_SPLT_CL = definition.cluster.softVsSoftImpulseSplit;
			softBody->m_cfg.viterations = std::max(0, definition.iteration.velocityIterations);
			softBody->m_cfg.piterations = std::max(1, definition.iteration.positionIterations);
			softBody->m_cfg.diterations = std::max(0, definition.iteration.driftIterations);
			softBody->m_cfg.citerations = std::max(0, definition.iteration.clusterIterations);
			const bool enableBendingLinks =
				HasSoftBodyFlag(definition.flags, kSoftBodyFlagBendingLinks) &&
				definition.bendingLinkDistance > 0;
			const bool enableClusters =
				HasSoftBodyFlag(definition.flags, kSoftBodyFlagClusters) &&
				definition.clusterCount > 0;
			const bool enableSelfCollision =
				HasSoftBodyFlag(definition.flags, kSoftBodyFlagSelfCollision);

			int collisionFlags =
				btSoftBody::fCollision::SDF_RD |
				btSoftBody::fCollision::SDF_RDN |
				btSoftBody::fCollision::SDF_RDF;

			if (enableClusters && (softBodyDefs.size() > 1 || enableSelfCollision))
			{
				collisionFlags |= btSoftBody::fCollision::CL_SS;
				if (enableSelfCollision)
				{
					collisionFlags |= btSoftBody::fCollision::CL_SELF;
				}
			}
			else if (softBodyDefs.size() > 1)
			{
				collisionFlags |= btSoftBody::fCollision::VF_SS;
			}

			softBody->m_cfg.collisions = collisionFlags;

			if (softBody->m_materials.size() > 0)
			{
				softBody->m_materials[0]->m_kLST = definition.material.linearStiffnessCoefficient;
				softBody->m_materials[0]->m_kAST = definition.material.areaAngularStiffnessCoefficient;
				softBody->m_materials[0]->m_kVST = definition.material.volumeStiffnessCoefficient;
			}

			if (enableBendingLinks)
			{
				softBody->generateBendingConstraints(definition.bendingLinkDistance);
			}
			if (enableClusters)
			{
				softBody->generateClusters(definition.clusterCount);
			}

			if (definition.totalMass > 0.0f)
			{
				softBody->setTotalMass(definition.totalMass, true);
			}
			if (definition.collisionMargin > 0.0f)
			{
				softBody->getCollisionShape()->setMargin(definition.collisionMargin);
			}

			for (uint32_t pinnedVertexIndex : definition.pinVertexIndices)
			{
				const auto pinIt = vertexRemap.find(pinnedVertexIndex);
				if (pinIt != vertexRemap.end())
				{
					softBody->setMass(pinIt->second, 0.0f);
				}
			}

			for (const auto& anchor : definition.anchors)
			{
				if (anchor.rigidBodyIndex < 0)
				{
					continue;
				}

				const auto nodeIt = vertexRemap.find(anchor.vertexIndex);
				if (nodeIt == vertexRemap.end())
				{
					continue;
				}

				const size_t rigidBodyIndex = static_cast<size_t>(anchor.rigidBodyIndex);
				if (rigidBodyIndex >= m_btRigidBodies.size() || !m_btRigidBodies[rigidBodyIndex])
				{
					continue;
				}

				softBody->appendAnchor(
					nodeIt->second,
					m_btRigidBodies[rigidBodyIndex].get(),
					false);
			}

			softBody->setActivationState(DISABLE_DEACTIVATION);
			const int softGroupIndex = std::clamp(static_cast<int>(definition.groupIndex), 0, 15);
			const int softGroup = static_cast<int>(ToCollisionGroupBit(softGroupIndex));
			const int softMask = static_cast<int>(ToBulletCollisionMask(
				definition.collisionGroupMask,
				m_useDirectCollisionMaskSemantics));
			m_btSoftWorld->addSoftBody(softBody.get(), softGroup, softMask);
				{
					std::ostringstream oss;
					oss << "SoftBodyBuilt index=" << softBodyIndex
						<< " shape=" << static_cast<unsigned>(definition.shape)
						<< " shapeName=" << SoftBodyShapeToString(definition.shape)
						<< " nodes=" << nodeVertexIndices.size()
						<< " triangles=" << (triangleIndices.size() / 3)
						<< " segments=" << (nodeVertexIndices.size() > 0 ? (nodeVertexIndices.size() - 1) : 0)
						<< " materialIndex=" << definition.materialIndex
						<< " group=" << softGroupIndex
						<< " mask=0x" << std::hex << static_cast<unsigned>(definition.collisionGroupMask) << std::dec
					<< " flags=0x" << std::hex << static_cast<unsigned>(definition.flags) << std::dec
					<< " collisions=0x" << std::hex << static_cast<unsigned>(collisionFlags) << std::dec
					<< " name=\"" << mmd::physics::debuglog::ToUtf8Lossy(definition.name) << "\"";
				mmd::physics::debuglog::AppendLine(oss.str());
			}

			SoftBodyInstance instance{};
			instance.defIndex = static_cast<int>(softBodyIndex);
			instance.nodeVertexIndices = std::move(nodeVertexIndices);
			m_softBodies.push_back(std::move(instance));
			m_btSoftBodies.push_back(std::move(softBody));
		}
	}

	UpdateSoftBodyVertexOverrides(model);
}

void MmdPhysicsWorld::RunRealBulletFixedStep(float fixedStepDt)
{
	if (!m_btWorld || m_btRigidBodies.size() != m_bodies.size())
	{
		return;
	}

	const float dt = std::max(0.0f, fixedStepDt);
	uint32_t movedKinematicCount = 0;
	float maxKinematicPosDelta = 0.0f;
	int maxKinematicBody = -1;

	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		Body& b = m_bodies[i];
		btRigidBody* rb = m_btRigidBodies[i].get();
		if (!rb) continue;

		if (b.invMass <= 0.0f && b.boneIndex >= 0)
		{
			const float kdx = b.kinematicTargetPos.x - b.kinematicStartPos.x;
			const float kdy = b.kinematicTargetPos.y - b.kinematicStartPos.y;
			const float kdz = b.kinematicTargetPos.z - b.kinematicStartPos.z;
			const float kinematicPosDelta = std::sqrt(std::max(0.0f, kdx * kdx + kdy * kdy + kdz * kdz));
			const bool movedThisStep = HasMeaningfulTransformDelta(
				b.kinematicStartPos,
				b.kinematicStartRot,
				b.kinematicTargetPos,
				b.kinematicTargetRot,
				m_settings.kinematicPositionThreshold,
				m_settings.kinematicRotationThreshold);
			if (movedThisStep)
			{
				++movedKinematicCount;
				if (kinematicPosDelta > maxKinematicPosDelta)
				{
					maxKinematicPosDelta = kinematicPosDelta;
					maxKinematicBody = static_cast<int>(i);
				}
			}
			else
			{
				b.kinematicTargetPos = b.kinematicStartPos;
				b.kinematicTargetRot = b.kinematicStartRot;
			}
			const btTransform prevT = ToBtTransform(b.kinematicStartPos, b.kinematicStartRot);
			const btTransform targetT = ToBtTransform(b.kinematicTargetPos, b.kinematicTargetRot);
			rb->setInterpolationWorldTransform(prevT);
			rb->setWorldTransform(targetT);
			if (rb->getMotionState())
			{
				rb->getMotionState()->setWorldTransform(targetT);
			}
			rb->clearForces();
			if (movedThisStep)
			{
				rb->activate(true);
			}
			// Keep animated collision bodies active so Bullet keeps deriving their kinematic
			// velocity from the interpolation transform instead of letting them fall asleep.
			rb->setActivationState(DISABLE_DEACTIVATION);

			b.position = b.kinematicTargetPos;
			b.rotation = b.kinematicTargetRot;
		}
	}

	// DynamicAndPositionAdjust bodies need to follow the animated bone translation each
	// sub-step while keeping the simulated orientation. Without this, roots on hair/chest
	// chains drift away from their animated anchor and the downstream constraints explode.
	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		Body& b = m_bodies[i];
		btRigidBody* rb = m_btRigidBodies[i].get();
		if (!rb) continue;
		if (b.invMass <= 0.0f) continue;
		if (b.operation != PmxModel::RigidBody::OperationType::DynamicAndPositionAdjust) continue;
		if (b.boneIndex < 0) continue;

		const bool movedThisStep = HasMeaningfulPositionDelta(
			b.kinematicStartPos,
			b.kinematicTargetPos,
			m_settings.kinematicPositionThreshold);

		btTransform currentT = rb->getWorldTransform();
		btTransform prevT = currentT;
		btTransform targetT = currentT;
		prevT.setOrigin(ToBtVector3(b.kinematicStartPos));
		targetT.setOrigin(ToBtVector3(b.kinematicTargetPos));

		rb->setInterpolationWorldTransform(prevT);
		rb->setWorldTransform(targetT);
		if (rb->getMotionState())
		{
			rb->getMotionState()->setWorldTransform(targetT);
		}

		if (dt > kEps)
		{
			const XMFLOAT3 delta = {
				b.kinematicTargetPos.x - b.kinematicStartPos.x,
				b.kinematicTargetPos.y - b.kinematicStartPos.y,
				b.kinematicTargetPos.z - b.kinematicStartPos.z
			};
			btVector3 vel(delta.x / dt, delta.y / dt, delta.z / dt);
			if (vel.length() < m_settings.minKinematicVelocityClip)
			{
				vel.setValue(0.0f, 0.0f, 0.0f);
			}
			rb->setLinearVelocity(vel);
		}
		else
		{
			rb->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
		}

		if (movedThisStep)
		{
			rb->activate(true);
		}

		b.position = b.kinematicTargetPos;
	}

	if (dt > 0.0f)
	{
		const float fixedTimeStep = std::max(1.0e-4f, m_settings.fixedTimeStep);
		m_btWorld->stepSimulation(dt, 1, fixedTimeStep);
	}

	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		Body& b = m_bodies[i];
		btRigidBody* rb = m_btRigidBodies[i].get();
		if (!rb) continue;

		b.prevPosition = b.position;
		b.prevRotation = b.rotation;

		btTransform t;
		if (rb->getMotionState())
		{
			rb->getMotionState()->getWorldTransform(t);
		}
		else
		{
			t = rb->getWorldTransform();
		}

		btQuaternion q = t.getRotation();
		q.normalize();

		b.position = ToXmFloat3(t.getOrigin());
		b.rotation = ToXmFloat4(q);
		b.linearVelocity = ToXmFloat3(rb->getLinearVelocity());
		b.angularVelocity = ToXmFloat3(rb->getAngularVelocity());
		b.hadContactThisStep = false;
	}

	if (mmd::physics::debuglog::IsEnabled())
	{
		static uint64_t s_realBulletStepCounter = 0;
		++s_realBulletStepCounter;

		if (s_realBulletStepCounter <= 180u || (s_realBulletStepCounter % 60u) == 0u)
		{
			double posErrSum = 0.0;
			float posErrMax = 0.0f;
			int posErrMaxJoint = -1;
			uint32_t jointCount = 0;

			for (size_t ji = 0; ji < m_joints.size(); ++ji)
			{
				const JointConstraint& c = m_joints[ji];
				if (c.bodyA < 0 || c.bodyB < 0) continue;
				if (c.bodyA >= static_cast<int>(m_bodies.size()) || c.bodyB >= static_cast<int>(m_bodies.size())) continue;

				const Body& A = m_bodies[static_cast<size_t>(c.bodyA)];
				const Body& B = m_bodies[static_cast<size_t>(c.bodyB)];

				const XMVECTOR pA = Load3(A.position);
				const XMVECTOR pB = Load3(B.position);
				const XMVECTOR qA = Load4(A.rotation);
				const XMVECTOR qB = Load4(B.rotation);
				const XMVECTOR rA = RotateVector(Load3(c.localAnchorA), qA);
				const XMVECTOR rB = RotateVector(Load3(c.localAnchorB), qB);
				const XMVECTOR wA = XMVectorAdd(pA, rA);
				const XMVECTOR wB = XMVectorAdd(pB, rB);
				const float err = Length3(XMVectorSubtract(wA, wB));
				if (!std::isfinite(err)) continue;

				posErrSum += static_cast<double>(err);
				if (err > posErrMax)
				{
					posErrMax = err;
					posErrMaxJoint = static_cast<int>(ji);
				}
				++jointCount;
			}

			float maxLinSpeed = 0.0f;
			float maxAngSpeed = 0.0f;
			int maxLinBody = -1;
			int maxAngBody = -1;
			for (size_t i = 0; i < m_bodies.size(); ++i)
			{
				const Body& b = m_bodies[i];
				if (b.invMass <= 0.0f) continue;
				const float v = Length3(Load3(b.linearVelocity));
				if (std::isfinite(v) && v > maxLinSpeed)
				{
					maxLinSpeed = v;
					maxLinBody = static_cast<int>(i);
				}
				const float w = Length3(Load3(b.angularVelocity));
				if (std::isfinite(w) && w > maxAngSpeed)
				{
					maxAngSpeed = w;
					maxAngBody = static_cast<int>(i);
				}
			}

			const bool logMaxAngContacts = (std::getenv("MMD_PHYSICS_LOG_MAXANG_CONTACTS") != nullptr);
			uint32_t maxAngContactPoints = 0;
			int maxAngTopPartner = -1;
			uint32_t maxAngTopPartnerContacts = 0;
			float maxAngTopPartnerMaxPen = 0.0f;
			if (logMaxAngContacts && maxAngBody >= 0 && m_btDispatcher)
			{
				std::vector<uint32_t> partnerContactCounts(m_bodies.size(), 0u);
				std::vector<float> partnerMaxPenetration(m_bodies.size(), 0.0f);

				const int manifoldCount = m_btDispatcher->getNumManifolds();
				for (int manifoldIndex = 0; manifoldIndex < manifoldCount; ++manifoldIndex)
				{
					btPersistentManifold* manifold = m_btDispatcher->getManifoldByIndexInternal(manifoldIndex);
					if (!manifold) continue;

					const btCollisionObject* body0 = manifold->getBody0();
					const btCollisionObject* body1 = manifold->getBody1();
					if (!body0 || !body1) continue;

					const int index0 = body0->getUserIndex();
					const int index1 = body1->getUserIndex();
					if (index0 < 0 || index1 < 0) continue;
					if (index0 >= static_cast<int>(m_bodies.size()) ||
						index1 >= static_cast<int>(m_bodies.size()))
					{
						continue;
					}

					int partnerIndex = -1;
					if (index0 == maxAngBody) partnerIndex = index1;
					else if (index1 == maxAngBody) partnerIndex = index0;
					else continue;

					const int pointCount = manifold->getNumContacts();
					for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex)
					{
						const btManifoldPoint& point = manifold->getContactPoint(pointIndex);
						if (point.getDistance() >= 0.0f) continue;

						++maxAngContactPoints;
						uint32_t& partnerCount = partnerContactCounts[static_cast<size_t>(partnerIndex)];
						++partnerCount;
						const float penetration = -point.getDistance();
						float& partnerPen = partnerMaxPenetration[static_cast<size_t>(partnerIndex)];
						if (penetration > partnerPen)
						{
							partnerPen = penetration;
						}
					}
				}

				for (size_t partnerIndex = 0; partnerIndex < partnerContactCounts.size(); ++partnerIndex)
				{
					const uint32_t count = partnerContactCounts[partnerIndex];
					if (count == 0u) continue;
					if (count > maxAngTopPartnerContacts)
					{
						maxAngTopPartner = static_cast<int>(partnerIndex);
						maxAngTopPartnerContacts = count;
						maxAngTopPartnerMaxPen = partnerMaxPenetration[partnerIndex];
					}
				}
			}

			std::ostringstream oss;
			oss << "RealBulletStep #" << s_realBulletStepCounter
				<< " jointErrAvg=" << (jointCount > 0 ? (posErrSum / static_cast<double>(jointCount)) : 0.0)
				<< " jointErrMax=" << posErrMax
				<< " jointErrMaxJoint=" << posErrMaxJoint
				<< " movedKinematic=" << movedKinematicCount
				<< " maxKinPosDelta=" << maxKinematicPosDelta
				<< " maxKinBody=" << maxKinematicBody
				<< " maxLinSpeed=" << maxLinSpeed
				<< " maxLinBody=" << maxLinBody
				<< " maxAngSpeed=" << maxAngSpeed
				<< " maxAngBody=" << maxAngBody;
			if (logMaxAngContacts)
			{
				oss << " maxAngContactPoints=" << maxAngContactPoints
					<< " maxAngTopPartner=" << maxAngTopPartner
					<< " maxAngTopPartnerContacts=" << maxAngTopPartnerContacts
					<< " maxAngTopPartnerMaxPen=" << maxAngTopPartnerMaxPen;
			}
			mmd::physics::debuglog::AppendLine(oss.str());
		}
	}
}

void MmdPhysicsWorld::PrecomputeKinematicTargets(
	const PmxModel& model,
	const std::vector<XMFLOAT4X4>& startBoneGlobals,
	const std::vector<XMFLOAT4X4>& targetBoneGlobals)
{
	const auto& bonesDef = model.Bones();

	for (size_t i = 0; i < m_bodies.size(); ++i)
	{
		Body& b = m_bodies[i];

		const bool isKinematicBody = (b.invMass <= 0.0f);
		const bool isDynamicPositionAdjust =
			(b.invMass > 0.0f &&
			 b.operation == PmxModel::RigidBody::OperationType::DynamicAndPositionAdjust);
		if (!isKinematicBody && !isDynamicPositionAdjust) continue;

		const int boneIndex = b.boneIndex;
		if (boneIndex < 0 || boneIndex >= static_cast<int>(bonesDef.size())) continue;
		if (boneIndex >= static_cast<int>(startBoneGlobals.size())) continue;
		if (boneIndex >= static_cast<int>(targetBoneGlobals.size())) continue;

		DirectX::XMMATRIX localFromBone = DirectX::XMLoadFloat4x4(&b.localFromBone);
		const XMMATRIX boneStartG = XMLoadFloat4x4(&startBoneGlobals[static_cast<size_t>(boneIndex)]);
		const XMMATRIX boneTargetG = XMLoadFloat4x4(&targetBoneGlobals[static_cast<size_t>(boneIndex)]);
		const XMMATRIX rbStartG = localFromBone * boneStartG;
		const XMMATRIX rbTargetG = localFromBone * boneTargetG;

		DecomposeTR(rbStartG, b.kinematicStartPos, b.kinematicStartRot);
		DecomposeTR(rbTargetG, b.kinematicTargetPos, b.kinematicTargetRot);
	}
}

void MmdPhysicsWorld::WriteBackBones(const PmxModel& model, BoneSolver& bones)
{
	const auto& rbDefs = model.RigidBodies();
	const auto& bonesDef = model.Bones();

	if (m_writeBackOrder.empty())
	{
		return;
	}

	const size_t boneCount = bonesDef.size();
	if (m_desiredGlobals.size() != boneCount)
	{
		m_desiredGlobals.resize(boneCount);
		m_appliedGlobals.resize(boneCount);
		m_hasDesiredGlobal.assign(boneCount, 0);
		m_hasAppliedGlobal.assign(boneCount, 0);
		m_originalLocalTranslation.resize(boneCount);
	}

	std::fill(m_hasDesiredGlobal.begin(), m_hasDesiredGlobal.end(), uint8_t{ 0 });
	std::fill(m_hasAppliedGlobal.begin(), m_hasAppliedGlobal.end(), uint8_t{ 0 });

	const size_t rbCount = std::min(rbDefs.size(), m_bodies.size());
	auto ShouldWriteBack = [&](const PmxModel::RigidBody& def, const Body& b) -> bool
		{
			if (def.operation == PmxModel::RigidBody::OperationType::Static) return false;
			if (b.invMass <= 0.0f) return false;
			return true;
		};

	bool needsTranslationBackup = false;
	for (uint8_t flag : m_keepTranslationFlags)
	{
		if (flag)
		{
			needsTranslationBackup = true;
			break;
		}
	}

	if (needsTranslationBackup)
	{
		for (size_t bi = 0; bi < boneCount; ++bi)
		{
			if (!m_keepTranslationFlags[bi]) continue;
			const XMMATRIX lm = XMLoadFloat4x4(&bones.GetBoneLocalMatrix(bi));
			XMFLOAT3 t; XMFLOAT4 r;
			DecomposeTR(lm, t, r);
			m_originalLocalTranslation[bi] = t;
		}
	}

	for (size_t i = 0; i < rbCount; ++i)
	{
		const auto& def = rbDefs[i];
		if (def.boneIndex < 0 || def.boneIndex >= static_cast<int>(boneCount)) continue;

		const Body& b = m_bodies[i];
		if (!ShouldWriteBack(def, b)) continue;

		XMFLOAT3 renderPos = b.position;
		XMFLOAT4 renderRot = b.rotation;

		const XMVECTOR pCheck = Load3(renderPos);
		const XMVECTOR qCheck = Load4(renderRot);
		if (!IsVectorFinite3(pCheck) || !IsVectorFinite4(qCheck)) continue;

		const XMMATRIX rbG = MatrixFromTR(renderPos, renderRot);
		const XMMATRIX localFromBone = XMLoadFloat4x4(&b.localFromBone);
		const XMMATRIX invLocalFromBone = XMMatrixInverse(nullptr, localFromBone);
		const XMMATRIX boneG = invLocalFromBone * rbG;

		XMFLOAT4X4 g{};
		XMStoreFloat4x4(&g, boneG);
		m_desiredGlobals[static_cast<size_t>(def.boneIndex)] = g;
		m_hasDesiredGlobal[static_cast<size_t>(def.boneIndex)] = 1;
	}

	if (std::none_of(m_hasDesiredGlobal.begin(), m_hasDesiredGlobal.end(), [](uint8_t v) { return v != 0; }))
	{
		return;
	}

	for (int boneIndex : m_writeBackOrder)
	{
		if (boneIndex < 0 || boneIndex >= static_cast<int>(boneCount)) continue;
		if (!m_hasDesiredGlobal[static_cast<size_t>(boneIndex)]) continue;

		const auto& boneDef = bonesDef[static_cast<size_t>(boneIndex)];

		const XMMATRIX desiredG = XMLoadFloat4x4(&m_desiredGlobals[static_cast<size_t>(boneIndex)]);

		XMFLOAT4X4 checkG;
		XMStoreFloat4x4(&checkG, desiredG);
		bool valid = true;
		for (int k = 0; k < 16; ++k)
		{
			if (!std::isfinite(checkG.m[k / 4][k % 4]))
			{
				valid = false; break;
			}
		}
		if (!valid) continue;

		XMMATRIX parentG = XMMatrixIdentity();
		if (boneDef.parentIndex >= 0)
		{
			const size_t parentIndex = static_cast<size_t>(boneDef.parentIndex);
			if (parentIndex < m_hasAppliedGlobal.size() && m_hasAppliedGlobal[parentIndex])
			{
				parentG = XMLoadFloat4x4(&m_appliedGlobals[parentIndex]);
			}
			else
			{
				parentG = XMLoadFloat4x4(&bones.GetBoneGlobalMatrix(parentIndex));
			}

			const XMVECTOR rel = XMVectorSubtract(
				Load3(boneDef.position),
				Load3(bonesDef[static_cast<size_t>(boneDef.parentIndex)].position));
			parentG = XMMatrixTranslationFromVector(rel) * parentG;
		}
		else
		{
			parentG = XMMatrixTranslationFromVector(Load3(boneDef.position));
		}

		const 		XMMATRIX localMat = desiredG * XMMatrixInverse(nullptr, parentG);

		XMFLOAT3 t;
		XMFLOAT4 r;
		DecomposeTR(localMat, t, r);

		if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.z)) continue;
		if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z) || !std::isfinite(r.w)) continue;

		if (m_settings.writebackAngleThresholdDeg > 0.0f)
		{
			const XMMATRIX currentLocalMat = XMLoadFloat4x4(&bones.GetBoneLocalMatrix(static_cast<size_t>(boneIndex)));
			XMFLOAT3 currentT{};
			XMFLOAT4 currentR{};
			DecomposeTR(currentLocalMat, currentT, currentR);
			XMVECTOR qCurrent = XMQuaternionNormalize(XMLoadFloat4(&currentR));
			XMVECTOR qNew = XMQuaternionNormalize(XMLoadFloat4(&r));
			float dot = std::fabs(XMVectorGetX(XMVector4Dot(qCurrent, qNew)));
			dot = std::clamp(dot, 0.0f, 1.0f);
			float angleDeg = 2.0f * std::acos(dot) * (180.0f / XM_PI);
			if (angleDeg < m_settings.writebackAngleThresholdDeg)
			{
				continue;
			}
		}

		if (boneIndex >= 0 && static_cast<size_t>(boneIndex) < m_keepTranslationFlags.size() && m_keepTranslationFlags[static_cast<size_t>(boneIndex)])
		{
			t = m_originalLocalTranslation[static_cast<size_t>(boneIndex)];
		}

		bones.SetBoneLocalPose(static_cast<size_t>(boneIndex), t, r);

		const XMMATRIX appliedLocal = MatrixFromTR(t, r);
		const XMMATRIX appliedG = appliedLocal * parentG;

		XMStoreFloat4x4(&m_appliedGlobals[static_cast<size_t>(boneIndex)], appliedG);
		m_hasAppliedGlobal[static_cast<size_t>(boneIndex)] = 1;
	}

}

XMVECTOR MmdPhysicsWorld::Load3(const XMFLOAT3& v)
{
	return XMLoadFloat3(&v);
}
XMVECTOR MmdPhysicsWorld::Load4(const XMFLOAT4& v)
{
	return XMLoadFloat4(&v);
}
void MmdPhysicsWorld::Store3(XMFLOAT3& o, XMVECTOR v)
{
	XMStoreFloat3(&o, v);
}
void MmdPhysicsWorld::Store4(XMFLOAT4& o, XMVECTOR v)
{
	XMStoreFloat4(&o, v);
}

XMMATRIX MmdPhysicsWorld::MatrixFromTR(const XMFLOAT3& t, const XMFLOAT4& r)
{
	return XMMatrixRotationQuaternion(XMLoadFloat4(&r)) * XMMatrixTranslationFromVector(XMLoadFloat3(&t));
}
void MmdPhysicsWorld::DecomposeTR(const XMMATRIX& m, XMFLOAT3& outT, XMFLOAT4& outR)
{
	XMVECTOR s, r, t;
	if (!XMMatrixDecompose(&s, &r, &t, m))
	{
		XMStoreFloat3(&outT, m.r[3]);
		outR = { 0.0f, 0.0f, 0.0f, 1.0f };
		return;
	}
	XMStoreFloat3(&outT, t);
	XMStoreFloat4(&outR, XMQuaternionNormalize(r));
	if (!std::isfinite(outR.x) || !std::isfinite(outR.y) || !std::isfinite(outR.z) || !std::isfinite(outR.w))
	{
		outR = { 0.0f, 0.0f, 0.0f, 1.0f };
	}
}
DirectX::XMFLOAT3 MmdPhysicsWorld::ExtractTranslation(const DirectX::XMMATRIX& m)
{
	XMFLOAT3 t; XMStoreFloat3(&t, m.r[3]); return t;
}
float MmdPhysicsWorld::ComputeDepth(const std::vector<PmxModel::Bone>& bones, int boneIndex)
{
	float d = 0; int c = boneIndex; int g = 0;
	while (c >= 0 && c < (int)bones.size() && g++ < 1000)
	{
		c = bones[c].parentIndex; d++;
	}
	return d;
}

