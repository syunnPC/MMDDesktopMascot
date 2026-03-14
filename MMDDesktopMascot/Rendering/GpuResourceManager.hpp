#pragma once

#include <d3d12.h>
#include <winrt/base.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Dx12Context.hpp"

class GpuResourceManager
{
public:
	struct GpuTexture
	{
		winrt::com_ptr<ID3D12Resource> resource;
		uint32_t srvIndex{};
		uint32_t width{};
		uint32_t height{};
		bool hasTransparentPixels{};
		bool hasTranslucentPixels{};
	};

	void Initialize(Dx12Context* ctx, std::function<void()> waitForGpu, UINT frameCount);

	void CreateSrvHeap();
	void CreateUploadObjects();
	void ResetTextureCache();

	uint32_t LoadTextureSrv(const std::filesystem::path& path);
	uint32_t LoadToonTextureSrv(const std::filesystem::path& path);
	uint32_t LoadSharedToonSrv(const std::filesystem::path& modelDir, int toonIndex);
	const GpuTexture* FindTexture(uint32_t srvIndex) const;

	ID3D12DescriptorHeap* GetSrvHeap() const
	{
		return m_srvHeap.get();
	}
	D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle(uint32_t index) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(uint32_t index) const;

	uint32_t AllocSrvIndex();
	uint32_t AllocSrvBlock3();
	void CopySrv(uint32_t dstIndex, uint32_t srcIndex);

	uint32_t GetDefaultWhiteSrv() const
	{
		return m_defaultWhiteSrv;
	}
	uint32_t GetDefaultToonSrv() const
	{
		return m_defaultToonSrv;
	}

private:
	winrt::com_ptr<ID3D12Resource> CreateTexture2DFromRgba(
		const uint8_t* rgba, uint32_t width, uint32_t height);

	winrt::com_ptr<ID3D12Resource> CreateTexture2DFromRgbaMips(
		uint32_t width, uint32_t height,
		const std::vector<std::vector<uint8_t>>& mips);
	void RegisterTexture(
		winrt::com_ptr<ID3D12Resource> texture,
		uint32_t srvIndex,
		uint32_t width,
		uint32_t height,
		bool hasTransparentPixels,
		bool hasTranslucentPixels);
	uint32_t LoadTextureSrvInternal(
		const std::filesystem::path& path,
		std::unordered_map<std::wstring, uint32_t>& cache,
		uint32_t fallbackSrv,
		bool generateMips);

	uint32_t CreateWhiteTexture1x1();
	uint32_t CreateDefaultToonRamp();
	uint32_t CreateSharedToonRamp(int toonIndex);

	Dx12Context* m_ctx{};
	std::function<void()> m_waitForGpu;
	UINT m_frameCount{};

	winrt::com_ptr<ID3D12DescriptorHeap> m_srvHeap;
	UINT m_srvDescriptorSize{};
	uint32_t m_srvCapacity{};

	std::vector<GpuTexture> m_textures;
	std::unordered_map<uint32_t, size_t> m_textureIndexBySrv;
	std::unordered_map<std::wstring, uint32_t> m_textureCache;
	std::unordered_map<std::wstring, uint32_t> m_toonTextureCache;
	std::unordered_map<int, uint32_t> m_sharedToonCache;

	winrt::com_ptr<ID3D12CommandAllocator> m_uploadAlloc;
	winrt::com_ptr<ID3D12GraphicsCommandList> m_uploadCmdList;

	uint32_t m_nextSrvIndex = 0;
	uint32_t m_defaultWhiteSrv = 0;
	uint32_t m_defaultToonSrv = 0;
};
