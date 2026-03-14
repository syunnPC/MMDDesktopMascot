#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

class Dx12Context
{
public:
	void Initialize();
	ID3D12Device* Device() const
	{
		return m_device.get();
	}
	IDXGIFactory4* Factory() const
	{
		return m_factory.get();
	}
	ID3D12CommandQueue* Queue() const
	{
		return m_queue.get();
	}

private:
	void CreateFactory();
	void CreateDevice();
	void CreateQueue();

	winrt::com_ptr<IDXGIFactory4> m_factory;
	winrt::com_ptr<ID3D12Device> m_device;
	winrt::com_ptr<ID3D12CommandQueue> m_queue;
};
