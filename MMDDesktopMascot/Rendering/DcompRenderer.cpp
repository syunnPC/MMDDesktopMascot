#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DcompRenderer.hpp"
#include "d3dx12.h"
#include "ExceptionHelper.hpp"
#include "DebugUtil.hpp"
#include "Win32UiUtil.hpp"
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <utility>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace
{
	constexpr DXGI_FORMAT kPresentRenderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	constexpr DXGI_FORMAT kSceneRenderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	bool UsesMsaaMode(int mode) noexcept
	{
		return mode == static_cast<int>(AntiAliasingMode::Msaa) ||
			   mode == static_cast<int>(AntiAliasingMode::MsaaFxaa);
	}

	bool UsesFxaaMode(int mode) noexcept
	{
		return mode == static_cast<int>(AntiAliasingMode::Fxaa) ||
			   mode == static_cast<int>(AntiAliasingMode::MsaaFxaa);
	}

	DXGI_SWAP_CHAIN_DESC1 MakeSwapChainDesc(UINT w, UINT h)
	{
		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = w;
		desc.Height = h;
		desc.Format = kPresentRenderTargetFormat;
		desc.Stereo = FALSE;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = DcompRenderer::FrameCount;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		return desc;
	}

	UINT64 Align256(UINT64 size)
	{
		return (size + 255ull) & ~255ull;
	}

	UINT NormalizeMsaaSampleCount(int requested) noexcept
	{
		if (requested >= 32) return 32;
		if (requested >= 16) return 16;
		if (requested >= 8) return 8;
		if (requested >= 4) return 4;
		if (requested >= 2) return 2;
		return 1;
	}

	UINT NormalizeShadowMapSize(int requested) noexcept
	{
		if (requested >= 2048) return 2048;
		if (requested >= 1024) return 1024;
		if (requested >= 512) return 512;
		return 256;
	}

	bool NearlyEqualFloat(float lhs, float rhs, float epsilon = 1e-4f) noexcept
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	bool NearlyEqualVector(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs, float epsilon = 1e-4f) noexcept
	{
		return NearlyEqualFloat(lhs.x, rhs.x, epsilon) &&
			   NearlyEqualFloat(lhs.y, rhs.y, epsilon) &&
			   NearlyEqualFloat(lhs.z, rhs.z, epsilon);
	}

	bool NearlyEqualMatrix(const DirectX::XMFLOAT4X4& lhs, const DirectX::XMFLOAT4X4& rhs, float epsilon = 1e-4f) noexcept
	{
		const float* lhsValues = &lhs._11;
		const float* rhsValues = &rhs._11;
		for (size_t i = 0; i < 16; ++i)
		{
			if (!NearlyEqualFloat(lhsValues[i], rhsValues[i], epsilon))
			{
				return false;
			}
		}
		return true;
	}

	float GetWindowDpiScale(HWND hwnd) noexcept
	{
		const UINT dpi = (hwnd && IsWindow(hwnd)) ? GetDpiForWindow(hwnd) : USER_DEFAULT_SCREEN_DPI;
		return (dpi > 0)
			? static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)
			: 1.0f;
	}

	DirectX::XMVECTOR SafeNormalizeDirection(DirectX::FXMVECTOR direction, DirectX::FXMVECTOR fallback) noexcept
	{
		const float lenSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction));
		if (!std::isfinite(lenSq) || lenSq <= 1.0e-10f)
		{
			return fallback;
		}

		return DirectX::XMVector3Normalize(direction);
	}
}

void DcompRenderer::SetLightSettings(const LightSettings& light)
{
	const bool msaaModeChanged =
		UsesMsaaMode(m_lightSettings.antiAliasingMode) != UsesMsaaMode(light.antiAliasingMode) ||
		NormalizeMsaaSampleCount(m_lightSettings.msaaSampleCount) != NormalizeMsaaSampleCount(light.msaaSampleCount);
	const bool shadowResolutionChanged =
		NormalizeShadowMapSize(m_lightSettings.shadowMapSize) != NormalizeShadowMapSize(light.shadowMapSize);
	m_lightSettings = light;

	if ((msaaModeChanged || shadowResolutionChanged) && m_swapChain)
	{
		UpdateRenderModeResources();
	}

	m_pmxDrawer.UpdateMaterialSettings(m_lightSettings); // 設定変更時にマテリアルパラメータも更新
}

void DcompRenderer::SetVSyncEnabled(bool enabled)
{
	m_vsyncEnabled = enabled;
}

void DcompRenderer::SetResizeOverlayEnabled(bool enabled)
{
	m_resizeOverlayEnabled = enabled;
}

void DcompRenderer::AdjustBrightness(float delta)
{
	m_lightSettings.brightness += delta;
	m_lightSettings.brightness = std::clamp(m_lightSettings.brightness, 0.1f, 3.0f);
}

DcompRenderer::~DcompRenderer()
{
	if (m_ctx.Queue() && m_fence && m_fenceEvent)
	{
		WaitForGpu();
	}

	if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

void DcompRenderer::CreateD3D()
{
	m_ctx.Initialize();
}

void DcompRenderer::ReportProgress(float value, const wchar_t* msg)
{
	if (m_progressCallback)
	{
		if (value < 0.0f) value = 0.0f;
		if (value > 1.0f) value = 1.0f;
		m_progressCallback(value, msg);
	}
}

void DcompRenderer::Initialize(HWND hwnd, ProgressCallback progress)
{
	m_hwnd = hwnd;
	m_progressCallback = std::move(progress);

	const SIZE clientSize = Win32UiUtil::GetClientSize(m_hwnd);
	m_width = static_cast<UINT>(clientSize.cx);
	m_height = static_cast<UINT>(clientSize.cy);

	ReportProgress(0.05f, L"Direct3D を初期化しています...");
	CreateD3D();

	m_pipeline.Initialize(&m_ctx);
	m_gpuResources.Initialize(&m_ctx, [this]() { WaitForGpu(); }, FrameCount);
	m_pmxDrawer.Initialize(&m_ctx, &m_gpuResources);

	CreateCommandObjects();

	ReportProgress(0.15f, L"コマンドリストを準備しています...");
	m_gpuResources.CreateUploadObjects();

	CreateSwapChain();
	CreateRenderTargets();
	CreateDirectCompositionTree();

	ReportProgress(0.30f, L"テクスチャ用のリソースを初期化しています...");
	m_gpuResources.CreateSrvHeap();
	m_gpuResources.ResetTextureCache();

	m_pipeline.CreatePmxRootSignature();

	ReportProgress(0.55f, L"メインシェーダーをコンパイルしています...");
	UpdateRenderModeResources();
	ReportProgress(0.80f, L"シャドウシェーダーをコンパイルしています...");
	m_pipeline.CreateShadowPipeline();

	ReportProgress(0.90f, L"FXAAパイプラインを準備しています...");
	m_pipeline.CreateFxaaPipeline();

	CreateSceneBuffers();
	CreateBoneBuffers();

	ReportProgress(1.0f, L"初期化が完了しました。");
}

void DcompRenderer::ReleaseIntermediateResources()
{
	m_intermediateTex = nullptr;
	m_intermediateRtvHeap = nullptr;
	m_intermediateSrvHeap = nullptr;
	m_intermediateState = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void DcompRenderer::CreateIntermediateResources()
{
	ReleaseIntermediateResources();

	if (m_width == 0 || m_height == 0) return;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		kSceneRenderTargetFormat, m_width, m_height,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
	);

	D3D12_CLEAR_VALUE clear{};
	clear.Format = kSceneRenderTargetFormat;
	clear.Color[0] = 0.f; clear.Color[1] = 0.f; clear.Color[2] = 0.f; clear.Color[3] = 0.f;

	DX_CALL(m_ctx.Device()->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		IID_PPV_ARGS(m_intermediateTex.put())));
	m_intermediateState = D3D12_RESOURCE_STATE_RENDER_TARGET;

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
	rtvHeapDesc.NumDescriptors = 1;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	DX_CALL(m_ctx.Device()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m_intermediateRtvHeap.put())));
	m_intermediateRtvHandle = m_intermediateRtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_ctx.Device()->CreateRenderTargetView(m_intermediateTex.get(), nullptr, m_intermediateRtvHandle);

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
	srvHeapDesc.NumDescriptors = 1;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	DX_CALL(m_ctx.Device()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(m_intermediateSrvHeap.put())));

	m_intermediateSrvCpuHandle = m_intermediateSrvHeap->GetCPUDescriptorHandleForHeapStart();
	m_intermediateSrvGpuHandle = m_intermediateSrvHeap->GetGPUDescriptorHandleForHeapStart();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = kSceneRenderTargetFormat;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	m_ctx.Device()->CreateShaderResourceView(m_intermediateTex.get(), &srvDesc, m_intermediateSrvCpuHandle);
}

void DcompRenderer::CreateShadowResources()
{
	if (!m_shadowDsvHeap || !m_gpuResources.GetSrvHeap())
	{
		return;
	}

	m_shadowMap = nullptr;
	m_shadowMapSize = GetActiveShadowMapSize();

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_TYPELESS,
		m_shadowMapSize, m_shadowMapSize,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1.0f;
	clear.DepthStencil.Stencil = 0;

	DX_CALL(m_ctx.Device()->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
		IID_PPV_ARGS(m_shadowMap.put())));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	m_shadowDsvHandle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_ctx.Device()->CreateDepthStencilView(m_shadowMap.get(), &dsvDesc, m_shadowDsvHandle);

	if (!m_shadowSrvAllocated)
	{
		m_shadowSrvIndex = m_gpuResources.AllocSrvIndex();
		m_shadowSrvAllocated = true;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	m_ctx.Device()->CreateShaderResourceView(
		m_shadowMap.get(),
		&srvDesc,
		m_gpuResources.GetSrvCpuHandle(m_shadowSrvIndex));

	m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

void DcompRenderer::UpdateRenderModeResources()
{
	if (!m_swapChain)
	{
		return;
	}

	WaitForGpu();
	CreateMsaaTargets();
	CreateIntermediateResources();
	CreateShadowResources();
	m_pipeline.CreatePmxPipeline(m_msaaSampleCount, m_msaaQuality);
	m_pipeline.CreateEdgePipeline(m_msaaSampleCount, m_msaaQuality);
}

void DcompRenderer::CreateSceneBuffers()
{
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(sizeof(SceneCB)));

	for (UINT i = 0; i < FrameCount; ++i)
	{
		DX_CALL(m_ctx.Device()->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_sceneCb[i].put())));

		void* mapped = nullptr;
		CD3DX12_RANGE range(0, 0);
		DX_CALL(m_sceneCb[i]->Map(0, &range, &mapped));
		m_sceneCbMapped[i] = reinterpret_cast<SceneCB*>(mapped);
		std::memset(m_sceneCbMapped[i], 0, sizeof(SceneCB));
	}
}

void DcompRenderer::CreateBoneBuffers()
{
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(sizeof(PmxModelDrawer::BoneCB)));

	for (UINT i = 0; i < FrameCount; ++i)
	{
		DX_CALL(m_ctx.Device()->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(m_boneCb[i].put())));

		void* mapped = nullptr;
		CD3DX12_RANGE range(0, 0);
		DX_CALL(m_boneCb[i]->Map(0, &range, &mapped));
		m_boneCbMapped[i] = reinterpret_cast<PmxModelDrawer::BoneCB*>(mapped);

		for (size_t b = 0; b < PmxModelDrawer::MaxBones; ++b)
		{
			DirectX::XMStoreFloat4x4(&m_boneCbMapped[i]->boneMatrices[b], DirectX::XMMatrixIdentity());
		}
	}
}

void DcompRenderer::CreateCommandObjects()
{
	for (UINT i = 0; i < FrameCount; ++i)
	{
		DX_CALL(m_ctx.Device()->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_alloc[i].put())));
	}

	DX_CALL(m_ctx.Device()->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].get(), nullptr,
		IID_PPV_ARGS(m_cmdList.put())));
	m_cmdList->Close();

	DX_CALL(m_ctx.Device()->CreateFence(
		0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));
	m_fenceValue = 1;

	m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!m_fenceEvent)
	{
		throw std::runtime_error("CreateEvent failed.");
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = FrameCount;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		DX_CALL(m_ctx.Device()->CreateDescriptorHeap(
			&heapDesc, IID_PPV_ARGS(m_rtvHeap.put())));
		m_rtvDescriptorSize = m_ctx.Device()->GetDescriptorHandleIncrementSize(
			D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
		dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvDesc.NumDescriptors = 1;
		dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		DX_CALL(m_ctx.Device()->CreateDescriptorHeap(
			&dsvDesc, IID_PPV_ARGS(m_dsvHeap.put())));
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
		dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvDesc.NumDescriptors = 1;
		dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		DX_CALL(m_ctx.Device()->CreateDescriptorHeap(
			&dsvDesc, IID_PPV_ARGS(m_shadowDsvHeap.put())));
	}
}

void DcompRenderer::CreateSwapChain()
{
	DXGI_SWAP_CHAIN_DESC1 desc = MakeSwapChainDesc(m_width, m_height);

	DX_CALL(m_ctx.Factory()->CreateSwapChainForComposition(
		m_ctx.Queue(), &desc, nullptr, m_swapChain1.put()));

	DX_CALL(m_swapChain1->QueryInterface(__uuidof(IDXGISwapChain3), m_swapChain.put_void()));
}

void DcompRenderer::CreateDirectCompositionTree()
{
	if (!m_hwnd || !m_swapChain)
	{
		throw std::runtime_error("DirectComposition requires a valid host window and swap chain.");
	}

	DX_CALL(DCompositionCreateDevice2(nullptr, __uuidof(IDCompositionDevice), m_dcompDevice.put_void()));

	DX_CALL(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.put()));
	DX_CALL(m_dcompDevice->CreateVisual(m_dcompVisual.put()));
	DX_CALL(m_dcompVisual->SetContent(m_swapChain.get()));
	DX_CALL(m_dcompTarget->SetRoot(m_dcompVisual.get()));
	DX_CALL(m_dcompDevice->Commit());
}

void DcompRenderer::CreateRenderTargets()
{
	auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < FrameCount; ++i)
	{
		DX_CALL(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_renderTargets[i].put())));
		m_ctx.Device()->CreateRenderTargetView(m_renderTargets[i].get(), nullptr, handle);
		handle.ptr += m_rtvDescriptorSize;
	}
}

void DcompRenderer::ResizeIfNeeded()
{
	const SIZE clientSize = Win32UiUtil::GetClientSize(m_hwnd);
	const UINT newW = static_cast<UINT>(clientSize.cx);
	const UINT newH = static_cast<UINT>(clientSize.cy);
	if (newW == m_width && newH == m_height) return;

	WaitForGpu();

	m_width = newW;
	m_height = newH;

	for (UINT i = 0; i < FrameCount; ++i)
	{
		m_renderTargets[i] = nullptr;
	}

	m_depth = nullptr;

	DX_CALL(m_swapChain->ResizeBuffers(
		FrameCount, m_width, m_height, kPresentRenderTargetFormat, 0));

	CreateRenderTargets();
	UpdateRenderModeResources();
}

void DcompRenderer::Render(const MmdAnimator& animator)
{
	const auto* model = animator.Model();
	if (!model) return;

	m_pmxDrawer.EnsurePmxResources(model, m_lightSettings);
	if (!m_pmxDrawer.IsReady())
	{
		return;
	}

	const auto& pmx = m_pmxDrawer.GetPmx();
	ResizeIfNeeded();

	auto ensureIntermediateResources = [&]() -> bool {
		if (!m_intermediateTex || !m_intermediateRtvHeap || !m_intermediateSrvHeap)
		{
			CreateIntermediateResources();
		}
		return (m_intermediateTex && m_intermediateRtvHeap && m_intermediateSrvHeap);
	};

	struct FrameTransform
	{
		DirectX::XMMATRIX model{};
		DirectX::XMMATRIX view{};
		DirectX::XMMATRIX proj{};
		DirectX::XMVECTOR eye{};
		float distance{ 0.0f };
		float minx{ 0.0f };
		float miny{ 0.0f };
		float minz{ 0.0f };
		float maxx{ 0.0f };
		float maxy{ 0.0f };
		float maxz{ 0.0f };
	};

	auto buildFrameTransform = [&]() -> FrameTransform {
		FrameTransform frame{};

		// 1. バウンディングボックスの取得
		animator.GetBounds(frame.minx, frame.miny, frame.minz, frame.maxx, frame.maxy, frame.maxz);

		// 足元スナップ計算用に、余白を含まない純粋な下端(Y)を保持しておく
		const float rawMinY = frame.miny;

		// 自動リサイズ等の計算用に余白(margin)を含めた値を計算
		const float margin = 3.0f;
		frame.minx -= margin;
		frame.miny -= margin;
		frame.minz -= margin;
		frame.maxx += margin;
		frame.maxy += margin;
		frame.maxz += margin;

		// マージン込みの中心（追従用）
		const float cx = (frame.minx + frame.maxx) * 0.5f;
		const float cy = (frame.miny + frame.maxy) * 0.5f;
		const float cz = (frame.minz + frame.maxz) * 0.5f;

		// スケールはモデル基準の固定サイズから算出する。
		// 毎フレームの姿勢AABBに追従すると、回転やモーションで擬似的にズームアウトして見えるため。
		float baseMinX = frame.minx;
		float baseMinY = frame.miny;
		float baseMinZ = frame.minz;
		float baseMaxX = frame.maxx;
		float baseMaxY = frame.maxy;
		float baseMaxZ = frame.maxz;
		model->GetBounds(baseMinX, baseMinY, baseMinZ, baseMaxX, baseMaxY, baseMaxZ);

		const float sx = (baseMaxX - baseMinX) + margin * 2.0f;
		const float sy = (baseMaxY - baseMinY) + margin * 2.0f;
		const float sz = (baseMaxZ - baseMinZ) + margin * 2.0f;
		const float size = std::max({ sx, sy, sz, 1.0f });

		using namespace DirectX;
		const float scale = (1.0f / size) * m_lightSettings.modelScale;
		const XMMATRIX motionTransform = XMLoadFloat4x4(&animator.MotionTransform());

		// モデルトラッキング行列: 中心を原点に移動し、スケール、モーションを適用
		const XMMATRIX M_track =
			XMMatrixTranslation(-cx, -cy, -cz) *
			XMMatrixScaling(scale, scale, scale) *
			motionTransform;

		const float baseDistance = 2.5f;
		const float cameraYaw = m_camera.GetYaw();
		const float cameraPitch = m_camera.GetPitch();
		const float cameraDistance = m_camera.GetDistance();
		frame.distance = std::max(0.1f, baseDistance * cameraDistance);
		const float cosPitch = std::cos(cameraPitch);

		XMVECTOR eyeOffset = XMVectorSet(
			frame.distance * std::sin(cameraYaw) * cosPitch,
			frame.distance * std::sin(cameraPitch),
			-frame.distance * std::cos(cameraYaw) * cosPitch,
			0.0f
		);

		XMVECTOR target = XMVector3TransformCoord(XMVectorZero(), M_track);
		frame.eye = XMVectorAdd(target, eyeOffset);
		XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		frame.view = XMMatrixLookAtLH(frame.eye, target, up);

		// カメラ上方向（ワールド）を取得
		XMMATRIX invV = XMMatrixInverse(nullptr, frame.view);
		// View空間の(0,1,0)をワールドへ変換して、移動方向を厳密に一致させる（Yawでの微小な上下ズレ対策）
		XMVECTOR upWorld = XMVector3Normalize(
			XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), invV));

		// DPI が変わっても表示領域に対するモデルの占有率が変わらないように、
		// 物理ピクセルではなく DPI 補正後の論理サイズを基準に FOV を決める。
		const float refFov = XMConvertToRadians(30.0f);
		const float K = 600.0f / std::tan(refFov * 0.5f); // tan(fov/2)=H/K
		const float dpiScale = GetWindowDpiScale(m_hwnd);
		const float logicalWidth = (m_width > 0) ? static_cast<float>(m_width) / dpiScale : 400.0f;
		const float logicalHeight = (m_height > 0) ? static_cast<float>(m_height) / dpiScale : 600.0f;
		const float tanHalfFov = logicalHeight / K;
		float fovY = 2.0f * std::atan(tanHalfFov);
		fovY = std::clamp(fovY, XMConvertToRadians(10.0f), XMConvertToRadians(100.0f));

		const float aspect = (logicalHeight > 0.0f) ? logicalWidth / logicalHeight : 1.0f;
		frame.proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 100.0f);

		float snapT = 0.0f;
		{
			float footOffsetY = (rawMinY - cy) * scale;
			// Track空間での足位置。X, Zは0（中心）とする。
			XMVECTOR footPosTrack = XMVectorSet(0.0f, footOffsetY, 0.0f, 1.0f);

			// View行列を掛けて、カメラから見た足の位置(View Space)を計算
			XMVECTOR footPosView = XMVector3TransformCoord(footPosTrack, frame.view);
			float currentY = XMVectorGetY(footPosView);
			float currentZ = XMVectorGetZ(footPosView);

			/*
				画面下端に足を合わせるが、モデルサイズやポスト処理などで見切れやすいので、
				下側に一定の論理ピクセルマージンを確保する。
				DPI 差があっても余白比率を揃えるため、投影と同じ論理サイズ基準を使う。
			*/
			const float bottomMarginLogicalPx = std::clamp(logicalHeight * 0.10f, 16.0f, 128.0f);
			// y_ndc = -1 + 2*margin/h を満たす targetY を求める。
			// tanHalfFov = h/K なので、targetY = -z*tanHalfFov + z*(2*margin/K)
			float targetY = -currentZ * tanHalfFov + currentZ * (2.0f * bottomMarginLogicalPx / K);
			snapT = targetY - currentY;
		}

		// レンダリング用モデル行列
		// snapT はカメラの上方向(upWorld)に沿って移動させることで、View空間でのY移動を実現する
		frame.model =
			M_track *
			XMMatrixTranslationFromVector(XMVectorScale(upWorld, snapT));

		m_camera.CacheMatrices(frame.model, frame.view, frame.proj, m_width, m_height);
		return frame;
	};

	const FrameTransform frameTransform = buildFrameTransform();

	using namespace DirectX;
	const XMVECTOR keyDirV = SafeNormalizeDirection(
		XMVectorSet(m_lightSettings.keyLightDirX, m_lightSettings.keyLightDirY, m_lightSettings.keyLightDirZ, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
	const XMVECTOR fillDirV = SafeNormalizeDirection(
		XMVectorSet(m_lightSettings.fillLightDirX, m_lightSettings.fillLightDirY, m_lightSettings.fillLightDirZ, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));

	const auto computeShadowViewProj = [&]() -> XMMATRIX {
		const float cx = (frameTransform.minx + frameTransform.maxx) * 0.5f;
		const float cy = (frameTransform.miny + frameTransform.maxy) * 0.5f;
		const float cz = (frameTransform.minz + frameTransform.maxz) * 0.5f;

		const float sx = frameTransform.maxx - frameTransform.minx;
		const float sy = frameTransform.maxy - frameTransform.miny;
		const float sz = frameTransform.maxz - frameTransform.minz;
		const float radius = std::max({ sx, sy, sz, 1.0f }) * 0.9f;

		const XMVECTOR localCenter = XMVectorSet(cx, cy, cz, 1.0f);
		const XMVECTOR worldCenter = XMVector3TransformCoord(localCenter, frameTransform.model);
		const XMVECTOR worldUp =
			(std::abs(XMVectorGetX(XMVector3Dot(keyDirV, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)))) > 0.95f)
			? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
			: XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		const XMVECTOR lightEye = XMVectorSubtract(worldCenter, XMVectorScale(keyDirV, radius * 3.0f));
		const XMMATRIX lightView = XMMatrixLookAtLH(lightEye, worldCenter, worldUp);

		const float maxFloat = (std::numeric_limits<float>::max)();
		XMVECTOR minV = XMVectorSet(maxFloat, maxFloat, maxFloat, 1.0f);
		XMVECTOR maxV = XMVectorSet(-maxFloat, -maxFloat, -maxFloat, 1.0f);
		for (int xi = 0; xi < 2; ++xi)
		{
			for (int yi = 0; yi < 2; ++yi)
			{
				for (int zi = 0; zi < 2; ++zi)
				{
					const XMVECTOR localCorner = XMVectorSet(
						xi == 0 ? frameTransform.minx : frameTransform.maxx,
						yi == 0 ? frameTransform.miny : frameTransform.maxy,
						zi == 0 ? frameTransform.minz : frameTransform.maxz,
						1.0f);
					const XMVECTOR worldCorner = XMVector3TransformCoord(localCorner, frameTransform.model);
					const XMVECTOR lightCorner = XMVector3TransformCoord(worldCorner, lightView);
					minV = XMVectorMin(minV, lightCorner);
					maxV = XMVectorMax(maxV, lightCorner);
				}
			}
		}

		const float marginXY = radius * 0.15f;
		const float marginZ = radius * 0.75f;
		const float minX = XMVectorGetX(minV) - marginXY;
		const float maxX = XMVectorGetX(maxV) + marginXY;
		const float minY = XMVectorGetY(minV) - marginXY;
		const float maxY = XMVectorGetY(maxV) + marginXY;
		const float minZ = std::max(0.01f, XMVectorGetZ(minV) - marginZ);
		const float maxZ = XMVectorGetZ(maxV) + marginZ;
		const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);
		return lightView * lightProj;
	};

	const UINT frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	WaitForFrame(frameIndex);

	if (!ensureIntermediateResources() || !m_dsvHeap || !m_depth) return;

	const bool selfShadowEnabled =
		m_lightSettings.selfShadowEnabled &&
		m_shadowMap &&
		m_shadowDsvHeap;
	const XMMATRIX shadowViewProj = selfShadowEnabled ? computeShadowViewProj() : XMMatrixIdentity();

	// -------------------------------------------------------------
	// モーフの更新処理 (頂点変形 & マテリアル更新)
	// -------------------------------------------------------------
	m_pmxDrawer.UpdatePmxMorphs(animator);
	// -------------------------------------------------------------

	// マテリアル設定（影の濃さなど）を再適用
	m_pmxDrawer.UpdateMaterialSettings(m_lightSettings);

	m_alloc[frameIndex]->Reset();
	m_cmdList->Reset(m_alloc[frameIndex].get(), nullptr);
	m_pmxDrawer.CommitGpuUploads(m_cmdList.get());

	m_pmxDrawer.UpdateBoneMatrices(animator, m_boneCbMapped[frameIndex]);

	const bool useMsaa = (m_msaaSampleCount > 1) && m_msaaColor;

	D3D12_RESOURCE_DESC interDesc = m_intermediateTex->GetDesc();
	if (interDesc.Width != m_width || interDesc.Height != m_height)
	{
		m_intermediateState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	if (useMsaa)
	{
		if (m_msaaColorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_msaaColor.get(), m_msaaColorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_cmdList->ResourceBarrier(1, &barrier);
			m_msaaColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}
	}
	else
	{
		if (m_intermediateState != D3D12_RESOURCE_STATE_RENDER_TARGET)
		{
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_intermediateTex.get(), m_intermediateState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_cmdList->ResourceBarrier(1, &barrier);
			m_intermediateState = D3D12_RESOURCE_STATE_RENDER_TARGET;
		}
	}

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = useMsaa ? m_msaaRtvHandle : m_intermediateRtvHandle;
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

	m_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
	m_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	D3D12_VIEWPORT vp{};
	vp.Width = static_cast<float>(m_width);
	vp.Height = static_cast<float>(m_height);
	vp.MaxDepth = 1.0f;
	D3D12_RECT sc{};
	sc.right = static_cast<LONG>(m_width);
	sc.bottom = static_cast<LONG>(m_height);

	m_cmdList->RSSetViewports(1, &vp);
	m_cmdList->RSSetScissorRects(1, &sc);

	SceneCB* scene = m_sceneCbMapped[frameIndex];
	if (scene)
	{
		XMStoreFloat3(&scene->lightDir0, keyDirV);
		scene->ambient = m_lightSettings.ambientStrength;
		scene->lightColor0 = { m_lightSettings.keyLightColorR, m_lightSettings.keyLightColorG, m_lightSettings.keyLightColorB };
		scene->lightInt0 = m_lightSettings.keyLightIntensity;

		XMStoreFloat3(&scene->lightDir1, fillDirV);
		scene->lightColor1 = { m_lightSettings.fillLightColorR, m_lightSettings.fillLightColorG, m_lightSettings.fillLightColorB };
		scene->lightInt1 = m_lightSettings.fillLightIntensity;

		scene->specPower = 48.0f;
		scene->specColor = { 1.0f, 1.0f, 1.0f };
		scene->specStrength = 0.18f;
		scene->brightness = m_lightSettings.brightness;
		scene->toonContrast = m_lightSettings.toonContrast;
		scene->shadowHueShift = m_lightSettings.shadowHueShiftDeg * (XM_PI / 180.0f);
		scene->outlineRefDistance = frameTransform.distance;
		scene->outlineDistanceScale = 1.0f;
		scene->outlineDistancePower = 0.8f;
		scene->shadowRampShift = m_lightSettings.shadowRampShift;
		scene->shadowDeepThreshold = m_lightSettings.shadowDeepThreshold;
		scene->shadowDeepSoftness = m_lightSettings.shadowDeepSoftness;
		scene->shadowDeepMul = m_lightSettings.shadowDeepMul;
		scene->globalSaturation = m_lightSettings.globalSaturation;
		scene->shadowSaturation = m_lightSettings.shadowSaturationBoost;
		scene->rimWidth = m_lightSettings.rimWidth;
		scene->rimIntensity = m_lightSettings.rimIntensity;
		scene->specularStep = m_lightSettings.specularStep;
		scene->enableToon = m_lightSettings.toonEnabled ? 1u : 0u;
		scene->enableSkinning = animator.HasSkinnedPose() ? 1 : 0;
		XMStoreFloat3(&scene->cameraPos, frameTransform.eye);
		XMMATRIX MVP = frameTransform.model * frameTransform.view * frameTransform.proj;
		XMStoreFloat4x4(&scene->model, XMMatrixTranspose(frameTransform.model));
		XMStoreFloat4x4(&scene->view, XMMatrixTranspose(frameTransform.view));
		XMStoreFloat4x4(&scene->proj, XMMatrixTranspose(frameTransform.proj));
		XMStoreFloat4x4(&scene->mvp, XMMatrixTranspose(MVP));
		XMStoreFloat4x4(&scene->shadowMatrix, XMMatrixTranspose(shadowViewProj));

		XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, frameTransform.model));
		XMFLOAT4X4 normalMat4x4;
		XMStoreFloat4x4(&normalMat4x4, normalMat);
		scene->normalMatrixRow0 = { normalMat4x4._11, normalMat4x4._12, normalMat4x4._13, 0.0f };
		scene->normalMatrixRow1 = { normalMat4x4._21, normalMat4x4._22, normalMat4x4._23, 0.0f };
		scene->normalMatrixRow2 = { normalMat4x4._31, normalMat4x4._32, normalMat4x4._33, 0.0f };
		scene->invScreenSize = {
			(m_width > 0) ? (1.0f / static_cast<float>(m_width)) : 0.0f,
			(m_height > 0) ? (1.0f / static_cast<float>(m_height)) : 0.0f
		};
		scene->shadowMapInvSize = 1.0f / static_cast<float>(m_shadowMapSize);
		scene->shadowStrength = 0.72f;
		scene->enableSelfShadow = selfShadowEnabled ? 1u : 0u;
		scene->shadowBias = 2.0f / static_cast<float>(m_shadowMapSize);
		scene->outlineWidthScale = std::max(0.0f, m_lightSettings.outlineWidthScale);
		scene->outlineOpacityScale = std::max(0.0f, m_lightSettings.outlineOpacityScale);
	}

	auto srvHeap = m_gpuResources.GetSrvHeap();
	if (srvHeap)
	{
		ID3D12DescriptorHeap* heaps[] = { srvHeap };
		m_cmdList->SetDescriptorHeaps(1, heaps);

		m_cmdList->SetGraphicsRootSignature(m_pipeline.GetPmxRootSignature());
		m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_cmdList->IASetVertexBuffers(0, 1, &pmx.vbv);
		m_cmdList->IASetIndexBuffer(&pmx.ibv);

		m_cmdList->SetGraphicsRootConstantBufferView(0, m_sceneCb[frameIndex]->GetGPUVirtualAddress());
		m_cmdList->SetGraphicsRootConstantBufferView(3, m_boneCb[frameIndex]->GetGPUVirtualAddress());

		const auto* materialCb = m_pmxDrawer.GetMaterialCbMapped();
		const auto materialStride = m_pmxDrawer.GetMaterialCbStride();
		const uint64_t materialStateRevision = m_pmxDrawer.GetMaterialStateRevision();
		const bool materialBucketsDirty =
			!m_materialBucketsValid ||
			m_cachedPmxRevision != pmx.revision ||
			m_cachedMaterialStateRevision != materialStateRevision;

		if (materialBucketsDirty)
		{
			m_opaqueCull.clear();
			m_opaqueNoCull.clear();
			m_cutoutCull.clear();
			m_cutoutNoCull.clear();
			m_transCull.clear();
			m_transNoCull.clear();
			m_edgeDrawIndices.clear();
			m_opaqueCull.reserve(pmx.materials.size());
			m_opaqueNoCull.reserve(pmx.materials.size());
			m_cutoutCull.reserve(pmx.materials.size());
			m_cutoutNoCull.reserve(pmx.materials.size());
			m_transCull.reserve(pmx.materials.size());
			m_transNoCull.reserve(pmx.materials.size());
			m_edgeDrawIndices.reserve(pmx.materials.size());

			for (size_t i = 0; i < pmx.materials.size(); ++i)
			{
				const auto& gm = pmx.materials[i];
				const auto* cb = reinterpret_cast<const PmxModelDrawer::MaterialCB*>(materialCb + i * materialStride);
				const float materialAlpha = cb->diffuse.w * cb->textureFactor.w;
				const bool isTranslucent = (materialAlpha < 0.999f) || gm.baseTextureHasTranslucency;
				const bool isCutout = !isTranslucent && (cb->alphaCutout > 0.5f);
				const bool noCull = gm.mat.IsDoubleSided();

				if (isTranslucent)
				{
					(noCull ? m_transNoCull : m_transCull).push_back(i);
				}
				else if (isCutout)
				{
					(noCull ? m_cutoutNoCull : m_cutoutCull).push_back(i);
				}
				else
				{
					(noCull ? m_opaqueNoCull : m_opaqueCull).push_back(i);
				}

				if (gm.mat.ShouldDrawEdge() &&
					materialAlpha > 0.01f &&
					cb->edgeSize > 0.0f &&
					cb->edgeColor.w > 0.001f)
				{
					m_edgeDrawIndices.push_back(i);
				}
			}

			m_cachedPmxRevision = pmx.revision;
			m_cachedMaterialStateRevision = materialStateRevision;
			m_materialBucketsValid = true;
			m_transparentSortValid = false;
		}

		auto sortTransparentIndices = [&](std::vector<size_t>& indices)
		{
			if (indices.size() < 2)
			{
				return;
			}

			m_sortedTransparentEntries.clear();
			m_sortedTransparentEntries.reserve(indices.size());
			for (const size_t index : indices)
			{
				const auto& center = pmx.materials[index].localCenter;
				const DirectX::XMVECTOR localCenter = DirectX::XMLoadFloat3(&center);
				const DirectX::XMVECTOR worldCenter = DirectX::XMVector3TransformCoord(localCenter, frameTransform.model);
				const float distanceSq = DirectX::XMVectorGetX(
					DirectX::XMVector3LengthSq(
						DirectX::XMVectorSubtract(worldCenter, frameTransform.eye)));
				m_sortedTransparentEntries.push_back({ index, distanceSq });
			}

			std::stable_sort(
				m_sortedTransparentEntries.begin(),
				m_sortedTransparentEntries.end(),
				[](const SortedMaterialEntry& lhs, const SortedMaterialEntry& rhs) {
					return lhs.distanceSq > rhs.distanceSq;
				});

			for (size_t i = 0; i < indices.size(); ++i)
			{
				indices[i] = m_sortedTransparentEntries[i].index;
			}
		};

		DirectX::XMFLOAT4X4 currentSortModel{};
		DirectX::XMStoreFloat4x4(&currentSortModel, frameTransform.model);
		const DirectX::XMFLOAT3 currentSortEye{
			DirectX::XMVectorGetX(frameTransform.eye),
			DirectX::XMVectorGetY(frameTransform.eye),
			DirectX::XMVectorGetZ(frameTransform.eye)
		};
		const bool transparentSortDirty =
			!m_transparentSortValid ||
			!NearlyEqualMatrix(m_lastTransparentSortModel, currentSortModel) ||
			!NearlyEqualVector(m_lastTransparentSortEye, currentSortEye);

		if (transparentSortDirty)
		{
			sortTransparentIndices(m_transCull);
			sortTransparentIndices(m_transNoCull);
			m_lastTransparentSortModel = currentSortModel;
			m_lastTransparentSortEye = currentSortEye;
			m_transparentSortValid = true;
		}

		auto DrawMats = [&](ID3D12PipelineState* pso, const std::vector<size_t>& indices) {
			if (!pso || indices.empty()) return;
			m_cmdList->SetPipelineState(pso);
			for (auto i : indices)
			{
				const auto& gm = pmx.materials[i];
				m_cmdList->SetGraphicsRootConstantBufferView(1, gm.materialCbGpu);
				m_cmdList->SetGraphicsRootDescriptorTable(2, m_gpuResources.GetSrvGpuHandle(gm.srvBlockIndex));
				if (gm.mat.indexCount > 0)
					m_cmdList->DrawIndexedInstanced((UINT)gm.mat.indexCount, 1, (UINT)gm.mat.indexOffset, 0, 0);
			}
			};

		if (selfShadowEnabled)
		{
			if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
			{
				auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
					m_shadowMap.get(), m_shadowMapState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				m_cmdList->ResourceBarrier(1, &barrier);
				m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			}

			D3D12_VIEWPORT shadowVp{};
			shadowVp.Width = static_cast<float>(m_shadowMapSize);
			shadowVp.Height = static_cast<float>(m_shadowMapSize);
			shadowVp.MaxDepth = 1.0f;
			D3D12_RECT shadowRect{};
			shadowRect.right = static_cast<LONG>(m_shadowMapSize);
			shadowRect.bottom = static_cast<LONG>(m_shadowMapSize);

			m_cmdList->RSSetViewports(1, &shadowVp);
			m_cmdList->RSSetScissorRects(1, &shadowRect);
			m_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandle);
			m_cmdList->ClearDepthStencilView(m_shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

			m_cmdList->SetGraphicsRootSignature(m_pipeline.GetPmxRootSignature());
			m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_cmdList->IASetVertexBuffers(0, 1, &pmx.vbv);
			m_cmdList->IASetIndexBuffer(&pmx.ibv);
			m_cmdList->SetGraphicsRootConstantBufferView(0, m_sceneCb[frameIndex]->GetGPUVirtualAddress());
			m_cmdList->SetGraphicsRootConstantBufferView(3, m_boneCb[frameIndex]->GetGPUVirtualAddress());

			DrawMats(m_pipeline.GetShadowPso(), m_opaqueCull);
			DrawMats(m_pipeline.GetShadowPsoNoCull(), m_opaqueNoCull);
			DrawMats(m_pipeline.GetShadowPso(), m_cutoutCull);
			DrawMats(m_pipeline.GetShadowPsoNoCull(), m_cutoutNoCull);

			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				m_shadowMap.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			m_cmdList->ResourceBarrier(1, &barrier);
			m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}

		m_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
		m_cmdList->RSSetViewports(1, &vp);
		m_cmdList->RSSetScissorRects(1, &sc);
		m_cmdList->SetGraphicsRootSignature(m_pipeline.GetPmxRootSignature());
		m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_cmdList->IASetVertexBuffers(0, 1, &pmx.vbv);
		m_cmdList->IASetIndexBuffer(&pmx.ibv);
		m_cmdList->SetGraphicsRootConstantBufferView(0, m_sceneCb[frameIndex]->GetGPUVirtualAddress());
		m_cmdList->SetGraphicsRootConstantBufferView(3, m_boneCb[frameIndex]->GetGPUVirtualAddress());
		m_cmdList->SetGraphicsRootDescriptorTable(4, m_gpuResources.GetSrvGpuHandle(m_shadowSrvIndex));

		DrawMats(m_pipeline.GetPmxPsoOpaque(), m_opaqueCull);
		DrawMats(m_pipeline.GetPmxPsoOpaqueNoCull(), m_opaqueNoCull);
		DrawMats(m_pipeline.GetPmxPsoCutout(), m_cutoutCull);
		DrawMats(m_pipeline.GetPmxPsoCutoutNoCull(), m_cutoutNoCull);
		DrawMats(m_pipeline.GetPmxPsoTrans(), m_transCull);
		DrawMats(m_pipeline.GetPmxPsoTransNoCull(), m_transNoCull);

		if (m_lightSettings.outlineEnabled &&
			m_lightSettings.outlineWidthScale > 0.001f &&
			m_lightSettings.outlineOpacityScale > 0.001f)
		{
			m_cmdList->SetPipelineState(m_pipeline.GetEdgePso());
			m_cmdList->SetGraphicsRootSignature(m_pipeline.GetPmxRootSignature());
			m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			m_cmdList->IASetVertexBuffers(0, 1, &pmx.vbv);
			m_cmdList->IASetIndexBuffer(&pmx.ibv);

			m_cmdList->SetGraphicsRootConstantBufferView(0, m_sceneCb[frameIndex]->GetGPUVirtualAddress());
			m_cmdList->SetGraphicsRootConstantBufferView(3, m_boneCb[frameIndex]->GetGPUVirtualAddress());

			for (size_t i : m_edgeDrawIndices)
			{
				const auto& gm = pmx.materials[i];
				m_cmdList->SetGraphicsRootConstantBufferView(1, gm.materialCbGpu);
				m_cmdList->SetGraphicsRootDescriptorTable(2, m_gpuResources.GetSrvGpuHandle(gm.srvBlockIndex));
				if (gm.mat.indexCount > 0)
					m_cmdList->DrawIndexedInstanced((UINT)gm.mat.indexCount, 1, (UINT)gm.mat.indexOffset, 0, 0);
			}
		}
	}

	if (useMsaa)
	{
		auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_msaaColor.get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_intermediateTex.get(), m_intermediateState, D3D12_RESOURCE_STATE_RESOLVE_DEST);

		D3D12_RESOURCE_BARRIER barriers[] = { b1, b2 };
		m_cmdList->ResourceBarrier(2, barriers);

		m_cmdList->ResolveSubresource(
			m_intermediateTex.get(), 0, m_msaaColor.get(), 0, kSceneRenderTargetFormat);

		m_msaaColorState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
		m_intermediateState = D3D12_RESOURCE_STATE_RESOLVE_DEST;

		auto b3 = CD3DX12_RESOURCE_BARRIER::Transition(
			m_msaaColor.get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_cmdList->ResourceBarrier(1, &b3);
		m_msaaColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	{
		D3D12_RESOURCE_BARRIER barriers[2];
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
			m_intermediateTex.get(), m_intermediateState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[frameIndex].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		m_cmdList->ResourceBarrier(2, barriers);
		m_intermediateState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}

	auto backBufferRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	backBufferRtv.ptr += (SIZE_T)frameIndex * m_rtvDescriptorSize;
	m_cmdList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);

	ID3D12DescriptorHeap* fxaaHeaps[] = { m_intermediateSrvHeap.get() };
	m_cmdList->SetDescriptorHeaps(1, fxaaHeaps);

	m_cmdList->SetGraphicsRootSignature(m_pipeline.GetFxaaRootSignature());
	m_cmdList->SetPipelineState(m_pipeline.GetFxaaPso());
	m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const float scaleDeviation = std::abs(m_lightSettings.modelScale - 1.0f);
	const bool enableFxaa = UsesFxaaMode(m_lightSettings.antiAliasingMode);
	const float sharpenStrength = enableFxaa ? std::clamp(scaleDeviation * 0.05f, 0.0f, 0.08f) : 0.0f;
	float consts[8] = {
		1.0f / (float)m_width,
		1.0f / (float)m_height,
		sharpenStrength,
		enableFxaa ? 1.0f : 0.0f,
		0.0f,
		0.0f,
		0.0f,
		0.0f
	};
	m_cmdList->SetGraphicsRoot32BitConstants(0, 8, consts, 0);

	m_cmdList->SetGraphicsRootDescriptorTable(1, m_intermediateSrvGpuHandle);

	m_cmdList->DrawInstanced(3, 1, 0, 0);

	{
		auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[frameIndex].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		m_cmdList->ResourceBarrier(1, &barrier);
	}

	m_cmdList->Close();
	ID3D12CommandList* lists[] = { m_cmdList.get() };
	m_ctx.Queue()->ExecuteCommandLists(1, lists);

	m_swapChain->Present(m_vsyncEnabled ? 1u : 0u, 0);

	const UINT64 signalValue = m_fenceValue++;
	m_ctx.Queue()->Signal(m_fence.get(), signalValue);
	m_frameFenceValues[frameIndex] = signalValue;
}

void DcompRenderer::WaitForGpu()
{
	const UINT64 fenceToWait = m_fenceValue++;
	m_ctx.Queue()->Signal(m_fence.get(), fenceToWait);

	if (m_fence->GetCompletedValue() < fenceToWait)
	{
		m_fence->SetEventOnCompletion(fenceToWait, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
}

void DcompRenderer::CreateDepthBuffer()
{
	if (m_width == 0 || m_height == 0) return;
	if (!m_dsvHeap)
	{
		throw std::runtime_error("DSV heap not created.");
	}

	m_depth = nullptr;

	D3D12_CLEAR_VALUE clear{};
	clear.Format = DXGI_FORMAT_D32_FLOAT;
	clear.DepthStencil.Depth = 1.0f;
	clear.DepthStencil.Stencil = 0;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_D32_FLOAT,
		m_width, m_height,
		1, 1,
		m_msaaSampleCount, m_msaaQuality,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
	);

	DX_CALL(m_ctx.Device()->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&clear, IID_PPV_ARGS(m_depth.put())));

	m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_ctx.Device()->CreateDepthStencilView(m_depth.get(), nullptr, m_dsvHandle);
}

void DcompRenderer::WaitForFrame(UINT frameIndex)
{
	const UINT64 fenceValue = m_frameFenceValues[frameIndex];
	if (fenceValue == 0) return;

	if (m_fence->GetCompletedValue() < fenceValue)
	{
		DX_CALL(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
}

void DcompRenderer::CreateMsaaTargets()
{
	SelectMaximumMsaa();
	if (UsesMsaaMode(m_lightSettings.antiAliasingMode) && m_maxMsaaSampleCount > 1)
	{
		m_msaaSampleCount = GetRequestedMsaaSampleCount();
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
		levels.Format = kSceneRenderTargetFormat;
		levels.SampleCount = m_msaaSampleCount;
		levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
		if (SUCCEEDED(m_ctx.Device()->CheckFeatureSupport(
			D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
			&levels, sizeof(levels))) &&
			levels.NumQualityLevels > 0)
		{
			m_msaaQuality = levels.NumQualityLevels - 1;
		}
		else
		{
			m_msaaSampleCount = 1;
			m_msaaQuality = 0;
		}
	}
	else
	{
		m_msaaSampleCount = 1;
		m_msaaQuality = 0;
	}

	CreateDepthBuffer();

	if (m_msaaSampleCount <= 1)
	{
		m_msaaColor = nullptr;
		m_msaaRtvHeap = nullptr;
		m_msaaColorState = D3D12_RESOURCE_STATE_COMMON;
		return;
	}

	// Create MSAA color target
	m_msaaColor = nullptr;
	m_msaaRtvHeap = nullptr;

	D3D12_CLEAR_VALUE clear{};
	clear.Format = kSceneRenderTargetFormat;
	clear.Color[0] = 0.f;
	clear.Color[1] = 0.f;
	clear.Color[2] = 0.f;
	clear.Color[3] = 0.f;

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto colorDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		kSceneRenderTargetFormat,
		m_width, m_height,
		1, 1,
		m_msaaSampleCount, m_msaaQuality,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

	DX_CALL(m_ctx.Device()->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &colorDesc,
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		&clear, IID_PPV_ARGS(m_msaaColor.put())));

	m_msaaColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

	// RTV heap for MSAA color
	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = 1;
	rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	DX_CALL(m_ctx.Device()->CreateDescriptorHeap(
		&rtvDesc, IID_PPV_ARGS(m_msaaRtvHeap.put())));

	m_msaaRtvHandle = m_msaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
	m_ctx.Device()->CreateRenderTargetView(
		m_msaaColor.get(), nullptr, m_msaaRtvHandle);
}

UINT DcompRenderer::GetRequestedMsaaSampleCount() const
{
	const UINT requested = NormalizeMsaaSampleCount(m_lightSettings.msaaSampleCount);
	return (std::min)(requested, m_maxMsaaSampleCount);
}

UINT DcompRenderer::GetActiveShadowMapSize() const
{
	return NormalizeShadowMapSize(m_lightSettings.shadowMapSize);
}

void DcompRenderer::SelectMaximumMsaa()
{
	const DXGI_FORMAT fmt = kSceneRenderTargetFormat;

	m_maxMsaaSampleCount = 1;
	m_maxMsaaQuality = 0;

	const UINT candidates[] = { 32, 16, 8, 4, 2 };
	for (UINT count : candidates)
	{
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
		levels.Format = fmt;
		levels.SampleCount = count;
		levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

		if (SUCCEEDED(m_ctx.Device()->CheckFeatureSupport(
			D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
			&levels, sizeof(levels))) &&
			levels.NumQualityLevels > 0)
		{
			m_maxMsaaSampleCount = count;
			m_maxMsaaQuality = levels.NumQualityLevels - 1;
			break;
		}
	}
}

void DcompRenderer::AdjustScale(float delta)
{
	m_camera.AdjustScale(m_lightSettings, delta);
}

void DcompRenderer::AddCameraRotation(float dxPixels, float dyPixels)
{
	m_camera.AddCameraRotation(dxPixels, dyPixels);
}

void DcompRenderer::LoadTexturesForModel(const PmxModel* model,
										 std::function<void(float, const wchar_t*)> onProgress,
										 float startProgress, float endProgress)
{
	if (!model) return;

	const auto& texPaths = model->TexturePaths();
	size_t total = texPaths.size();
	if (total == 0) return;

	m_gpuResources.CreateUploadObjects();

	for (size_t i = 0; i < total; ++i)
	{
		if (onProgress && (i % 5 == 0 || i == total - 1))
		{
			float ratio = (float)i / (float)total;
			float current = startProgress + ratio * (endProgress - startProgress);

			auto buf = std::format(L"テクスチャ読み込み中 ({}/{})...", i + 1, total);
			onProgress(current, buf.c_str());
		}

		m_gpuResources.LoadTextureSrv(texPaths[i]);
	}
}

DirectX::XMFLOAT3 DcompRenderer::ProjectToScreen(const DirectX::XMFLOAT3& localPos) const
{
	return m_camera.ProjectToScreen(localPos);
}
