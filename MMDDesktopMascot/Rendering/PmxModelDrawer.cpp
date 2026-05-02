#include "PmxModelDrawer.hpp"

#include "d3dx12.h"
#include "ExceptionHelper.hpp"
#include "DebugUtil.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	std::wstring ToLowerW(std::wstring s)
	{
		std::transform(s.begin(), s.end(), s.begin(),
					   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
		return s;
	}

	static bool ContainsAnyW(const std::wstring& hay, std::initializer_list<const wchar_t*> needles)
	{
		const std::wstring low = ToLowerW(hay);
		for (auto n : needles)
		{
			if (low.find(n) != std::wstring::npos) return true;
		}
		return false;
	}

	int TryParseTypeTag(const std::wstring& memo)
	{
		if (memo.empty()) return -1;
		const auto m = ToLowerW(memo);
		auto has = [&](const wchar_t* s) { return m.find(s) != std::wstring::npos; };

		if (has(L"type=face") || has(L"type:face") || has(L"#face")) return 3;
		if (has(L"type=eye") || has(L"type:eye") || has(L"#eye"))  return 4;
		if (has(L"type=skin") || has(L"type:skin") || has(L"#skin")) return 1;
		if (has(L"type=hair") || has(L"type:hair") || has(L"#hair")) return 2;
		if (has(L"type=glass") || has(L"type:glass") || has(L"#glass")) return 5;
		return -1;
	}

	uint32_t GuessMaterialType(const PmxModel::Material& mat)
	{
		if (int t = TryParseTypeTag(mat.memo); t >= 0)
			return static_cast<uint32_t>(t);

		const std::wstring n = mat.name;
		const std::wstring ne = mat.nameEn;

		if (ContainsAnyW(n, { L"目", L"瞳", L"eye", L"iris", L"pupil" }) ||
			ContainsAnyW(ne, { L"eye", L"iris" })) return 4;

		if (ContainsAnyW(n, { L"顔", L"face", L"頬", L"ほほ" }) ||
			ContainsAnyW(ne, { L"face", L"cheek" })) return 3;

		if (ContainsAnyW(n, { L"髪", L"hair", L"ヘア" }) ||
			ContainsAnyW(ne, { L"hair" })) return 2;

		if (ContainsAnyW(n, { L"肌", L"skin" }) ||
			ContainsAnyW(ne, { L"skin" })) return 1;

		if (mat.diffuse[3] < 0.98f ||
			ContainsAnyW(n, { L"glass", L"透明" }) ||
			ContainsAnyW(ne, { L"glass", L"transparent" })) return 5;

		const float avg = (mat.diffuse[0] + mat.diffuse[1] + mat.diffuse[2]) / 3.0f;
		if (mat.specularPower >= 80.0f) return 2;
		if (avg >= 0.55f && mat.specularPower <= 25.0f) return 1;

		return 0;
	}

	bool LooksLikeFaceMaterial(const PmxModel::Material& m)
	{
		const std::wstring all = m.name + L" " + m.nameEn + L" " + m.memo;
		const std::wstring low = ToLowerW(all);

		if (low.find(L"face") != std::wstring::npos) return true;
		if (low.find(L"facial") != std::wstring::npos) return true;

		if (all.find(L"顔") != std::wstring::npos) return true;
		if (all.find(L"かお") != std::wstring::npos) return true;
		if (all.find(L"頭部") != std::wstring::npos) return true;

		return false;
	}

	void GetMaterialStyleParams(
		uint32_t type,
		float& outRimMul,
		float& outSpecMul,
		float& outShadowMul,
		float& outToonContrastMul)
	{
		outRimMul = 1.0f; outSpecMul = 1.0f; outShadowMul = 1.0f; outToonContrastMul = 1.0f;

		switch (type)
		{
			case 3:
				outRimMul = 0.55f; outSpecMul = 0.35f; outShadowMul = 0.60f; outToonContrastMul = 0.85f; break;
			case 1:
				outRimMul = 0.65f; outSpecMul = 0.45f; outShadowMul = 0.70f; outToonContrastMul = 0.90f; break;
			case 2:
				outRimMul = 1.00f; outSpecMul = 1.35f; outShadowMul = 1.00f; outToonContrastMul = 1.05f; break;
			case 4:
				outRimMul = 0.20f; outSpecMul = 1.20f; outShadowMul = 0.85f; outToonContrastMul = 1.00f; break;
			case 5:
				outRimMul = 1.10f; outSpecMul = 1.00f; outShadowMul = 1.00f; outToonContrastMul = 1.00f; break;
		}
	}

	static bool IsEyeOrLashMaterial(
		const PmxModel::Material& m,
		const std::vector<std::filesystem::path>& texPaths)
	{
		if (ContainsAnyW(m.name + L" " + m.nameEn + L" " + m.memo,
						 { L"eye", L"iris", L"pupil", L"eyeball", L"lash", L"eyelash", L"eyeline",
						   L"hitomi", L"matsuge", L"matuge", L"目", L"瞳", L"白目", L"虹彩",
						   L"まつ毛", L"まつげ", L"睫毛", L"アイライン" }))
		{
			return true;
		}

		auto checkIdx = [&](int32_t idx) -> bool {
			if (idx < 0 || idx >= static_cast<int32_t>(texPaths.size())) return false;
			const std::wstring file = texPaths[idx].filename().wstring();
			return ContainsAnyW(file,
								{ L"eye", L"iris", L"pupil", L"eyeball", L"lash", L"eyelash", L"white", L"hitomi" });
			};

		if (checkIdx(m.textureIndex)) return true;
		if (m.toonFlag == 0 && checkIdx(m.toonIndex)) return true;
		if (checkIdx(m.sphereTextureIndex)) return true;

		return false;
	}

	DirectX::XMFLOAT3 ComputeMaterialCenter(
		const PmxModel::Material& mat,
		const std::vector<PmxModel::Vertex>& verts,
		const std::vector<uint32_t>& inds)
	{
		if (mat.indexCount == 0 || mat.indexOffset >= inds.size())
		{
			return {};
		}

		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float minZ = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();
		float maxY = std::numeric_limits<float>::lowest();
		float maxZ = std::numeric_limits<float>::lowest();
		bool hasVertex = false;

		const size_t start = static_cast<size_t>(mat.indexOffset);
		const size_t end = std::min(inds.size(), start + static_cast<size_t>(mat.indexCount));
		for (size_t ii = start; ii < end; ++ii)
		{
			const uint32_t vertexIndex = inds[ii];
			if (vertexIndex >= verts.size())
			{
				continue;
			}

			const auto& v = verts[vertexIndex];
			minX = std::min(minX, v.px);
			minY = std::min(minY, v.py);
			minZ = std::min(minZ, v.pz);
			maxX = std::max(maxX, v.px);
			maxY = std::max(maxY, v.py);
			maxZ = std::max(maxZ, v.pz);
			hasVertex = true;
		}

		if (!hasVertex)
		{
			return {};
		}

		return {
			(minX + maxX) * 0.5f,
			(minY + maxY) * 0.5f,
			(minZ + maxZ) * 0.5f
		};
	}

	constexpr float kMorphWeightThreshold = 1e-4f;

	bool HasMorphWeight(float weight) noexcept
	{
		return std::abs(weight) > kMorphWeightThreshold;
	}

	bool IsUvMorphType(PmxModel::Morph::Type type) noexcept
	{
		switch (type)
		{
			case PmxModel::Morph::Type::UV:
			case PmxModel::Morph::Type::AdditionalUV1:
			case PmxModel::Morph::Type::AdditionalUV2:
			case PmxModel::Morph::Type::AdditionalUV3:
			case PmxModel::Morph::Type::AdditionalUV4:
				return true;
			default:
				return false;
		}
	}

	float* SelectAdditionalUvTarget(PmxModelDrawer::PmxVsVertex& vertex, std::uint8_t channel) noexcept
	{
		switch (channel)
		{
			case 1: return vertex.addUv1;
			case 2: return vertex.addUv2;
			case 3: return vertex.addUv3;
			case 4: return vertex.addUv4;
			default: return nullptr;
		}
	}

	void ApplyMultiply(DirectX::XMFLOAT4& target, const DirectX::XMFLOAT4& offset, float weight) noexcept
	{
		target.x *= (1.0f + (offset.x - 1.0f) * weight);
		target.y *= (1.0f + (offset.y - 1.0f) * weight);
		target.z *= (1.0f + (offset.z - 1.0f) * weight);
		target.w *= (1.0f + (offset.w - 1.0f) * weight);
	}

	void ApplyMultiply(DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& offset, float weight) noexcept
	{
		target.x *= (1.0f + (offset.x - 1.0f) * weight);
		target.y *= (1.0f + (offset.y - 1.0f) * weight);
		target.z *= (1.0f + (offset.z - 1.0f) * weight);
	}

	void ApplyMultiply(float& target, float offset, float weight) noexcept
	{
		target *= (1.0f + (offset - 1.0f) * weight);
	}

	void ApplyAdd(DirectX::XMFLOAT4& target, const DirectX::XMFLOAT4& offset, float weight) noexcept
	{
		target.x += offset.x * weight;
		target.y += offset.y * weight;
		target.z += offset.z * weight;
		target.w += offset.w * weight;
	}

	void ApplyAdd(DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& offset, float weight) noexcept
	{
		target.x += offset.x * weight;
		target.y += offset.y * weight;
		target.z += offset.z * weight;
	}

	void ApplyAdd(float& target, float offset, float weight) noexcept
	{
		target += offset * weight;
	}

	std::filesystem::path FindNormalMapPath(const std::filesystem::path& baseTexturePath)
	{
		if (baseTexturePath.empty()) return {};
		const std::filesystem::path dir = baseTexturePath.parent_path();
		const std::wstring stem = baseTexturePath.stem().wstring();
		const std::wstring ext = baseTexturePath.extension().wstring();

		const std::vector<std::wstring> suffixes = { L"_normal", L"_n", L"_norm" };
		for (const auto& suffix : suffixes)
		{
			std::filesystem::path candidate = dir / (stem + suffix + ext);
			if (std::filesystem::exists(candidate)) return candidate;
			candidate = dir / (stem + suffix + L".png");
			if (std::filesystem::exists(candidate)) return candidate;
		}
		return {};
	}

	void ComputeVertexTangents(
		std::vector<PmxModelDrawer::PmxVsVertex>& vertices,
		const std::vector<uint32_t>& indices)
	{
		using namespace DirectX;
		struct Accumulator
		{
			XMFLOAT3 tangent = { 0,0,0 };
			XMFLOAT3 bitangent = { 0,0,0 };
		};
		std::vector<Accumulator> acc(vertices.size());

		for (size_t i = 0; i + 2 < indices.size(); i += 3)
		{
			const uint32_t i0 = indices[i];
			const uint32_t i1 = indices[i + 1];
			const uint32_t i2 = indices[i + 2];
			if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

			const auto& v0 = vertices[i0];
			const auto& v1 = vertices[i1];
			const auto& v2 = vertices[i2];

			XMVECTOR p0 = XMVectorSet(v0.px, v0.py, v0.pz, 0);
			XMVECTOR p1 = XMVectorSet(v1.px, v1.py, v1.pz, 0);
			XMVECTOR p2 = XMVectorSet(v2.px, v2.py, v2.pz, 0);

			XMVECTOR e1 = XMVectorSubtract(p1, p0);
			XMVECTOR e2 = XMVectorSubtract(p2, p0);

			float du1 = v1.u - v0.u;
			float dv1 = v1.v - v0.v;
			float du2 = v2.u - v0.u;
			float dv2 = v2.v - v0.v;

			float det = du1 * dv2 - du2 * dv1;
			if (std::abs(det) < 1e-8f) continue;

			float invDet = 1.0f / det;
			XMVECTOR t = XMVectorScale(XMVectorSubtract(XMVectorScale(e1, dv2), XMVectorScale(e2, dv1)), invDet);
			XMVECTOR b = XMVectorScale(XMVectorSubtract(XMVectorScale(e2, du1), XMVectorScale(e1, du2)), invDet);

			auto add = [](XMFLOAT3& dst, XMVECTOR src) {
				dst.x += XMVectorGetX(src);
				dst.y += XMVectorGetY(src);
				dst.z += XMVectorGetZ(src);
			};
			add(acc[i0].tangent, t);
			add(acc[i0].bitangent, b);
			add(acc[i1].tangent, t);
			add(acc[i1].bitangent, b);
			add(acc[i2].tangent, t);
			add(acc[i2].bitangent, b);
		}

		for (size_t i = 0; i < vertices.size(); ++i)
		{
			XMVECTOR n = XMVectorSet(vertices[i].nx, vertices[i].ny, vertices[i].nz, 0);
			XMVECTOR t = XMVectorSet(acc[i].tangent.x, acc[i].tangent.y, acc[i].tangent.z, 0);
			XMVECTOR b = XMVectorSet(acc[i].bitangent.x, acc[i].bitangent.y, acc[i].bitangent.z, 0);

			// Gram-Schmidt orthogonalize tangent
			t = XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t))));
			float tLenSq = XMVectorGetX(XMVector3LengthSq(t));
			if (tLenSq > 1e-8f)
			{
				t = XMVector3Normalize(t);
			}
			else
			{
				// Degenerate: pick arbitrary perpendicular vector
				XMVECTOR up = XMVectorSet(0, 1, 0, 0);
				if (std::abs(XMVectorGetX(XMVector3Dot(n, up))) > 0.99f)
					up = XMVectorSet(1, 0, 0, 0);
				t = XMVector3Cross(n, up);
				t = XMVector3Normalize(t);
			}

			// Compute handedness
			XMVECTOR cross = XMVector3Cross(n, t);
			float w = (XMVectorGetX(XMVector3Dot(cross, b)) < 0.0f) ? -1.0f : 1.0f;

			vertices[i].tangent[0] = XMVectorGetX(t);
			vertices[i].tangent[1] = XMVectorGetY(t);
			vertices[i].tangent[2] = XMVectorGetZ(t);
			vertices[i].tangent[3] = w;
		}
	}

	void ClampMaterialConstants(PmxModelDrawer::MaterialCB& material) noexcept
	{
		auto saturateColor = [](float& value) { value = std::clamp(value, 0.0f, 1.0f); };
		auto clampNonNegative = [](float& value) { value = std::max(value, 0.0f); };

		saturateColor(material.diffuse.x);
		saturateColor(material.diffuse.y);
		saturateColor(material.diffuse.z);
		saturateColor(material.diffuse.w);
		saturateColor(material.specular.x);
		saturateColor(material.specular.y);
		saturateColor(material.specular.z);
		saturateColor(material.ambient.x);
		saturateColor(material.ambient.y);
		saturateColor(material.ambient.z);
		saturateColor(material.edgeColor.x);
		saturateColor(material.edgeColor.y);
		saturateColor(material.edgeColor.z);
		saturateColor(material.edgeColor.w);

		clampNonNegative(material.textureFactor.x);
		clampNonNegative(material.textureFactor.y);
		clampNonNegative(material.textureFactor.z);
		clampNonNegative(material.textureFactor.w);
		clampNonNegative(material.sphereFactor.x);
		clampNonNegative(material.sphereFactor.y);
		clampNonNegative(material.sphereFactor.z);
		clampNonNegative(material.sphereFactor.w);
		clampNonNegative(material.toonFactor.x);
		clampNonNegative(material.toonFactor.y);
		clampNonNegative(material.toonFactor.z);
		clampNonNegative(material.toonFactor.w);
		material.edgeSize = std::max(0.0f, material.edgeSize);
	}
}

void PmxModelDrawer::Initialize(Dx12Context* ctx, GpuResourceManager* resources)
{
	m_ctx = ctx;
	m_resources = resources;
}

void PmxModelDrawer::EnsurePmxResources(const PmxModel* model, const LightSettings& lightSettings)
{
	if (!model || !model->HasGeometry())
	{
		m_pmx.ready = false;
		return;
	}

	if (m_pmx.ready && m_pmx.revision == model->Revision())
	{
		return;
	}

	for (int i = 0; i < 2; ++i)
	{
		if (m_vertexUpload[i] && m_vertexUploadMapped[i])
		{
			m_vertexUpload[i]->Unmap(0, nullptr);
			m_vertexUploadMapped[i] = nullptr;
		}
		m_vertexUpload[i] = nullptr;
	}
	m_currentUploadBuffer = 0;

	m_indexUpload = nullptr;
	m_pendingVertexUploadRanges.clear();
	m_indexUploadPending = false;
	m_vertexBufferSizeBytes = 0;
	m_indexBufferSizeBytes = 0;
	m_vertexTriangleOffsets.clear();
	m_vertexTriangleIndices.clear();

	m_pmx = {};
	m_appliedMorphWeights.clear();
	m_activeVertexMorphIndices.clear();
	m_activeMaterialMorphIndices.clear();
	m_hasAppliedLightSettings = false;
	m_hasAppliedVertexOverrides = false;
	m_hasAppliedMaterialMorphs = false;

	const auto& verts = model->Vertices();
	const auto& inds = model->Indices();
	const auto& mats = model->Materials();
	const auto& texPaths = model->TexturePaths();

	std::vector<PmxVsVertex> vtx;
	vtx.reserve(verts.size());

	const auto boneCount = model->Bones().size();
	if (boneCount > MaxBones)
	{
		throw std::runtime_error("Model bone count exceeds renderer limit.");
	}

	for (const auto& v : verts)
	{
		PmxVsVertex pv{};
		pv.px = v.px; pv.py = v.py; pv.pz = v.pz;
		pv.nx = v.nx; pv.ny = v.ny; pv.nz = v.nz;
		pv.u = v.u; pv.v = v.v;
		pv.edgeScale = v.edgeScale;
		pv.addUv1[0] = v.additionalUV[0].x; pv.addUv1[1] = v.additionalUV[0].y; pv.addUv1[2] = v.additionalUV[0].z; pv.addUv1[3] = v.additionalUV[0].w;
		pv.addUv2[0] = v.additionalUV[1].x; pv.addUv2[1] = v.additionalUV[1].y; pv.addUv2[2] = v.additionalUV[1].z; pv.addUv2[3] = v.additionalUV[1].w;
		pv.addUv3[0] = v.additionalUV[2].x; pv.addUv3[1] = v.additionalUV[2].y; pv.addUv3[2] = v.additionalUV[2].z; pv.addUv3[3] = v.additionalUV[2].w;
		pv.addUv4[0] = v.additionalUV[3].x; pv.addUv4[1] = v.additionalUV[3].y; pv.addUv4[2] = v.additionalUV[3].z; pv.addUv4[3] = v.additionalUV[3].w;
		pv.tangent[0] = 0.0f; pv.tangent[1] = 0.0f; pv.tangent[2] = 0.0f; pv.tangent[3] = 1.0f;

		for (int i = 0; i < 4; ++i)
		{
			pv.boneIndices[i] = -1;
			pv.boneWeights[i] = 0.0f;
		}

		int32_t fallbackBone = -1;
		for (int i = 0; i < 4; ++i)
		{
			int32_t boneIdx = v.weight.boneIndices[i];
			float weight = v.weight.weights[i];

			if (boneIdx >= 0 &&
				boneIdx < static_cast<int32_t>(boneCount) &&
				boneIdx < static_cast<int32_t>(MaxBones) &&
				weight > 0.0f)
			{
				pv.boneIndices[i] = boneIdx;
				pv.boneWeights[i] = weight;
				if (fallbackBone < 0) fallbackBone = boneIdx;
			}
		}

		float totalWeight = pv.boneWeights[0] + pv.boneWeights[1] +
			pv.boneWeights[2] + pv.boneWeights[3];

		if (totalWeight > 0.001f)
		{
			for (int i = 0; i < 4; ++i)
			{
				pv.boneWeights[i] /= totalWeight;
			}
		}
		else if (fallbackBone >= 0)
		{
			pv.boneIndices[0] = fallbackBone;
			pv.boneWeights[0] = 1.0f;
		}

		pv.weightType = v.weight.type;
		if (v.weight.type == 3)
		{
			pv.sdefC[0] = v.weight.sdefC.x;
			pv.sdefC[1] = v.weight.sdefC.y;
			pv.sdefC[2] = v.weight.sdefC.z;
			pv.sdefR0[0] = v.weight.sdefR0.x;
			pv.sdefR0[1] = v.weight.sdefR0.y;
			pv.sdefR0[2] = v.weight.sdefR0.z;
			pv.sdefR1[0] = v.weight.sdefR1.x;
			pv.sdefR1[1] = v.weight.sdefR1.y;
			pv.sdefR1[2] = v.weight.sdefR1.z;
		}

		vtx.push_back(pv);
	}

	m_baseVertices = vtx;
	ComputeVertexTangents(m_baseVertices, inds);
	m_workingVertices = m_baseVertices;
	m_morphWeights.resize(model->Morphs().size());

	const size_t vertexCount = vtx.size();
	m_vertexTriangleOffsets.assign(vertexCount + 1, 0);
	for (size_t index = 0; index + 2 < inds.size(); index += 3)
	{
		const uint32_t ia = inds[index];
		const uint32_t ib = inds[index + 1];
		const uint32_t ic = inds[index + 2];
		if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
		{
			continue;
		}

		++m_vertexTriangleOffsets[static_cast<size_t>(ia) + 1];
		++m_vertexTriangleOffsets[static_cast<size_t>(ib) + 1];
		++m_vertexTriangleOffsets[static_cast<size_t>(ic) + 1];
	}

	for (size_t vertexIndex = 1; vertexIndex < m_vertexTriangleOffsets.size(); ++vertexIndex)
	{
		m_vertexTriangleOffsets[vertexIndex] += m_vertexTriangleOffsets[vertexIndex - 1];
	}

	m_vertexTriangleIndices.assign(m_vertexTriangleOffsets.back(), 0);
	auto adjacencyWriteOffsets = m_vertexTriangleOffsets;
	for (size_t index = 0; index + 2 < inds.size(); index += 3)
	{
		const uint32_t ia = inds[index];
		const uint32_t ib = inds[index + 1];
		const uint32_t ic = inds[index + 2];
		if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
		{
			continue;
		}

		const uint32_t triangleStart = static_cast<uint32_t>(index);
		m_vertexTriangleIndices[adjacencyWriteOffsets[ia]++] = triangleStart;
		m_vertexTriangleIndices[adjacencyWriteOffsets[ib]++] = triangleStart;
		m_vertexTriangleIndices[adjacencyWriteOffsets[ic]++] = triangleStart;
	}

	const UINT64 vbSize = static_cast<UINT64>(vtx.size()) * sizeof(PmxVsVertex);
	const UINT64 ibSize = static_cast<UINT64>(inds.size()) * sizeof(uint32_t);
	m_vertexBufferSizeBytes = vbSize;
	m_indexBufferSizeBytes = ibSize;

	auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

	{
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
		DX_CALL(m_ctx->Device()->CreateCommittedResource(
			&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(m_pmx.vb.put())));

		for (int i = 0; i < 2; ++i)
		{
			DX_CALL(m_ctx->Device()->CreateCommittedResource(
				&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(m_vertexUpload[i].put())));

			void* mappedUpload = nullptr;
			CD3DX12_RANGE range(0, 0);
			DX_CALL(m_vertexUpload[i]->Map(0, &range, &mappedUpload));
			m_vertexUploadMapped[i] = static_cast<uint8_t*>(mappedUpload);
			std::memcpy(m_vertexUploadMapped[i], m_baseVertices.data(), static_cast<size_t>(vbSize));
		}
		m_pendingVertexUploadRanges.emplace_back(0, vbSize);
		m_vertexBufferState = D3D12_RESOURCE_STATE_COPY_DEST;
		m_currentUploadBuffer = 0;

		m_pmx.vbv.BufferLocation = m_pmx.vb->GetGPUVirtualAddress();
		m_pmx.vbv.StrideInBytes = sizeof(PmxVsVertex);
		m_pmx.vbv.SizeInBytes = static_cast<UINT>(vbSize);
	}

	{
		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
		DX_CALL(m_ctx->Device()->CreateCommittedResource(
			&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(m_pmx.ib.put())));

		DX_CALL(m_ctx->Device()->CreateCommittedResource(
			&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_indexUpload.put())));

		void* mapped = nullptr;
		CD3DX12_RANGE range(0, 0);
		DX_CALL(m_indexUpload->Map(0, &range, &mapped));
		std::memcpy(mapped, inds.data(), static_cast<size_t>(ibSize));
		m_indexUpload->Unmap(0, nullptr);
		m_indexUploadPending = true;
		m_indexBufferState = D3D12_RESOURCE_STATE_COPY_DEST;

		m_pmx.ibv.BufferLocation = m_pmx.ib->GetGPUVirtualAddress();
		m_pmx.ibv.Format = DXGI_FORMAT_R32_UINT;
		m_pmx.ibv.SizeInBytes = static_cast<UINT>(ibSize);
	}

	{
		const size_t matCount = mats.size();
		const UINT64 totalSize = m_materialCbStride * matCount;

		if (totalSize > 0)
		{
			auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);
			DX_CALL(m_ctx->Device()->CreateCommittedResource(
				&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				IID_PPV_ARGS(m_materialCb.put())));

			CD3DX12_RANGE range(0, 0);
			DX_CALL(m_materialCb->Map(0, &range, reinterpret_cast<void**>(&m_materialCbMapped)));
		}
	}

	m_pmx.materials.clear();
	m_pmx.materials.reserve(mats.size());

	for (size_t mi = 0; mi < mats.size(); ++mi)
	{
		const auto& mat = mats[mi];
		PmxGpuMaterial gm{};
		gm.mat = mat;

		float edgeSize = mat.edgeSize;
		if (IsEyeOrLashMaterial(mat, texPaths))
		{
			edgeSize = 0.0f;
		}
		gm.mat.edgeSize = edgeSize;

		uint32_t matType = GuessMaterialType(mat);
		float rimMul, specMul, shadowMul, toonContrastMul;
		GetMaterialStyleParams(matType, rimMul, specMul, shadowMul, toonContrastMul);

		gm.srvBlockIndex = m_resources->AllocSrvBlock4();

		uint32_t baseSrv = m_resources->GetDefaultWhiteSrv();
		std::filesystem::path baseTexPath;
		if (mat.textureIndex >= 0 && mat.textureIndex < static_cast<int32_t>(texPaths.size()))
		{
			baseTexPath = texPaths[mat.textureIndex];
			baseSrv = m_resources->LoadTextureSrv(baseTexPath);
		}
		if (const auto* texture = m_resources->FindTexture(baseSrv))
		{
			gm.baseTextureHasTransparency = texture->hasTransparentPixels;
			gm.baseTextureHasTranslucency = texture->hasTranslucentPixels;
		}
		m_resources->CopySrv(gm.srvBlockIndex + 0, baseSrv);

		uint32_t toonSrv = m_resources->GetDefaultToonSrv();
		if (mat.toonFlag == 0 && mat.toonIndex >= 0 && mat.toonIndex < static_cast<int32_t>(texPaths.size()))
		{
			toonSrv = m_resources->LoadToonTextureSrv(texPaths[mat.toonIndex]);
		}
		else if (mat.toonFlag != 0)
		{
			toonSrv = m_resources->LoadSharedToonSrv(model->Path().parent_path(), mat.toonIndex);
		}
		m_resources->CopySrv(gm.srvBlockIndex + 1, toonSrv);

		uint32_t sphereSrv = m_resources->GetDefaultWhiteSrv();
		if (mat.sphereTextureIndex >= 0 && mat.sphereTextureIndex < static_cast<int32_t>(texPaths.size()))
		{
			sphereSrv = m_resources->LoadTextureSrv(texPaths[mat.sphereTextureIndex]);
		}
		m_resources->CopySrv(gm.srvBlockIndex + 2, sphereSrv);

		uint32_t normalSrv = m_resources->GetDefaultNormalSrv();
		std::filesystem::path normalPath = FindNormalMapPath(baseTexPath);
		if (!normalPath.empty())
		{
			normalSrv = m_resources->LoadTextureSrv(normalPath);
			gm.hasNormalMap = true;
		}
		gm.normalMapSrv = normalSrv;
		m_resources->CopySrv(gm.srvBlockIndex + 3, normalSrv);

		const bool isFace = lightSettings.faceMaterialOverridesEnabled && LooksLikeFaceMaterial(mat);
		if (isFace)
		{
			shadowMul = lightSettings.faceShadowMul;
			toonContrastMul = lightSettings.faceToonContrastMul;
		}

		gm.materialCbGpu = m_materialCb->GetGPUVirtualAddress() + mi * m_materialCbStride;
		gm.localCenter = ComputeMaterialCenter(gm.mat, verts, inds);

		MaterialCB* mcb = reinterpret_cast<MaterialCB*>(m_materialCbMapped + mi * m_materialCbStride);
		mcb->diffuse = { mat.diffuse[0],  mat.diffuse[1],  mat.diffuse[2],  mat.diffuse[3] };
		mcb->ambient = { mat.ambient[0],  mat.ambient[1],  mat.ambient[2] };
		mcb->specular = { mat.specular[0], mat.specular[1], mat.specular[2] };
		mcb->specPower = mat.specularPower;
		mcb->sphereMode = mat.sphereMode;
		mcb->edgeSize = edgeSize;
		mcb->edgeColor = { mat.edgeColor[0], mat.edgeColor[1], mat.edgeColor[2], mat.edgeColor[3] };

		mcb->materialType = matType;
		mcb->rimMul = rimMul;
		mcb->specMul = specMul;
		mcb->shadowMul = shadowMul;
		mcb->toonContrastMul = toonContrastMul;
		mcb->alphaCutout = (gm.baseTextureHasTransparency && !gm.baseTextureHasTranslucency) ? 1.0f : 0.0f;
		mcb->textureFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mcb->sphereFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mcb->toonFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mcb->normalFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mcb->normalMapIntensity = lightSettings.normalMapEnabled ? lightSettings.normalMapIntensity : 0.0f;

		gm.rimMul = rimMul;
		gm.specMul = specMul;
		gm.shadowMul = shadowMul;
		gm.toonContrastMul = toonContrastMul;
		gm.materialType = matType;
		gm.normalMapIntensity = mcb->normalMapIntensity;

		m_pmx.materials.push_back(gm);
	}

	m_pmx.indexCount = static_cast<uint32_t>(inds.size());
	m_pmx.revision = model->Revision();
	m_pmx.ready = true;
	m_appliedLightSettings = lightSettings;
	m_hasAppliedLightSettings = true;
	++m_materialStateRevision;
}

void PmxModelDrawer::UpdateMaterialSettings(const LightSettings& lightSettings)
{
	if (!m_pmx.ready || !m_materialCbMapped) return;
	if (m_hasAppliedLightSettings && m_appliedLightSettings == lightSettings)
	{
		return;
	}

	for (size_t mi = 0; mi < m_pmx.materials.size(); ++mi)
	{
		auto& gm = m_pmx.materials[mi];

		uint32_t matType = GuessMaterialType(gm.mat);
		float rimMul, specMul, shadowMul, toonContrastMul;
		GetMaterialStyleParams(matType, rimMul, specMul, shadowMul, toonContrastMul);

		if (lightSettings.faceMaterialOverridesEnabled && LooksLikeFaceMaterial(gm.mat))
		{
			shadowMul = lightSettings.faceShadowMul;
			toonContrastMul = lightSettings.faceToonContrastMul;
		}

		MaterialCB* mcb = reinterpret_cast<MaterialCB*>(m_materialCbMapped + mi * m_materialCbStride);
		mcb->shadowMul = shadowMul;
		mcb->toonContrastMul = toonContrastMul;
		mcb->normalMapIntensity = lightSettings.normalMapEnabled ? lightSettings.normalMapIntensity : 0.0f;
		mcb->rimMul = rimMul;
		mcb->specMul = specMul;
		mcb->materialType = matType;
		m_pmx.materials[mi].rimMul = rimMul;
		m_pmx.materials[mi].specMul = specMul;
		m_pmx.materials[mi].shadowMul = shadowMul;
		m_pmx.materials[mi].toonContrastMul = toonContrastMul;
		m_pmx.materials[mi].materialType = matType;
		m_pmx.materials[mi].normalMapIntensity = mcb->normalMapIntensity;
	}

	m_appliedLightSettings = lightSettings;
	m_hasAppliedLightSettings = true;
}

void PmxModelDrawer::CollectMorphWeights(const MmdAnimator& animator, size_t morphCount)
{
	if (m_morphWeights.size() != morphCount)
	{
		m_morphWeights.resize(morphCount);
	}

	const auto& resolvedWeights = animator.ResolvedMorphWeights();
	if (resolvedWeights.size() == morphCount)
	{
		std::copy(resolvedWeights.begin(), resolvedWeights.end(), m_morphWeights.begin());
		return;
	}

	std::fill(m_morphWeights.begin(), m_morphWeights.end(), 0.0f);
}

bool PmxModelDrawer::MorphWeightsChanged() const noexcept
{
	return m_appliedMorphWeights.size() != m_morphWeights.size() ||
		!std::equal(
			m_morphWeights.begin(),
			m_morphWeights.end(),
			m_appliedMorphWeights.begin(),
			[](float lhs, float rhs) {
				return std::abs(lhs - rhs) <= 1e-5f;
			});
}

void PmxModelDrawer::ClassifyActiveMorphs(const std::vector<PmxModel::Morph>& morphs)
{
	m_activeVertexMorphIndices.clear();
	m_activeMaterialMorphIndices.clear();

	for (size_t index = 0; index < morphs.size(); ++index)
	{
		if (!HasMorphWeight(m_morphWeights[index]))
		{
			continue;
		}

		const auto type = morphs[index].type;
		if (type == PmxModel::Morph::Type::Vertex || IsUvMorphType(type))
		{
			m_activeVertexMorphIndices.push_back(index);
			continue;
		}

		if (type == PmxModel::Morph::Type::Material)
		{
			m_activeMaterialMorphIndices.push_back(index);
		}
	}
}

void PmxModelDrawer::ResetWorkingVertices()
{
	if (m_workingVertices.size() != m_baseVertices.size())
	{
		m_workingVertices = m_baseVertices;
		return;
	}

	std::memcpy(m_workingVertices.data(), m_baseVertices.data(), m_baseVertices.size() * sizeof(PmxVsVertex));
}

bool PmxModelDrawer::ApplyVertexMorphs(const std::vector<PmxModel::Morph>& morphs,
									   VertexDirtySet& vertexDirtySet,
									   VertexDirtySet& positionDirtySet)
{
	bool vertexDirty = false;

	for (size_t index : m_activeVertexMorphIndices)
	{
		const float weight = m_morphWeights[index];
		const auto& morph = morphs[index];
		if (morph.type == PmxModel::Morph::Type::Vertex)
		{
			vertexDirty = true;
			for (const auto& offset : morph.vertexOffsets)
			{
				if (offset.vertexIndex >= m_workingVertices.size())
				{
					continue;
				}

				auto& vertex = m_workingVertices[offset.vertexIndex];
				vertex.px += offset.positionOffset.x * weight;
				vertex.py += offset.positionOffset.y * weight;
				vertex.pz += offset.positionOffset.z * weight;
				vertexDirtySet.Mark(offset.vertexIndex);
				positionDirtySet.Mark(offset.vertexIndex);
			}
			continue;
		}

		if (!IsUvMorphType(morph.type))
		{
			continue;
		}

		vertexDirty = true;
		for (const auto& offset : morph.uvOffsets)
		{
			if (offset.vertexIndex >= m_workingVertices.size())
			{
				continue;
			}

			auto& vertex = m_workingVertices[offset.vertexIndex];
			if (offset.channel == 0)
			{
				vertex.u += offset.offset.x * weight;
				vertex.v += offset.offset.y * weight;
				vertexDirtySet.Mark(offset.vertexIndex);
				continue;
			}

			float* target = SelectAdditionalUvTarget(vertex, offset.channel);
			if (!target)
			{
				continue;
			}

			target[0] += offset.offset.x * weight;
			target[1] += offset.offset.y * weight;
			target[2] += offset.offset.z * weight;
			target[3] += offset.offset.w * weight;
			vertexDirtySet.Mark(offset.vertexIndex);
		}
	}

	return vertexDirty;
}

bool PmxModelDrawer::ApplySoftBodyOverrides(const MmdAnimator& animator,
											VertexDirtySet& vertexDirtySet,
											VertexDirtySet& positionDirtySet)
{
	using namespace DirectX;

	const auto& overridePositions = animator.SoftBodyVertexPositions();
	const auto& overrideMask = animator.SoftBodyVertexMask();
	const auto& activeVertexIndices = animator.SoftBodyActiveVertexIndices();
	const auto& skinningMatrices = animator.GetSkinningMatrices();
	if (overridePositions.size() != m_workingVertices.size() ||
		overrideMask.size() != m_workingVertices.size() ||
		skinningMatrices.empty() ||
		activeVertexIndices.empty())
	{
		return false;
	}

	bool vertexDirty = false;
	for (const std::uint32_t activeVertexIndex : activeVertexIndices)
	{
		const size_t vertexIndex = static_cast<size_t>(activeVertexIndex);
		if (vertexIndex >= m_workingVertices.size())
		{
			continue;
		}

		if (overrideMask[vertexIndex] == 0)
		{
			continue;
		}

		XMMATRIX blendedSkinning{
			XMVectorZero(),
			XMVectorZero(),
			XMVectorZero(),
			XMVectorZero()
		};
		float totalWeight = 0.0f;
		const auto& vertex = m_workingVertices[vertexIndex];
		for (int influenceIndex = 0; influenceIndex < 4; ++influenceIndex)
		{
			const float weight = vertex.boneWeights[influenceIndex];
			const int32_t boneIndex = vertex.boneIndices[influenceIndex];
			if (weight <= 0.0f || boneIndex < 0 || boneIndex >= static_cast<int32_t>(skinningMatrices.size()))
			{
				continue;
			}

			const XMMATRIX skinning = XMLoadFloat4x4(&skinningMatrices[static_cast<size_t>(boneIndex)]);
			blendedSkinning.r[0] = XMVectorAdd(blendedSkinning.r[0], XMVectorScale(skinning.r[0], weight));
			blendedSkinning.r[1] = XMVectorAdd(blendedSkinning.r[1], XMVectorScale(skinning.r[1], weight));
			blendedSkinning.r[2] = XMVectorAdd(blendedSkinning.r[2], XMVectorScale(skinning.r[2], weight));
			blendedSkinning.r[3] = XMVectorAdd(blendedSkinning.r[3], XMVectorScale(skinning.r[3], weight));
			totalWeight += weight;
		}

		if (totalWeight <= 1.0e-5f)
		{
			continue;
		}

		XMVECTOR determinant = XMVectorZero();
		const XMMATRIX inverseSkinning = XMMatrixInverse(&determinant, blendedSkinning);
		const float det = XMVectorGetX(determinant);
		if (!std::isfinite(det) || std::abs(det) <= 1.0e-8f)
		{
			continue;
		}

		const XMVECTOR desiredPosition = XMLoadFloat3(&overridePositions[vertexIndex]);
		const XMVECTOR localPosition = XMVector3TransformCoord(desiredPosition, inverseSkinning);
		m_workingVertices[vertexIndex].px = XMVectorGetX(localPosition);
		m_workingVertices[vertexIndex].py = XMVectorGetY(localPosition);
		m_workingVertices[vertexIndex].pz = XMVectorGetZ(localPosition);
		vertexDirtySet.Mark(vertexIndex);
		positionDirtySet.Mark(vertexIndex);
		vertexDirty = true;
	}

	return vertexDirty;
}

void PmxModelDrawer::RecomputeWorkingNormals(const PmxModel& model,
											 const VertexDirtySet& positionDirtySet,
											 VertexDirtySet& vertexDirtySet)
{
	using namespace DirectX;

	if (positionDirtySet.Empty())
	{
		return;
	}

	const auto& indices = model.Indices();
	const size_t vertexCount = m_workingVertices.size();

	auto recomputeAllNormals = [&]()
	{
		for (auto& vertex : m_workingVertices)
		{
			vertex.nx = 0.0f;
			vertex.ny = 0.0f;
			vertex.nz = 0.0f;
		}

		for (size_t index = 0; index + 2 < indices.size(); index += 3)
		{
			const uint32_t ia = indices[index];
			const uint32_t ib = indices[index + 1];
			const uint32_t ic = indices[index + 2];
			if (ia >= m_workingVertices.size() ||
				ib >= m_workingVertices.size() ||
				ic >= m_workingVertices.size())
			{
				continue;
			}

			const auto& va = m_workingVertices[ia];
			const auto& vb = m_workingVertices[ib];
			const auto& vc = m_workingVertices[ic];
			const XMVECTOR pa = XMVectorSet(va.px, va.py, va.pz, 1.0f);
			const XMVECTOR pb = XMVectorSet(vb.px, vb.py, vb.pz, 1.0f);
			const XMVECTOR pc = XMVectorSet(vc.px, vc.py, vc.pz, 1.0f);
			const XMVECTOR normal = XMVector3Cross(XMVectorSubtract(pb, pa), XMVectorSubtract(pc, pa));
			const float normalLengthSq = XMVectorGetX(XMVector3LengthSq(normal));
			if (!std::isfinite(normalLengthSq) || normalLengthSq <= 1.0e-10f)
			{
				continue;
			}

			auto accumulateNormal = [&](PmxVsVertex& vertex)
			{
				vertex.nx += XMVectorGetX(normal);
				vertex.ny += XMVectorGetY(normal);
				vertex.nz += XMVectorGetZ(normal);
			};
			accumulateNormal(m_workingVertices[ia]);
			accumulateNormal(m_workingVertices[ib]);
			accumulateNormal(m_workingVertices[ic]);
		}

		for (size_t vertexIndex = 0; vertexIndex < m_workingVertices.size(); ++vertexIndex)
		{
			auto& vertex = m_workingVertices[vertexIndex];
			const XMVECTOR accumulated = XMVectorSet(vertex.nx, vertex.ny, vertex.nz, 0.0f);
			const float normalLengthSq = XMVectorGetX(XMVector3LengthSq(accumulated));
			if (!std::isfinite(normalLengthSq) || normalLengthSq <= 1.0e-10f)
			{
				vertex.nx = m_baseVertices[vertexIndex].nx;
				vertex.ny = m_baseVertices[vertexIndex].ny;
				vertex.nz = m_baseVertices[vertexIndex].nz;
			}
			else
			{
				const XMVECTOR normalized = XMVector3Normalize(accumulated);
				vertex.nx = XMVectorGetX(normalized);
				vertex.ny = XMVectorGetY(normalized);
				vertex.nz = XMVectorGetZ(normalized);
			}
			vertexDirtySet.Mark(vertexIndex);
		}
	};

	const bool hasAdjacency =
		(m_vertexTriangleOffsets.size() == vertexCount + 1) &&
		(m_vertexTriangleOffsets.empty() || m_vertexTriangleOffsets.back() == m_vertexTriangleIndices.size());
	if (!hasAdjacency)
	{
		recomputeAllNormals();
		return;
	}

	if (positionDirtySet.indices.size() * 4 >= vertexCount)
	{
		recomputeAllNormals();
		return;
	}

	m_affectedNormalVerticesScratch.Reset(vertexCount);
	for (const size_t changedVertex : positionDirtySet.indices)
	{
		if (changedVertex >= vertexCount)
		{
			continue;
		}

		const uint32_t begin = m_vertexTriangleOffsets[changedVertex];
		const uint32_t end = m_vertexTriangleOffsets[changedVertex + 1];
		for (uint32_t cursor = begin; cursor < end; ++cursor)
		{
			const size_t triangleStart = static_cast<size_t>(m_vertexTriangleIndices[cursor]);
			if (triangleStart + 2 >= indices.size())
			{
				continue;
			}

			const uint32_t ia = indices[triangleStart];
			const uint32_t ib = indices[triangleStart + 1];
			const uint32_t ic = indices[triangleStart + 2];
			m_affectedNormalVerticesScratch.Mark(ia);
			m_affectedNormalVerticesScratch.Mark(ib);
			m_affectedNormalVerticesScratch.Mark(ic);
		}
	}

	for (const size_t vertexIndex : m_affectedNormalVerticesScratch.indices)
	{
		if (vertexIndex >= vertexCount)
		{
			continue;
		}

		XMVECTOR accumulated = XMVectorZero();
		const uint32_t begin = m_vertexTriangleOffsets[vertexIndex];
		const uint32_t end = m_vertexTriangleOffsets[vertexIndex + 1];
		for (uint32_t cursor = begin; cursor < end; ++cursor)
		{
			const size_t triangleStart = static_cast<size_t>(m_vertexTriangleIndices[cursor]);
			if (triangleStart + 2 >= indices.size())
			{
				continue;
			}

			const uint32_t ia = indices[triangleStart];
			const uint32_t ib = indices[triangleStart + 1];
			const uint32_t ic = indices[triangleStart + 2];
			if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
			{
				continue;
			}

			const auto& va = m_workingVertices[ia];
			const auto& vb = m_workingVertices[ib];
			const auto& vc = m_workingVertices[ic];
			const XMVECTOR pa = XMVectorSet(va.px, va.py, va.pz, 1.0f);
			const XMVECTOR pb = XMVectorSet(vb.px, vb.py, vb.pz, 1.0f);
			const XMVECTOR pc = XMVectorSet(vc.px, vc.py, vc.pz, 1.0f);
			const XMVECTOR normal = XMVector3Cross(XMVectorSubtract(pb, pa), XMVectorSubtract(pc, pa));
			const float normalLengthSq = XMVectorGetX(XMVector3LengthSq(normal));
			if (!std::isfinite(normalLengthSq) || normalLengthSq <= 1.0e-10f)
			{
				continue;
			}

			accumulated = XMVectorAdd(accumulated, normal);
		}

		auto& vertex = m_workingVertices[vertexIndex];
		const float accumulatedLengthSq = XMVectorGetX(XMVector3LengthSq(accumulated));
		if (!std::isfinite(accumulatedLengthSq) || accumulatedLengthSq <= 1.0e-10f)
		{
			vertex.nx = m_baseVertices[vertexIndex].nx;
			vertex.ny = m_baseVertices[vertexIndex].ny;
			vertex.nz = m_baseVertices[vertexIndex].nz;
		}
		else
		{
			const XMVECTOR normalized = XMVector3Normalize(accumulated);
			vertex.nx = XMVectorGetX(normalized);
			vertex.ny = XMVectorGetY(normalized);
			vertex.nz = XMVectorGetZ(normalized);
		}

		vertexDirtySet.Mark(vertexIndex);
	}
}

void PmxModelDrawer::UploadWorkingVertices(const VertexDirtySet& vertexDirtySet)
{
	if (!m_pmx.vb || !m_vertexUpload[m_currentUploadBuffer] || !m_vertexUploadMapped[m_currentUploadBuffer] || vertexDirtySet.Empty())
	{
		return;
	}

	m_sortedDirtyVerticesScratch = vertexDirtySet.indices;
	std::sort(m_sortedDirtyVerticesScratch.begin(), m_sortedDirtyVerticesScratch.end());

	size_t spanFirst = m_sortedDirtyVerticesScratch.front();
	size_t spanLast = spanFirst;
	auto flushSpan = [&](size_t firstVertex, size_t lastVertex)
	{
		if (lastVertex < firstVertex)
		{
			return;
		}

		const UINT64 vertexStride = static_cast<UINT64>(sizeof(PmxVsVertex));
		const UINT64 byteOffset = static_cast<UINT64>(firstVertex) * vertexStride;
		const UINT64 byteCount = (static_cast<UINT64>(lastVertex) - static_cast<UINT64>(firstVertex) + 1u) * vertexStride;
		if (byteOffset >= m_vertexBufferSizeBytes || byteCount == 0)
		{
			return;
		}

		const UINT64 copyByteCount = std::min(byteCount, m_vertexBufferSizeBytes - byteOffset);
		std::memcpy(
			m_vertexUploadMapped[m_currentUploadBuffer] + byteOffset,
			m_workingVertices.data() + firstVertex,
			static_cast<size_t>(copyByteCount));
		m_pendingVertexUploadRanges.emplace_back(byteOffset, copyByteCount);
	};

	for (size_t i = 1; i < m_sortedDirtyVerticesScratch.size(); ++i)
	{
		const size_t current = m_sortedDirtyVerticesScratch[i];
		if (current <= spanLast + 1)
		{
			spanLast = current;
			continue;
		}

		flushSpan(spanFirst, spanLast);
		spanFirst = current;
		spanLast = current;
	}

	flushSpan(spanFirst, spanLast);
}

void PmxModelDrawer::ResetMaterialConstants(size_t materialIndex)
{
	const auto& material = m_pmx.materials[materialIndex];
	auto* cb = reinterpret_cast<MaterialCB*>(m_materialCbMapped + materialIndex * m_materialCbStride);

	cb->diffuse = { material.mat.diffuse[0], material.mat.diffuse[1], material.mat.diffuse[2], material.mat.diffuse[3] };
	cb->specular = { material.mat.specular[0], material.mat.specular[1], material.mat.specular[2] };
	cb->specPower = material.mat.specularPower;
	cb->ambient = { material.mat.ambient[0], material.mat.ambient[1], material.mat.ambient[2] };
	cb->edgeColor = { material.mat.edgeColor[0], material.mat.edgeColor[1], material.mat.edgeColor[2], material.mat.edgeColor[3] };
	cb->edgeSize = material.mat.edgeSize;
	cb->alphaCutout = (material.baseTextureHasTransparency && !material.baseTextureHasTranslucency) ? 1.0f : 0.0f;
	cb->textureFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
	cb->sphereFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
	cb->toonFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
	cb->normalFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
	cb->sphereMode = material.mat.sphereMode;
	cb->rimMul = material.rimMul;
	cb->specMul = material.specMul;
	cb->shadowMul = material.shadowMul;
	cb->toonContrastMul = material.toonContrastMul;
	cb->materialType = material.materialType;
	cb->normalMapIntensity = material.normalMapIntensity;
}

void PmxModelDrawer::ApplyMaterialOffset(MaterialCB& cb,
										 const PmxModel::Morph::MaterialOffset& offset,
										 float weight)
{
	if (offset.operation == 0)
	{
		ApplyMultiply(cb.diffuse, offset.diffuse, weight);
		ApplyMultiply(cb.specular, offset.specular, weight);
		ApplyMultiply(cb.specPower, offset.specularPower, weight);
		ApplyMultiply(cb.ambient, offset.ambient, weight);
		ApplyMultiply(cb.edgeColor, offset.edgeColor, weight);
		ApplyMultiply(cb.edgeSize, offset.edgeSize, weight);
		ApplyMultiply(cb.textureFactor, offset.textureFactor, weight);
		ApplyMultiply(cb.sphereFactor, offset.sphereTextureFactor, weight);
		ApplyMultiply(cb.toonFactor, offset.toonTextureFactor, weight);
		return;
	}

	if (offset.operation == 1)
	{
		ApplyAdd(cb.diffuse, offset.diffuse, weight);
		ApplyAdd(cb.specular, offset.specular, weight);
		ApplyAdd(cb.specPower, offset.specularPower, weight);
		ApplyAdd(cb.ambient, offset.ambient, weight);
		ApplyAdd(cb.edgeColor, offset.edgeColor, weight);
		ApplyAdd(cb.edgeSize, offset.edgeSize, weight);
		ApplyAdd(cb.textureFactor, offset.textureFactor, weight);
		ApplyAdd(cb.sphereFactor, offset.sphereTextureFactor, weight);
		ApplyAdd(cb.toonFactor, offset.toonTextureFactor, weight);
	}
}

void PmxModelDrawer::ApplyMaterialMorphs(const std::vector<PmxModel::Morph>& morphs)
{
	if (m_pmx.materials.empty() || !m_materialCbMapped)
	{
		return;
	}

	auto materialCbAt = [this](size_t materialIndex) -> MaterialCB* {
		return reinterpret_cast<MaterialCB*>(m_materialCbMapped + materialIndex * m_materialCbStride);
	};

	for (size_t materialIndex = 0; materialIndex < m_pmx.materials.size(); ++materialIndex)
	{
		ResetMaterialConstants(materialIndex);
	}

	for (size_t morphIndex : m_activeMaterialMorphIndices)
	{
		const float weight = m_morphWeights[morphIndex];
		const auto& morph = morphs[morphIndex];
		for (const auto& offset : morph.materialOffsets)
		{
			if (offset.materialIndex == -1)
			{
				for (size_t materialIndex = 0; materialIndex < m_pmx.materials.size(); ++materialIndex)
				{
					ApplyMaterialOffset(*materialCbAt(materialIndex), offset, weight);
				}
				continue;
			}

			if (offset.materialIndex < 0 ||
				offset.materialIndex >= static_cast<std::int32_t>(m_pmx.materials.size()))
			{
				continue;
			}

			ApplyMaterialOffset(*materialCbAt(static_cast<size_t>(offset.materialIndex)), offset, weight);
		}
	}

	for (size_t materialIndex = 0; materialIndex < m_pmx.materials.size(); ++materialIndex)
	{
		const auto& material = m_pmx.materials[materialIndex];
		auto* cb = materialCbAt(materialIndex);
		ClampMaterialConstants(*cb);
		cb->alphaCutout =
			(material.baseTextureHasTransparency && !material.baseTextureHasTranslucency &&
			 (cb->diffuse.w * cb->textureFactor.w) >= 0.999f)
			? 1.0f
			: 0.0f;
	}
}

void PmxModelDrawer::UpdatePmxMorphs(const MmdAnimator& animator)
{
	if (!m_pmx.ready) return;
	const auto* model = animator.Model();
	if (!model) return;

	const auto& morphs = model->Morphs();
	const bool hasSoftBodyOverrides = animator.HasSoftBodyVertexOverrides();
	if (morphs.empty() &&
		!hasSoftBodyOverrides &&
		!m_hasAppliedVertexOverrides &&
		!m_hasAppliedMaterialMorphs)
	{
		return;
	}

	CollectMorphWeights(animator, morphs.size());
	ClassifyActiveMorphs(morphs);

	auto relevantMorphWeightsChanged =
		[&](auto&& predicate)
	{
		if (m_appliedMorphWeights.size() != m_morphWeights.size())
		{
			return true;
		}

		for (size_t index = 0; index < morphs.size(); ++index)
		{
			if (!predicate(morphs[index].type))
			{
				continue;
			}

			if (std::abs(m_morphWeights[index] - m_appliedMorphWeights[index]) > kMorphWeightThreshold)
			{
				return true;
			}
		}

		return false;
	};

	const bool vertexMorphWeightsChanged = relevantMorphWeightsChanged(
		[](PmxModel::Morph::Type type) noexcept
		{
			return type == PmxModel::Morph::Type::Vertex || IsUvMorphType(type);
		});
	const bool materialMorphWeightsChanged = relevantMorphWeightsChanged(
		[](PmxModel::Morph::Type type) noexcept
		{
			return type == PmxModel::Morph::Type::Material;
		});
	const bool hasActiveVertexMorphs = !m_activeVertexMorphIndices.empty();
	const bool hasActiveVertexOverrides = hasSoftBodyOverrides || hasActiveVertexMorphs;
	const bool needsVertexReset = !hasActiveVertexOverrides && m_hasAppliedVertexOverrides;
	const bool needsVertexUpdate =
		hasSoftBodyOverrides ||
		(hasActiveVertexMorphs && vertexMorphWeightsChanged) ||
		needsVertexReset;
	const bool hasActiveMaterialMorphs = !m_activeMaterialMorphIndices.empty();
	const bool needsMaterialUpdate =
		(hasActiveMaterialMorphs && materialMorphWeightsChanged) ||
		(!hasActiveMaterialMorphs && m_hasAppliedMaterialMorphs);

	if (!needsVertexUpdate && !needsMaterialUpdate)
	{
		if (m_appliedMorphWeights.size() != m_morphWeights.size())
		{
			m_appliedMorphWeights = m_morphWeights;
		}
		return;
	}

	if (needsVertexUpdate && m_baseVertices.empty())
	{
		return;
	}

	if (needsVertexUpdate)
	{
		ResetWorkingVertices();
		const size_t vertexCount = m_workingVertices.size();
		m_vertexDirtyScratch.Reset(vertexCount);
		m_positionDirtyScratch.Reset(vertexCount);
		bool vertexDirty = false;

		if (needsVertexReset)
		{
			for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
			{
				m_vertexDirtyScratch.Mark(vertexIndex);
			}
			vertexDirty = (vertexCount > 0);
		}
		else
		{
			vertexDirty = ApplyVertexMorphs(morphs, m_vertexDirtyScratch, m_positionDirtyScratch);
			if (hasSoftBodyOverrides && ApplySoftBodyOverrides(animator, m_vertexDirtyScratch, m_positionDirtyScratch))
			{
				vertexDirty = true;
			}
		}

		if (!m_positionDirtyScratch.Empty())
		{
			RecomputeWorkingNormals(*model, m_positionDirtyScratch, m_vertexDirtyScratch);
		}
		if (vertexDirty)
		{
			UploadWorkingVertices(m_vertexDirtyScratch);
		}

		m_hasAppliedVertexOverrides = hasActiveVertexOverrides;
	}

	bool materialUpdated = false;
	if (needsMaterialUpdate)
	{
		ApplyMaterialMorphs(morphs);
		m_hasAppliedMaterialMorphs = hasActiveMaterialMorphs;
		materialUpdated = true;
	}

	if (m_appliedMorphWeights.size() != m_morphWeights.size() ||
		vertexMorphWeightsChanged ||
		materialMorphWeightsChanged)
	{
		m_appliedMorphWeights = m_morphWeights;
	}

	if (materialUpdated)
	{
		++m_materialStateRevision;
	}
}

void PmxModelDrawer::CommitGpuUploads(ID3D12GraphicsCommandList* cmdList)
{
	if (!cmdList)
	{
		return;
	}

	if (m_indexUploadPending && m_pmx.ib && m_indexUpload && m_indexBufferSizeBytes > 0)
	{
		if (m_indexBufferState != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_pmx.ib.get(),
				m_indexBufferState,
				D3D12_RESOURCE_STATE_COPY_DEST);
			cmdList->ResourceBarrier(1, &barrier);
			m_indexBufferState = D3D12_RESOURCE_STATE_COPY_DEST;
		}

		cmdList->CopyBufferRegion(
			m_pmx.ib.get(),
			0,
			m_indexUpload.get(),
			0,
			m_indexBufferSizeBytes);

		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_pmx.ib.get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_INDEX_BUFFER);
		cmdList->ResourceBarrier(1, &barrier);
		m_indexBufferState = D3D12_RESOURCE_STATE_INDEX_BUFFER;
		m_indexUploadPending = false;
	}

	if (!m_pendingVertexUploadRanges.empty() && m_pmx.vb && m_vertexUpload[m_currentUploadBuffer])
	{
		if (m_vertexBufferState != D3D12_RESOURCE_STATE_COPY_DEST)
		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_pmx.vb.get(),
				m_vertexBufferState,
				D3D12_RESOURCE_STATE_COPY_DEST);
			cmdList->ResourceBarrier(1, &barrier);
			m_vertexBufferState = D3D12_RESOURCE_STATE_COPY_DEST;
		}

		for (const auto& [byteOffset, byteCount] : m_pendingVertexUploadRanges)
		{
			if (byteCount == 0 || byteOffset >= m_vertexBufferSizeBytes)
			{
				continue;
			}

			const UINT64 clampedByteCount = std::min(byteCount, m_vertexBufferSizeBytes - byteOffset);
			cmdList->CopyBufferRegion(
				m_pmx.vb.get(),
				byteOffset,
				m_vertexUpload[m_currentUploadBuffer].get(),
				byteOffset,
				clampedByteCount);
		}

		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_pmx.vb.get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		cmdList->ResourceBarrier(1, &barrier);
		m_vertexBufferState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		m_pendingVertexUploadRanges.clear();
		m_currentUploadBuffer ^= 1;
	}
}

void PmxModelDrawer::UpdateBoneMatrices(const MmdAnimator& animator, BoneCB* dst)
{
	if (!dst) return;

	if (animator.HasSkinnedPose())
	{
		const auto& matrices = animator.GetSkinningMatrices();
		size_t count = std::min(matrices.size(), static_cast<size_t>(MaxBones));

		for (size_t i = 0; i < count; ++i)
		{
			DirectX::XMMATRIX mat = DirectX::XMLoadFloat4x4(&matrices[i]);
			DirectX::XMStoreFloat4x4(&dst->boneMatrices[i], DirectX::XMMatrixTranspose(mat));
		}

		for (size_t i = count; i < MaxBones; ++i)
		{
			DirectX::XMStoreFloat4x4(&dst->boneMatrices[i], DirectX::XMMatrixIdentity());
		}
	}
	else
	{
		for (size_t i = 0; i < MaxBones; ++i)
		{
			DirectX::XMStoreFloat4x4(&dst->boneMatrices[i], DirectX::XMMatrixIdentity());
		}
	}
}
