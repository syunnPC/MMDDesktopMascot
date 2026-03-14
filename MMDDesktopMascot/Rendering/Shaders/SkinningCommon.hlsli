float4x4 GetSkinMatrix(int4 indices, float4 weights)
{
    float4x4 skinMat = (float4x4)0;
    float totalWeight = 0.0;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (indices[i] >= 0 && indices[i] < 1024 && weights[i] > 0.0)
        {
            skinMat += g_boneMatrices[indices[i]] * weights[i];
            totalWeight += weights[i];
        }
    }

    if (totalWeight < 0.001)
    {
        return float4x4(
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1);
    }

    return skinMat / totalWeight;
}

float4 QuaternionFromMatrix(float3x3 m)
{
    float4 q;
    float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0)
    {
        float s = sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25 * s;
    }
    return normalize(q);
}

float3 RotateByQuaternion(float3 v, float4 q)
{
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float4 QuaternionMultiply(float4 a, float4 b)
{
    return float4(
        a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
        a.w * b.w - dot(a.xyz, b.xyz));
}

float4 QuaternionConjugate(float4 q)
{
    return float4(-q.xyz, q.w);
}

void ApplyLinearSkin(VSIn i, out float3 skinnedPos, out float3 skinnedNrm)
{
    float4x4 skinMat = GetSkinMatrix(i.boneIndices, i.boneWeights);
    float4 pos4 = mul(float4(i.pos, 1.0), skinMat);
    skinnedPos = pos4.xyz;
    float3x3 skinMat3x3 = (float3x3)skinMat;
    skinnedNrm = normalize(mul(i.nrm, skinMat3x3));
}

void ApplySdefSkin(VSIn i, out float3 skinnedPos, out float3 skinnedNrm)
{
    skinnedPos = i.pos;
    skinnedNrm = i.nrm;

    int idx0 = i.boneIndices[0];
    int idx1 = i.boneIndices[1];

    if (idx0 < 0 || idx1 < 0 || idx0 >= 1024 || idx1 >= 1024)
    {
        ApplyLinearSkin(i, skinnedPos, skinnedNrm);
        return;
    }

    float w0 = saturate(i.boneWeights[0]);
    float w1Raw = saturate(i.boneWeights[1]);
    float w1 = (w1Raw > 0.0001) ? w1Raw : (1.0 - w0);

    float sumW = w0 + w1;
    if (sumW > 0.0001)
    {
        w0 /= sumW;
        w1 /= sumW;
    }
    else
    {
        w0 = 1.0;
        w1 = 0.0;
    }

    float4x4 matA4 = g_boneMatrices[idx0];
    float4x4 matB4 = g_boneMatrices[idx1];
    float3x3 matA = (float3x3)matA4;
    float3x3 matB = (float3x3)matB4;

    float4 qa = QuaternionFromMatrix(matA);
    float4 qb = QuaternionFromMatrix(matB);
    if (dot(qa, qb) < 0.0)
    {
        qb = -qb;
    }

    float4 q = normalize(qa * w0 + qb * w1);

    float3 C = i.sdefC;
    float3 R0 = i.sdefR0;
    float3 R1 = i.sdefR1;

    float3 rTilde = R0 * w0 + R1 * w1;
    float3 C0 = C + (R0 - rTilde) * 0.5;
    float3 C1 = C + (R1 - rTilde) * 0.5;

    float3 term0 = mul(float4(C0, 1.0), matA4).xyz * w0;
    float3 term1 = mul(float4(C1, 1.0), matB4).xyz * w1;

    float3 rotated = RotateByQuaternion(i.pos - C, q);

    skinnedPos = term0 + term1 + rotated;
    skinnedNrm = normalize(RotateByQuaternion(i.nrm, q));
}

void ApplyQdefSkin(VSIn i, out float3 skinnedPos, out float3 skinnedNrm)
{
    skinnedPos = i.pos;
    skinnedNrm = i.nrm;

    float4 blendedReal = float4(0.0, 0.0, 0.0, 0.0);
    float4 blendedDual = float4(0.0, 0.0, 0.0, 0.0);
    float4 referenceReal = float4(0.0, 0.0, 0.0, 1.0);
    float totalWeight = 0.0;
    bool hasReference = false;

    [unroll]
    for (int influenceIndex = 0; influenceIndex < 4; ++influenceIndex)
    {
        const int boneIndex = i.boneIndices[influenceIndex];
        const float weight = i.boneWeights[influenceIndex];
        if (boneIndex < 0 || boneIndex >= 1024 || weight <= 0.0)
        {
            continue;
        }

        const float4x4 boneMatrix = g_boneMatrices[boneIndex];
        float4 real = QuaternionFromMatrix((float3x3)boneMatrix);

        if (!hasReference)
        {
            referenceReal = real;
            hasReference = true;
        }
        else if (dot(real, referenceReal) < 0.0)
        {
            real = -real;
        }

        const float3 translation = mul(float4(0.0, 0.0, 0.0, 1.0), boneMatrix).xyz;
        const float4 dual = 0.5 * QuaternionMultiply(float4(translation, 0.0), real);

        blendedReal += real * weight;
        blendedDual += dual * weight;
        totalWeight += weight;
    }

    if (!hasReference || totalWeight <= 0.001)
    {
        ApplyLinearSkin(i, skinnedPos, skinnedNrm);
        return;
    }

    blendedReal /= totalWeight;
    blendedDual /= totalWeight;

    const float realLength = length(blendedReal);
    if (realLength <= 1.0e-5)
    {
        ApplyLinearSkin(i, skinnedPos, skinnedNrm);
        return;
    }

    blendedReal /= realLength;
    blendedDual /= realLength;
    blendedDual -= blendedReal * dot(blendedReal, blendedDual);

    const float4 translationQuat = QuaternionMultiply(blendedDual, QuaternionConjugate(blendedReal));
    const float3 translation = 2.0 * translationQuat.xyz;

    skinnedPos = RotateByQuaternion(i.pos, blendedReal) + translation;
    skinnedNrm = normalize(RotateByQuaternion(i.nrm, blendedReal));
}
