#include "BulletPhysicsAdapter.hpp"
#include <algorithm>
#include <cmath>

BulletRigidBodyParams BulletPhysicsAdapter::ApproximateRigidBody(const PmxModel::RigidBody& def)
{
	BulletRigidBodyParams out{};
	out.linearDamping = ClampNonNegative(def.linearDamping);
	out.angularDamping = ClampNonNegative(def.angularDamping);
	out.friction = ClampNonNegative(def.friction);
	out.restitution = ClampNonNegative(def.restitution);

	const bool dynamic = (def.operation != PmxModel::RigidBody::OperationType::Static);
	if (dynamic && def.mass > 0.0f)
	{
		out.invMass = 1.0f / def.mass;
	}

	return out;
}

float BulletPhysicsAdapter::ClampNonNegative(float value)
{
	return std::max(value, 0.0f);
}
