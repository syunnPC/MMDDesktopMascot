#pragma once

#include <DirectXMath.h>
#include "PmxModel.hpp"
#include "Settings.hpp"

struct BulletRigidBodyParams
{
	float linearDamping{};
	float angularDamping{};
	float friction{};
	float restitution{};
	float invMass{};
};

class BulletPhysicsAdapter
{
public:
	static BulletRigidBodyParams ApproximateRigidBody(const PmxModel::RigidBody& def);

private:
	static float ClampNonNegative(float value);
};
