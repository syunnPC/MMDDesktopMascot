#pragma once

#include <d3d12.h>
#include <winrt/base.h>

#include "Dx12Context.hpp"

class RenderPipelineManager
{
public:
	void Initialize(Dx12Context* ctx);

	void CreatePmxRootSignature();
	void CreatePmxPipeline(UINT msaaSampleCount, UINT msaaQuality);
	void CreateEdgePipeline(UINT msaaSampleCount, UINT msaaQuality);
	void CreateShadowPipeline();
	void CreatePostProcessRootSignature();
	void CreateSsaoPipeline();
	void CreateBloomPipeline();
	void CreateToneMapPipeline();

	ID3D12RootSignature* GetPmxRootSignature() const
	{
		return m_pmxRootSig.get();
	}
	ID3D12PipelineState* GetPmxPsoOpaque() const
	{
		return m_pmxPsoOpaque.get();
	}
	ID3D12PipelineState* GetPmxPsoOpaqueNoCull() const
	{
		return m_pmxPsoOpaqueNoCull.get();
	}
	ID3D12PipelineState* GetPmxPsoTrans() const
	{
		return m_pmxPsoTrans.get();
	}
	ID3D12PipelineState* GetPmxPsoTransNoCull() const
	{
		return m_pmxPsoTransNoCull.get();
	}
	ID3D12PipelineState* GetPmxPsoCutout() const
	{
		return m_pmxPsoCutout.get();
	}
	ID3D12PipelineState* GetPmxPsoCutoutNoCull() const
	{
		return m_pmxPsoCutoutNoCull.get();
	}
	ID3D12PipelineState* GetEdgePso() const
	{
		return m_edgePso.get();
	}
	ID3D12PipelineState* GetShadowPso() const
	{
		return m_shadowPso.get();
	}
	ID3D12PipelineState* GetShadowPsoNoCull() const
	{
		return m_shadowPsoNoCull.get();
	}
	ID3D12RootSignature* GetPostProcessRootSignature() const
	{
		return m_postProcessRootSig.get();
	}
	ID3D12PipelineState* GetSsaoPso() const
	{
		return m_ssaoPso.get();
	}
	ID3D12PipelineState* GetBloomDownsamplePso() const
	{
		return m_bloomDownPso.get();
	}
	ID3D12PipelineState* GetBloomBlurPso() const
	{
		return m_bloomBlurPso.get();
	}
	ID3D12PipelineState* GetToneMapPso() const
	{
		return m_toneMapPso.get();
	}

private:
	Dx12Context* m_ctx{};

	winrt::com_ptr<ID3D12RootSignature> m_pmxRootSig;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoOpaque;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoOpaqueNoCull;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoTrans;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoTransNoCull;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoCutout;
	winrt::com_ptr<ID3D12PipelineState> m_pmxPsoCutoutNoCull;
	winrt::com_ptr<ID3D12PipelineState> m_edgePso;
	winrt::com_ptr<ID3D12PipelineState> m_shadowPso;
	winrt::com_ptr<ID3D12PipelineState> m_shadowPsoNoCull;

	winrt::com_ptr<ID3D12RootSignature> m_postProcessRootSig;
	winrt::com_ptr<ID3D12PipelineState> m_ssaoPso;
	winrt::com_ptr<ID3D12PipelineState> m_bloomDownPso;
	winrt::com_ptr<ID3D12PipelineState> m_bloomBlurPso;
	winrt::com_ptr<ID3D12PipelineState> m_toneMapPso;
};
