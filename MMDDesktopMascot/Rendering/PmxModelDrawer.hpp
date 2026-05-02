#pragma once

#include <algorithm>
#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <DirectXMath.h>

#include "Dx12Context.hpp"
#include "GpuResourceManager.hpp"
#include "MmdAnimator.hpp"
#include "PmxModel.hpp"
#include "Settings.hpp"

class PmxModelDrawer
{
public:
	static constexpr size_t MaxBones = 1024;

	struct PmxVsVertex
	{
		float px, py, pz;
		float nx, ny, nz;
		float u, v;
		int32_t boneIndices[4];
		float boneWeights[4];
		float sdefC[3];
		float sdefR0[3];
		float sdefR1[3];
		uint32_t weightType;
		float edgeScale;
		float addUv1[4];
		float addUv2[4];
		float addUv3[4];
		float addUv4[4];
		float tangent[4]; // XYZ + handedness W
	};

	struct alignas(16) MaterialCB
	{
		DirectX::XMFLOAT4 diffuse;
		DirectX::XMFLOAT3 ambient;     float _pad0{};
		DirectX::XMFLOAT3 specular;    float specPower{};

		uint32_t sphereMode{};
		float edgeSize{};
		float rimMul{ 1.0f };
		float specMul{ 1.0f };

		DirectX::XMFLOAT4 edgeColor;

		uint32_t materialType{ 0 };
		float shadowMul{ 1.0f };
		float toonContrastMul{ 1.0f };
		float alphaCutout{};

		DirectX::XMFLOAT4 textureFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 sphereFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 toonFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 normalFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float normalMapIntensity{ 1.0f };
		float _pad1[3]{};
	};

	struct alignas(16) BoneCB
	{
		DirectX::XMFLOAT4X4 boneMatrices[MaxBones];
	};

	struct PmxGpuMaterial
	{
		PmxModel::Material mat;
		uint32_t srvBlockIndex{};
		D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu{};
		bool baseTextureHasTransparency{};
		bool baseTextureHasTranslucency{};
		DirectX::XMFLOAT3 localCenter{};
		uint32_t normalMapSrv{};
		bool hasNormalMap{};
		float rimMul = 1.0f;
		float specMul = 1.0f;
		float shadowMul = 1.0f;
		float toonContrastMul = 1.0f;
		uint32_t materialType = 0;
		float normalMapIntensity = 1.0f;
	};

	struct PmxGpu
	{
		winrt::com_ptr<ID3D12Resource> vb;
		winrt::com_ptr<ID3D12Resource> ib;
		D3D12_VERTEX_BUFFER_VIEW vbv{};
		D3D12_INDEX_BUFFER_VIEW ibv{};
		std::vector<PmxGpuMaterial> materials;
		uint32_t indexCount{};
		uint64_t revision{};
		bool ready{ false };
	};

	void Initialize(Dx12Context* ctx, GpuResourceManager* resources);

	void EnsurePmxResources(const PmxModel* model, const LightSettings& lightSettings);
	void UpdatePmxMorphs(const MmdAnimator& animator);
	void CommitGpuUploads(ID3D12GraphicsCommandList* cmdList);
	void UpdateBoneMatrices(const MmdAnimator& animator, BoneCB* dst);
	void UpdateMaterialSettings(const LightSettings& lightSettings);

	const PmxGpu& GetPmx() const
	{
		return m_pmx;
	}
	bool IsReady() const
	{
		return m_pmx.ready;
	}

	ID3D12Resource* GetMaterialCb() const
	{
		return m_materialCb.get();
	}
	uint8_t* GetMaterialCbMapped() const
	{
		return m_materialCbMapped;
	}
	UINT64 GetMaterialCbStride() const
	{
		return m_materialCbStride;
	}
	uint64_t GetMaterialStateRevision() const
	{
		return m_materialStateRevision;
	}

private:
	struct VertexDirtySet
	{
		std::vector<uint8_t> mask;
		std::vector<size_t> indices;

		bool Empty() const noexcept
		{
			return indices.empty();
		}

		void Reset(size_t vertexCount)
		{
			if (mask.size() != vertexCount)
			{
				mask.assign(vertexCount, 0);
				indices.clear();
				return;
			}

			for (size_t index : indices)
			{
				mask[index] = 0;
			}
			indices.clear();
		}

		void Mark(size_t index)
		{
			if (index >= mask.size())
			{
				return;
			}
			if (mask[index] != 0)
			{
				return;
			}

			mask[index] = 1;
			indices.push_back(index);
		}
	};

	void CollectMorphWeights(const MmdAnimator& animator, size_t morphCount);
	void ClassifyActiveMorphs(const std::vector<PmxModel::Morph>& morphs);
	bool MorphWeightsChanged() const noexcept;
	void ResetWorkingVertices();
	bool ApplyVertexMorphs(const std::vector<PmxModel::Morph>& morphs,
						  VertexDirtySet& vertexDirtySet,
						  VertexDirtySet& positionDirtySet);
	bool ApplySoftBodyOverrides(const MmdAnimator& animator,
								VertexDirtySet& vertexDirtySet,
								VertexDirtySet& positionDirtySet);
	void RecomputeWorkingNormals(const PmxModel& model,
								 const VertexDirtySet& positionDirtySet,
								 VertexDirtySet& vertexDirtySet);
	void UploadWorkingVertices(const VertexDirtySet& vertexDirtySet);
	void ResetMaterialConstants(size_t materialIndex);
	void ApplyMaterialMorphs(const std::vector<PmxModel::Morph>& morphs);
	void ApplyMaterialOffset(MaterialCB& cb,
							 const PmxModel::Morph::MaterialOffset& offset,
							 float weight);

	Dx12Context* m_ctx{};
	GpuResourceManager* m_resources{};

	PmxGpu m_pmx;

	winrt::com_ptr<ID3D12Resource> m_materialCb;
	uint8_t* m_materialCbMapped = nullptr;
	UINT64 m_materialCbStride = 256;

	std::vector<PmxVsVertex> m_baseVertices;
	std::vector<PmxVsVertex> m_workingVertices;
	std::vector<uint32_t> m_vertexTriangleOffsets;
	std::vector<uint32_t> m_vertexTriangleIndices;
	VertexDirtySet m_vertexDirtyScratch;
	VertexDirtySet m_positionDirtyScratch;
	VertexDirtySet m_affectedNormalVerticesScratch;
	std::vector<size_t> m_sortedDirtyVerticesScratch;
	std::vector<float> m_morphWeights;
	std::vector<float> m_appliedMorphWeights;
	std::vector<size_t> m_activeVertexMorphIndices;
	std::vector<size_t> m_activeMaterialMorphIndices;
	winrt::com_ptr<ID3D12Resource> m_vertexUpload[2];
	uint8_t* m_vertexUploadMapped[2]{};
	int m_currentUploadBuffer = 0;
	winrt::com_ptr<ID3D12Resource> m_indexUpload;
	UINT64 m_vertexBufferSizeBytes{};
	UINT64 m_indexBufferSizeBytes{};
	D3D12_RESOURCE_STATES m_vertexBufferState{ D3D12_RESOURCE_STATE_COPY_DEST };
	D3D12_RESOURCE_STATES m_indexBufferState{ D3D12_RESOURCE_STATE_COPY_DEST };
	std::vector<std::pair<UINT64, UINT64>> m_pendingVertexUploadRanges;
	bool m_indexUploadPending{};
	LightSettings m_appliedLightSettings{};
	bool m_hasAppliedLightSettings{ false };
	bool m_hasAppliedVertexOverrides{ false };
	bool m_hasAppliedMaterialMorphs{ false };
	uint64_t m_materialStateRevision{ 1 };
};
