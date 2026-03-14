#include "GpuResourceManager.hpp"

#include "d3dx12.h"
#include "ExceptionHelper.hpp"
#include "WicTexture.hpp"
#include "DebugUtil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace
{
	static float Lanczos(float x, float a = 3.0f)
	{
		if (std::abs(x) < 1e-6f) return 1.0f;
		if (x < -a || x > a) return 0.0f;
		const float pi = 3.14159265359f;
		float pi_x = pi * x;
		return (a * std::sin(pi_x) * std::sin(pi_x / a)) / (pi_x * pi_x);
	}

	static std::vector<std::vector<uint8_t>> BuildMipChainRGBA(const uint8_t* src, uint32_t w, uint32_t h)
	{
		static constexpr int kRadius = 3;
		static constexpr int kTaps = kRadius * 2;

		static const std::array<float, 256> kToLinear = [] {
			std::array<float, 256> t{};
			for (int i = 0; i < 256; ++i)
			{
				const float v = (float)i / 255.0f;
				t[i] = std::pow(v, 2.2f);
			}
			return t;
			}();
		static const std::array<float, 256> kToNorm = [] {
			std::array<float, 256> t{};
			for (int i = 0; i < 256; ++i) t[i] = (float)i / 255.0f;
			return t;
			}();

		auto clampi = [](int v, int lo, int hi) -> int {
			return (v < lo) ? lo : (v > hi ? hi : v);
			};

		uint32_t levels = 1;
		for (uint32_t tw = w, th = h; tw > 1 || th > 1; )
		{
			tw = std::max(1u, tw / 2);
			th = std::max(1u, th / 2);
			++levels;
		}

		std::vector<std::vector<uint8_t>> mips;
		mips.reserve(levels);

		mips.emplace_back(src, src + (size_t)w * h * 4);

		uint32_t cw = w, ch = h;

		while (cw > 1 || ch > 1)
		{
			const uint32_t nw = std::max(1u, cw / 2);
			const uint32_t nh = std::max(1u, ch / 2);

			std::vector<uint8_t> next((size_t)nw * nh * 4);

			const auto& prevVec = mips.back();
			const uint8_t* prev = prevVec.data();
			uint8_t* dst = next.data();

			const float scaleX = (float)cw / (float)nw;
			const float scaleY = (float)ch / (float)nh;
			static constexpr float kInvGamma = 1.0f / 2.2f;

			std::vector<std::array<int, kTaps>> pxTable(nw);
			std::vector<std::array<float, kTaps>> wxTable(nw);

			for (uint32_t x = 0; x < nw; ++x)
			{
				const float centerX = (x + 0.5f) * scaleX - 0.5f;
				const int startX = (int)std::floor(centerX) - kRadius + 1;

				auto& pxA = pxTable[x];
				auto& wxA = wxTable[x];

				for (int kx = 0; kx < kTaps; ++kx)
				{
					const int px = clampi(startX + kx, 0, (int)cw - 1);
					pxA[kx] = px;
					wxA[kx] = Lanczos(centerX - (float)px);
				}
			}

#if defined(_OPENMP)
#pragma omp parallel for schedule(static)
#endif
			for (int yi = 0; yi < (int)nh; ++yi)
			{
				const uint32_t y = (uint32_t)yi;

				std::array<int, kTaps> pyA{};
				std::array<float, kTaps> wyA{};
				std::array<size_t, kTaps> rowBaseA{};

				const float centerY = (y + 0.5f) * scaleY - 0.5f;
				const int startY = (int)std::floor(centerY) - kRadius + 1;

				for (int ky = 0; ky < kTaps; ++ky)
				{
					const int py = clampi(startY + ky, 0, (int)ch - 1);
					pyA[ky] = py;
					wyA[ky] = Lanczos(centerY - (float)py);
					rowBaseA[ky] = (size_t)py * (size_t)cw * 4;
				}

				for (uint32_t x = 0; x < nw; ++x)
				{
					const auto& pxA = pxTable[x];
					const auto& wxA = wxTable[x];

					float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
					float weightSum = 0.0f;

					for (int ky = 0; ky < kTaps; ++ky)
					{
						const float wy = wyA[ky];
						const size_t rowBase = rowBaseA[ky];

						for (int kx = 0; kx < kTaps; ++kx)
						{
							const float weight = wxA[kx] * wy;
							weightSum += weight;

							const size_t idx = rowBase + (size_t)pxA[kx] * 4;

							const uint8_t r8 = prev[idx + 0];
							const uint8_t g8 = prev[idx + 1];
							const uint8_t b8 = prev[idx + 2];
							const uint8_t a8 = prev[idx + 3];

							acc0 += kToLinear[r8] * weight;
							acc1 += kToLinear[g8] * weight;
							acc2 += kToLinear[b8] * weight;
							acc3 += kToNorm[a8] * weight;
						}
					}

					if (weightSum > 0.0f)
					{
						const float invW = 1.0f / weightSum;
						acc0 *= invW;
						acc1 *= invW;
						acc2 *= invW;
						acc3 *= invW;
					}

					const size_t di = ((size_t)y * nw + x) * 4;
					dst[di + 0] = (uint8_t)std::clamp(std::pow(acc0, kInvGamma) * 255.0f, 0.0f, 255.0f);
					dst[di + 1] = (uint8_t)std::clamp(std::pow(acc1, kInvGamma) * 255.0f, 0.0f, 255.0f);
					dst[di + 2] = (uint8_t)std::clamp(std::pow(acc2, kInvGamma) * 255.0f, 0.0f, 255.0f);
					dst[di + 3] = (uint8_t)std::clamp(acc3 * 255.0f, 0.0f, 255.0f);
				}
			}

			mips.emplace_back(std::move(next));
			cw = nw;
			ch = nh;
		}

		return mips;
	}

	static std::wstring MakeSharedToonFileName(int toonIndex)
	{
		const int oneBasedIndex = toonIndex + 1;
		return (oneBasedIndex < 10)
			? (L"toon0" + std::to_wstring(oneBasedIndex) + L".bmp")
			: (L"toon" + std::to_wstring(oneBasedIndex) + L".bmp");
	}

	static std::vector<uint8_t> BuildToonRampTexture(
		float band0,
		float band1,
		float band2,
		float band3)
	{
		std::vector<uint8_t> rgba(256 * 4);
		for (int x = 0; x < 256; ++x)
		{
			const float t = x / 255.0f;
			float v = band3;
			if (t < 0.22f) v = band0;
			else if (t < 0.52f) v = band1;
			else if (t < 0.80f) v = band2;

			const uint8_t c = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
			rgba[x * 4 + 0] = c;
			rgba[x * 4 + 1] = c;
			rgba[x * 4 + 2] = c;
			rgba[x * 4 + 3] = 255;
		}
		return rgba;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC MakeTexture2dSrvDesc(UINT mipLevels)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = mipLevels;
		return srv;
	}
}

void GpuResourceManager::Initialize(Dx12Context* ctx, std::function<void()> waitForGpu, UINT frameCount)
{
	m_ctx = ctx;
	m_waitForGpu = std::move(waitForGpu);
	m_frameCount = frameCount;
}

void GpuResourceManager::CreateSrvHeap()
{
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = 4096;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	DX_CALL(m_ctx->Device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_srvHeap.put())));
	m_srvDescriptorSize = m_ctx->Device()->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);
	m_srvCapacity = desc.NumDescriptors;
}

void GpuResourceManager::CreateUploadObjects()
{
	if (m_uploadAlloc && m_uploadCmdList) return;

	DX_CALL(m_ctx->Device()->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_uploadAlloc.put())));

	DX_CALL(m_ctx->Device()->CreateCommandList(
		0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_uploadAlloc.get(), nullptr,
		IID_PPV_ARGS(m_uploadCmdList.put())));

	m_uploadCmdList->Close();
}

void GpuResourceManager::ResetTextureCache()
{
	m_nextSrvIndex = 0;
	m_textureCache.clear();
	m_toonTextureCache.clear();
	m_sharedToonCache.clear();
	m_textures.clear();
	m_textureIndexBySrv.clear();
	m_defaultWhiteSrv = CreateWhiteTexture1x1();
	m_defaultToonSrv = CreateDefaultToonRamp();
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuResourceManager::GetSrvCpuHandle(uint32_t index) const
{
	auto h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(index) * m_srvDescriptorSize;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuResourceManager::GetSrvGpuHandle(uint32_t index) const
{
	auto h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(index) * m_srvDescriptorSize;
	return h;
}

uint32_t GpuResourceManager::AllocSrvIndex()
{
	if (!m_srvHeap || m_srvCapacity == 0)
	{
		throw std::runtime_error("SRV heap is not initialized.");
	}
	if (m_nextSrvIndex >= m_srvCapacity)
	{
		throw std::runtime_error("SRV heap capacity exceeded.");
	}

	const uint32_t idx = m_nextSrvIndex++;
	return idx;
}

uint32_t GpuResourceManager::AllocSrvBlock3()
{
	if (!m_srvHeap || m_srvCapacity == 0)
	{
		throw std::runtime_error("SRV heap is not initialized.");
	}
	if (m_srvCapacity < 3 || m_nextSrvIndex > m_srvCapacity - 3)
	{
		throw std::runtime_error("SRV heap capacity exceeded.");
	}

	const uint32_t base = m_nextSrvIndex;
	m_nextSrvIndex += 3;
	return base;
}

void GpuResourceManager::CopySrv(uint32_t dstIndex, uint32_t srcIndex)
{
	if (!m_srvHeap || m_srvCapacity == 0)
	{
		throw std::runtime_error("SRV heap is not initialized.");
	}
	if (dstIndex >= m_srvCapacity || srcIndex >= m_srvCapacity)
	{
		throw std::runtime_error("SRV copy index out of range.");
	}

	auto dst = GetSrvCpuHandle(dstIndex);
	auto src = GetSrvCpuHandle(srcIndex);

	m_ctx->Device()->CopyDescriptorsSimple(
		1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint32_t GpuResourceManager::CreateWhiteTexture1x1()
{
	const uint8_t white[4] = { 255, 255, 255, 255 };

	const uint32_t srvIndex = AllocSrvIndex();

	auto tex = CreateTexture2DFromRgba(white, 1, 1);
	const auto srv = MakeTexture2dSrvDesc(1);
	m_ctx->Device()->CreateShaderResourceView(tex.get(), &srv, GetSrvCpuHandle(srvIndex));
	RegisterTexture(std::move(tex), srvIndex, 1, 1, false, false);

	return srvIndex;
}

uint32_t GpuResourceManager::CreateDefaultToonRamp()
{
	const std::vector<uint8_t> rgba = BuildToonRampTexture(0.15f, 0.45f, 0.78f, 1.0f);

	const uint32_t srvIndex = AllocSrvIndex();

	auto tex = CreateTexture2DFromRgba(rgba.data(), 256, 1);
	const auto srv = MakeTexture2dSrvDesc(1);
	m_ctx->Device()->CreateShaderResourceView(tex.get(), &srv, GetSrvCpuHandle(srvIndex));
	RegisterTexture(std::move(tex), srvIndex, 256, 1, false, false);

	return srvIndex;
}

uint32_t GpuResourceManager::CreateSharedToonRamp(int toonIndex)
{
	static constexpr std::array<std::array<float, 4>, 10> kBands = {
		std::array<float, 4>{ 0.06f, 0.22f, 0.48f, 0.92f },
		std::array<float, 4>{ 0.08f, 0.26f, 0.54f, 0.94f },
		std::array<float, 4>{ 0.10f, 0.32f, 0.60f, 0.96f },
		std::array<float, 4>{ 0.12f, 0.36f, 0.66f, 0.98f },
		std::array<float, 4>{ 0.15f, 0.42f, 0.72f, 1.00f },
		std::array<float, 4>{ 0.18f, 0.48f, 0.78f, 1.00f },
		std::array<float, 4>{ 0.22f, 0.54f, 0.82f, 1.00f },
		std::array<float, 4>{ 0.26f, 0.60f, 0.86f, 1.00f },
		std::array<float, 4>{ 0.30f, 0.66f, 0.90f, 1.00f },
		std::array<float, 4>{ 0.34f, 0.72f, 0.94f, 1.00f },
	};

	const int clampedIndex = std::clamp(toonIndex, 0, static_cast<int>(kBands.size()) - 1);
	const auto& bands = kBands[clampedIndex];
	const std::vector<uint8_t> rgba = BuildToonRampTexture(bands[0], bands[1], bands[2], bands[3]);

	const uint32_t srvIndex = AllocSrvIndex();
	auto tex = CreateTexture2DFromRgba(rgba.data(), 256, 1);
	const auto srv = MakeTexture2dSrvDesc(1);
	m_ctx->Device()->CreateShaderResourceView(tex.get(), &srv, GetSrvCpuHandle(srvIndex));
	RegisterTexture(std::move(tex), srvIndex, 256, 1, false, false);
	return srvIndex;
}

uint32_t GpuResourceManager::LoadTextureSrv(const std::filesystem::path& path)
{
	return LoadTextureSrvInternal(path, m_textureCache, m_defaultWhiteSrv, true);
}

uint32_t GpuResourceManager::LoadToonTextureSrv(const std::filesystem::path& path)
{
	return LoadTextureSrvInternal(path, m_toonTextureCache, m_defaultToonSrv, false);
}

uint32_t GpuResourceManager::LoadTextureSrvInternal(
	const std::filesystem::path& path,
	std::unordered_map<std::wstring, uint32_t>& cache,
	uint32_t fallbackSrv,
	bool generateMips)
{
	const std::wstring key = path.wstring();

	if (auto it = cache.find(key); it != cache.end())
	{
		return it->second;
	}

	if (!std::filesystem::exists(path))
	{
		cache[key] = fallbackSrv;
		return fallbackSrv;
	}

	WicImage img = WicTexture::LoadRgba(path);

	const uint32_t srvIndex = AllocSrvIndex();
	winrt::com_ptr<ID3D12Resource> tex;
	UINT mipLevels = 1;
	if (generateMips)
	{
		auto mips = BuildMipChainRGBA(img.rgba.data(), img.width, img.height);
		mipLevels = static_cast<UINT>(mips.size());
		tex = CreateTexture2DFromRgbaMips(img.width, img.height, mips);
	}
	else
	{
		tex = CreateTexture2DFromRgba(img.rgba.data(), img.width, img.height);
	}

	const auto srv = MakeTexture2dSrvDesc(mipLevels);
	m_ctx->Device()->CreateShaderResourceView(tex.get(), &srv, GetSrvCpuHandle(srvIndex));
	RegisterTexture(
		std::move(tex),
		srvIndex,
		img.width,
		img.height,
		img.hasTransparentPixels,
		img.hasTranslucentPixels);

	cache[key] = srvIndex;
	return srvIndex;
}

uint32_t GpuResourceManager::LoadSharedToonSrv(const std::filesystem::path& modelDir, int toonIndex)
{
	if (toonIndex < 0 || toonIndex >= 10)
	{
		return m_defaultToonSrv;
	}

	if (auto it = m_sharedToonCache.find(toonIndex); it != m_sharedToonCache.end())
	{
		return it->second;
	}

	const std::wstring fileName = MakeSharedToonFileName(toonIndex);
	const std::array<std::filesystem::path, 5> candidates = {
		modelDir / fileName,
		modelDir / L"toon" / fileName,
		modelDir.parent_path() / fileName,
		modelDir.parent_path() / L"toon" / fileName,
		std::filesystem::current_path() / fileName,
	};

	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			const uint32_t srv = LoadToonTextureSrv(candidate);
			m_sharedToonCache[toonIndex] = srv;
			return srv;
		}
	}

	const uint32_t fallbackSrv = CreateSharedToonRamp(toonIndex);
	m_sharedToonCache[toonIndex] = fallbackSrv;
	return fallbackSrv;
}

const GpuResourceManager::GpuTexture* GpuResourceManager::FindTexture(uint32_t srvIndex) const
{
	if (auto it = m_textureIndexBySrv.find(srvIndex); it != m_textureIndexBySrv.end())
	{
		const size_t textureIndex = it->second;
		if (textureIndex < m_textures.size())
		{
			return &m_textures[textureIndex];
		}
	}

	return nullptr;
}

void GpuResourceManager::RegisterTexture(
	winrt::com_ptr<ID3D12Resource> texture,
	uint32_t srvIndex,
	uint32_t width,
	uint32_t height,
	bool hasTransparentPixels,
	bool hasTranslucentPixels)
{
	m_textures.push_back(GpuTexture{
		std::move(texture),
		srvIndex,
		width,
		height,
		hasTransparentPixels,
		hasTranslucentPixels
	});
	m_textureIndexBySrv[srvIndex] = m_textures.size() - 1;
}

winrt::com_ptr<ID3D12Resource>
GpuResourceManager::CreateTexture2DFromRgbaMips(
	uint32_t width, uint32_t height,
	const std::vector<std::vector<uint8_t>>& mips)
{
	if (mips.empty()) throw std::runtime_error("mips empty.");
	if (!m_uploadAlloc || !m_uploadCmdList)
	{
		CreateUploadObjects();
	}

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R8G8B8A8_UNORM,
		width, height,
		1, (UINT16)mips.size()
	);

	winrt::com_ptr<ID3D12Resource> tex;
	DX_CALL(m_ctx->Device()->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(tex.put())));

	const UINT64 uploadSize = GetRequiredIntermediateSize(tex.get(), 0, (UINT)mips.size());

	auto upHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

	winrt::com_ptr<ID3D12Resource> upload;
	DX_CALL(m_ctx->Device()->CreateCommittedResource(
		&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(upload.put())));

	std::vector<D3D12_SUBRESOURCE_DATA> subs;
	subs.reserve(mips.size());

	uint32_t cw = width, ch = height;
	for (size_t i = 0; i < mips.size(); ++i)
	{
		D3D12_SUBRESOURCE_DATA s{};
		s.pData = mips[i].data();
		s.RowPitch = (LONG_PTR)cw * 4;
		s.SlicePitch = s.RowPitch * ch;
		subs.push_back(s);

		cw = std::max(1u, cw / 2);
		ch = std::max(1u, ch / 2);
	}

	m_uploadAlloc->Reset();
	m_uploadCmdList->Reset(m_uploadAlloc.get(), nullptr);

	UpdateSubresources(
		m_uploadCmdList.get(), tex.get(), upload.get(),
		0, 0, (UINT)subs.size(), subs.data());

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		tex.get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_uploadCmdList->ResourceBarrier(1, &barrier);

	m_uploadCmdList->Close();
	ID3D12CommandList* lists[] = { m_uploadCmdList.get() };
	m_ctx->Queue()->ExecuteCommandLists(1, lists);

	if (m_waitForGpu)
	{
		m_waitForGpu();
	}

	return tex;
}

winrt::com_ptr<ID3D12Resource>
GpuResourceManager::CreateTexture2DFromRgba(const uint8_t* rgba, uint32_t width, uint32_t height)
{
	if (!rgba || width == 0 || height == 0)
	{
		throw std::runtime_error("CreateTexture2DFromRgba: invalid input.");
	}

	if (!m_uploadAlloc || !m_uploadCmdList)
	{
		CreateUploadObjects();
	}

	auto device = m_ctx->Device();

	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R8G8B8A8_UNORM, width, height);

	winrt::com_ptr<ID3D12Resource> tex;
	DX_CALL(device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(tex.put())));

	const UINT64 uploadSize = GetRequiredIntermediateSize(tex.get(), 0, 1);

	auto upHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto upDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

	winrt::com_ptr<ID3D12Resource> upload;
	DX_CALL(device->CreateCommittedResource(
		&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(upload.put())));

	D3D12_SUBRESOURCE_DATA sub{};
	sub.pData = rgba;
	sub.RowPitch = (LONG_PTR)width * 4;
	sub.SlicePitch = sub.RowPitch * height;

	m_uploadAlloc->Reset();
	m_uploadCmdList->Reset(m_uploadAlloc.get(), nullptr);

	UpdateSubresources(
		m_uploadCmdList.get(),
		tex.get(), upload.get(),
		0, 0, 1, &sub);

	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		tex.get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	m_uploadCmdList->ResourceBarrier(1, &barrier);

	m_uploadCmdList->Close();
	ID3D12CommandList* lists[] = { m_uploadCmdList.get() };
	m_ctx->Queue()->ExecuteCommandLists(1, lists);

	if (m_waitForGpu)
	{
		m_waitForGpu();
	}

	return tex;
}

