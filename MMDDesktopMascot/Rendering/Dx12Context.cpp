#include "Dx12Context.hpp"
#include "ExceptionHelper.hpp"
#include "DebugUtil.hpp"
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace
{
	struct AdapterCandidate
	{
		winrt::com_ptr<IDXGIAdapter1> adapter;
		DXGI_ADAPTER_DESC1 desc{};
		int64_t score{};
	};

	void AppendHardwareAdapter(std::vector<AdapterCandidate>& candidates, IDXGIAdapter1* rawAdapter)
	{
		if (!rawAdapter)
		{
			return;
		}

		AdapterCandidate candidate{};
		candidate.adapter.copy_from(rawAdapter);
		candidate.adapter->GetDesc1(&candidate.desc);
		if (candidate.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			return;
		}

		candidates.push_back(std::move(candidate));
	}

	bool IsRunningOnBattery() noexcept
	{
		SYSTEM_POWER_STATUS power{};
		return GetSystemPowerStatus(&power) && power.ACLineStatus == 0;
	}

	int64_t ScoreAdapter(const DXGI_ADAPTER_DESC1& desc, bool preferDiscrete)
	{
		const bool discrete = desc.DedicatedVideoMemory > 0;
		const bool preferredVendor =
			(desc.VendorId == 0x10DE) || // NVIDIA
			(desc.VendorId == 0x1002) || // AMD
			(desc.VendorId == 0x1022);   // AMD Alt (APU)

		int64_t score = static_cast<int64_t>(std::min<UINT64>(desc.DedicatedVideoMemory / (1024ull * 1024ull), 500'000ull));
		if (discrete)
		{
			score += preferDiscrete ? 1'000'000 : 50'000;
		}
		else if (!preferDiscrete)
		{
			score += 200'000;
		}
		if (preferredVendor)
		{
			score += 100'000;
		}

		return score;
	}
}

void Dx12Context::Initialize()
{
	CreateFactory();
	CreateDevice();
	CreateQueue();
}

void Dx12Context::CreateFactory()
{
#if defined(_DEBUG)
	if (SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(m_factory.put()))))
	{
		return;
	}
#endif
	DX_CALL(CreateDXGIFactory2(0, IID_PPV_ARGS(m_factory.put())));
}

void Dx12Context::CreateDevice()
{
	std::vector<AdapterCandidate> candidates;

	winrt::com_ptr<IDXGIFactory6> factory6;
	m_factory.as(__uuidof(IDXGIFactory6), factory6.put_void());

	if (factory6)
	{
		for (UINT i = 0;; ++i)
		{
			winrt::com_ptr<IDXGIAdapter1> adapter;
			if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put())) == DXGI_ERROR_NOT_FOUND)
			{
				break;
			}
			AppendHardwareAdapter(candidates, adapter.get());
		}
	}

	if (candidates.empty())
	{
		for (UINT i = 0;; ++i)
		{
			winrt::com_ptr<IDXGIAdapter1> adapter;
			if (m_factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
			AppendHardwareAdapter(candidates, adapter.get());
		}
	}

	if (candidates.empty())
	{
		throw std::runtime_error("No hardware DXGI adapters were found.");
	}

	const bool preferDiscrete = !IsRunningOnBattery();

	for (auto& c : candidates)
	{
		c.score = ScoreAdapter(c.desc, preferDiscrete);
	}

	std::sort(candidates.begin(), candidates.end(),
			  [](const AdapterCandidate& a, const AdapterCandidate& b) { return a.score > b.score; });

	for (const auto& c : candidates)
	{
		if (SUCCEEDED(D3D12CreateDevice(c.adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(m_device.put()))))
		{
			return;
		}
	}

	throw std::runtime_error("No hardware adapter supports Direct3D 12 feature level 12_0.");
}

void Dx12Context::CreateQueue()
{
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	DX_CALL(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(m_queue.put())));
}
