#pragma once
#include <windows.h>
#include <dcomp.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <DirectXMath.h>
#include <atomic>

#include "Dx12Context.hpp"
#include "GpuResourceManager.hpp"
#include "RenderPipelineManager.hpp"
#include "PmxModelDrawer.hpp"
#include "Camera.hpp"
#include "MmdAnimator.hpp"
#include "PmxModel.hpp"
#include "Settings.hpp"

class DcompRenderer
{
public:
	DcompRenderer() = default;
	~DcompRenderer();

	DcompRenderer(const DcompRenderer&) = delete;
	DcompRenderer& operator=(const DcompRenderer&) = delete;

	using ProgressCallback = std::function<void(float, const wchar_t*)>;

	void Initialize(HWND hwnd, ProgressCallback progress = {});
	void Render(const MmdAnimator& animator);

	void SetLightSettings(const LightSettings& light);
	void SetVSyncEnabled(bool enabled);
	bool IsVSyncEnabled() const
	{
		return m_vsyncEnabled;
	}
	const LightSettings& GetLightSettings() const
	{
		return m_lightSettings;
	}
	UINT GetMaximumSupportedMsaaSampleCount() const
	{
		return m_maxMsaaSampleCount;
	}

	void AdjustBrightness(float delta);
	static constexpr UINT FrameCount = 3;
	static constexpr size_t MaxBones = 1024;

	void AdjustScale(float delta);
	void AddCameraRotation(float dxPixels, float dyPixels);

	// 3D空間(Model Local)の座標をスクリーン(クライアント)座標に変換する
	DirectX::XMFLOAT3 ProjectToScreen(const DirectX::XMFLOAT3& localPos) const;

	void LoadTexturesForModel(const PmxModel* model,
							  std::function<void(float, const wchar_t*)> onProgress,
							  float startProgress, float endProgress);

	void SetResizeOverlayEnabled(bool enabled);
private:
	void CreateD3D();
	void CreateDirectCompositionTree();
	void CreateSwapChain();
	void CreateCommandObjects();
	void CreateRenderTargets();
	void WaitForGpu();
	void ResizeIfNeeded();

    HWND m_hwnd{};
    Dx12Context m_ctx;

    LightSettings m_lightSettings;

    Camera m_camera;
    RenderPipelineManager m_pipeline;
    GpuResourceManager m_gpuResources;
    PmxModelDrawer m_pmxDrawer;

    winrt::com_ptr<IDXGISwapChain1> m_swapChain1;
    winrt::com_ptr<IDXGISwapChain3> m_swapChain;
    winrt::com_ptr<IDCompositionDevice> m_dcompDevice;
    winrt::com_ptr<IDCompositionTarget> m_dcompTarget;
    winrt::com_ptr<IDCompositionVisual> m_dcompVisual;

    winrt::com_ptr<ID3D12CommandAllocator> m_alloc[FrameCount];
    winrt::com_ptr<ID3D12GraphicsCommandList> m_cmdList;
    winrt::com_ptr<ID3D12Fence> m_fence;
    std::atomic<UINT64> m_fenceValue{};
    HANDLE m_fenceEvent{};

    winrt::com_ptr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvDescriptorSize{};
    winrt::com_ptr<ID3D12Resource> m_renderTargets[FrameCount];

    UINT m_width{};
    UINT m_height{};

    ProgressCallback m_progressCallback;

    #pragma warning(push)
    #pragma warning(disable: 4324)
    struct alignas(16) SceneCB
    {
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
        DirectX::XMFLOAT4X4 mvp;

        DirectX::XMFLOAT3 lightDir0; float ambient;
        DirectX::XMFLOAT3 lightColor0; float lightInt0;

        DirectX::XMFLOAT3 lightDir1; float lightInt1;
        DirectX::XMFLOAT3 lightColor1; float _pad1;

        DirectX::XMFLOAT3 cameraPos; float specPower;
        DirectX::XMFLOAT3 specColor; float specStrength;

        DirectX::XMFLOAT4 normalMatrixRow0;
        DirectX::XMFLOAT4 normalMatrixRow1;
        DirectX::XMFLOAT4 normalMatrixRow2;
        DirectX::XMFLOAT4X4 shadowMatrix;

        float brightness{ 1.3f };
        uint32_t enableSkinning{ 0 };
        float toonContrast{ 1.15f };
        float shadowHueShift{ -0.1396f };

        float shadowSaturation{ 0.25f };
        float rimWidth{ 0.6f };
        float rimIntensity{ 0.35f };
        float specularStep{ 0.3f };

        uint32_t enableToon{ 1 };

        float outlineRefDistance{ 2.5f };
        float outlineDistanceScale{ 1.0f };
        float outlineDistancePower{ 0.8f };
        float shadowRampShift{ 0.0f };

        float shadowDeepThreshold;
        float shadowDeepSoftness;
        float shadowDeepMul;
        float globalSaturation;

        DirectX::XMFLOAT2 invScreenSize{ 0.0f, 0.0f };
        float shadowMapInvSize{ 0.0f };
        float shadowStrength{ 0.0f };

        uint32_t enableSelfShadow{ 0 };
        float shadowBias{ 0.0015f };
        float outlineWidthScale{ 1.0f };
        float outlineOpacityScale{ 1.0f };
    };
    #pragma warning(pop)

    winrt::com_ptr<ID3D12Resource> m_sceneCb[FrameCount];
    SceneCB* m_sceneCbMapped[FrameCount] = {};

    winrt::com_ptr<ID3D12Resource> m_boneCb[FrameCount];
    PmxModelDrawer::BoneCB* m_boneCbMapped[FrameCount] = {};

    winrt::com_ptr<ID3D12DescriptorHeap> m_dsvHeap;
    winrt::com_ptr<ID3D12Resource> m_depth;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle{};

	void CreateDepthBuffer();
	void CreateSceneBuffers();
	void CreateBoneBuffers();
	void CreateShadowResources();
	void UpdateRenderModeResources();
	UINT GetRequestedMsaaSampleCount() const;
	UINT GetActiveShadowMapSize() const;

    UINT64 m_frameFenceValues[FrameCount]{};

    void WaitForFrame(UINT frameIndex);

    UINT m_msaaSampleCount = 1;
    UINT m_msaaQuality = 0;
    UINT m_maxMsaaSampleCount = 1;
    UINT m_maxMsaaQuality = 0;

    winrt::com_ptr<ID3D12Resource> m_msaaColor;
    winrt::com_ptr<ID3D12DescriptorHeap> m_msaaRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_msaaRtvHandle{};

    void CreateMsaaTargets();

    D3D12_RESOURCE_STATES m_msaaColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    void SelectMaximumMsaa();

    winrt::com_ptr<ID3D12Resource> m_intermediateTex;

    winrt::com_ptr<ID3D12DescriptorHeap> m_intermediateRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_intermediateRtvHandle{};

    winrt::com_ptr<ID3D12DescriptorHeap> m_intermediateSrvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_intermediateSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_intermediateSrvGpuHandle{};
    D3D12_RESOURCE_STATES m_intermediateState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    void CreateIntermediateResources();
    void ReleaseIntermediateResources();
    void ReportProgress(float value, const wchar_t* msg);

    winrt::com_ptr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_shadowDsvHandle{};
    winrt::com_ptr<ID3D12Resource> m_shadowMap;
    D3D12_RESOURCE_STATES m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t m_shadowSrvIndex{};
    UINT m_shadowMapSize = 1024;
    bool m_shadowSrvAllocated{ false };

    struct SortedMaterialEntry
    {
        size_t index{};
        float distanceSq{};
    };

    std::vector<size_t> m_opaqueCull;
    std::vector<size_t> m_opaqueNoCull;
    std::vector<size_t> m_cutoutCull;
    std::vector<size_t> m_cutoutNoCull;
    std::vector<size_t> m_transCull;
    std::vector<size_t> m_transNoCull;
    std::vector<size_t> m_edgeDrawIndices;
    std::vector<SortedMaterialEntry> m_sortedTransparentEntries;
    DirectX::XMFLOAT3 m_lastTransparentSortEye{};
    DirectX::XMFLOAT4X4 m_lastTransparentSortModel{};
    uint64_t m_cachedPmxRevision{};
    uint64_t m_cachedMaterialStateRevision{};
    bool m_materialBucketsValid{ false };
    bool m_transparentSortValid{ false };

    bool m_resizeOverlayEnabled{ false };
	bool m_vsyncEnabled{ false };
};
