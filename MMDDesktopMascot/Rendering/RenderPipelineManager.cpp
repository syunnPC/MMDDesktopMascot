#include "RenderPipelineManager.hpp"

#include "d3dx12.h"
#include "ExceptionHelper.hpp"
#include "FileUtil.hpp"
#include "DebugUtil.hpp"

#include <d3dcompiler.h>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	constexpr DXGI_FORMAT kSceneRenderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr DXGI_FORMAT kPresentRenderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
	const D3D12_INPUT_ELEMENT_DESC kPmxInputLayout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_SINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 76, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 88, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 4, DXGI_FORMAT_R32_UINT, 0, 100, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 5, DXGI_FORMAT_R32_FLOAT, 0, 104, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 108, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 124, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 140, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 9, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 156, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 172, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	std::optional<WPARAM> g_deferredQuitCode;

	void PumpPendingWindowMessages()
	{
		MSG msg{};
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				if (!g_deferredQuitCode)
				{
					g_deferredQuitCode = msg.wParam;
				}
				continue;
			}

			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	struct DeferredQuitMessageRestorer
	{
		~DeferredQuitMessageRestorer()
		{
			if (g_deferredQuitCode)
			{
				PostQuitMessage(static_cast<int>(*g_deferredQuitCode));
				g_deferredQuitCode.reset();
			}
		}
	};

	template <typename TWork>
	auto RunWithMessagePump(TWork&& work)
	{
		DeferredQuitMessageRestorer restoreQuitMessage;
		auto future = std::async(std::launch::async, std::forward<TWork>(work));

		for (;;)
		{
			PumpPendingWindowMessages();

			if (future.wait_for(std::chrono::milliseconds(15)) == std::future_status::ready)
			{
				PumpPendingWindowMessages();
				return future.get();
			}
		}
	}

	std::filesystem::path NormalizePath(const std::filesystem::path& path)
	{
		std::error_code ec;
		const auto normalized = std::filesystem::weakly_canonical(path, ec);
		return ec ? path.lexically_normal() : normalized;
	}

	std::string TrimAscii(std::string_view text)
	{
		size_t first = 0;
		while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])))
		{
			++first;
		}

		size_t last = text.size();
		while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
		{
			--last;
		}

		return std::string(text.substr(first, last - first));
	}

	std::optional<std::filesystem::path> ParseIncludePath(std::string_view line)
	{
		const std::string trimmed = TrimAscii(line);
		if (trimmed.empty() || trimmed[0] != '#')
		{
			return std::nullopt;
		}

		std::string directive = TrimAscii(std::string_view(trimmed).substr(1));
		constexpr std::string_view includeKeyword = "include";
		if (directive.size() < includeKeyword.size() ||
			std::string_view(directive).substr(0, includeKeyword.size()) != includeKeyword)
		{
			return std::nullopt;
		}

		directive = TrimAscii(std::string_view(directive).substr(includeKeyword.size()));
		if (directive.size() < 3 || directive.front() != '"')
		{
			return std::nullopt;
		}

		const size_t closingQuote = directive.find('"', 1);
		if (closingQuote == std::string::npos || closingQuote <= 1)
		{
			return std::nullopt;
		}

		return std::filesystem::path(std::string(directive.substr(1, closingQuote - 1)));
	}

	void CollectShaderDependenciesRecursive(
		const std::filesystem::path& sourcePath,
		std::vector<std::filesystem::path>& outDependencies,
		std::unordered_set<std::wstring>& visited)
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		if (!visited.insert(normalizedPath.wstring()).second)
		{
			return;
		}

		outDependencies.push_back(normalizedPath);

		std::ifstream input(normalizedPath);
		if (!input)
		{
			return;
		}

		std::string line;
		while (std::getline(input, line))
		{
			const auto includePath = ParseIncludePath(line);
			if (!includePath)
			{
				continue;
			}

			const std::filesystem::path resolvedInclude = normalizedPath.parent_path() / *includePath;
			CollectShaderDependenciesRecursive(resolvedInclude, outDependencies, visited);
		}
	}

	std::vector<std::filesystem::path> CollectShaderDependencies(const std::filesystem::path& sourcePath)
	{
		std::vector<std::filesystem::path> dependencies;
		std::unordered_set<std::wstring> visited;
		CollectShaderDependenciesRecursive(sourcePath, dependencies, visited);
		return dependencies;
	}

	std::filesystem::path ResolveShaderSourcePath(std::wstring_view fileName)
	{
		const std::filesystem::path executableDir = FileUtil::GetExecutableDir();
		const std::array candidates = {
			executableDir.parent_path().parent_path() / L"MMDDesktopMascot" / std::filesystem::path(fileName),
			std::filesystem::current_path() / L"MMDDesktopMascot" / std::filesystem::path(fileName),
			executableDir / L"Shaders" / std::filesystem::path(fileName),
		};

		for (const auto& candidate : candidates)
		{
			if (std::filesystem::exists(candidate))
			{
				return candidate;
			}
		}

		return candidates.back();
	}

	std::filesystem::path ResolveShaderCachePath(std::wstring_view fileName)
	{
		const std::filesystem::path cacheDir = FileUtil::GetExecutableDir() / L"Shaders";
		std::error_code ec;
		std::filesystem::create_directories(cacheDir, ec);
		return cacheDir / std::filesystem::path(fileName);
	}

	bool NeedsShaderRecompile(const std::filesystem::path& sourcePath, const std::filesystem::path& compiledPath)
	{
		if (!std::filesystem::exists(compiledPath))
		{
			return true;
		}

		std::error_code compiledEc;
		const auto compiledTime = std::filesystem::last_write_time(compiledPath, compiledEc);
		if (compiledEc)
		{
			return true;
		}

		for (const auto& dependency : CollectShaderDependencies(sourcePath))
		{
			std::error_code dependencyEc;
			const auto dependencyTime = std::filesystem::last_write_time(dependency, dependencyEc);
			if (!dependencyEc && dependencyTime > compiledTime)
			{
				return true;
			}
		}

		return false;
	}

	winrt::com_ptr<ID3DBlob> LoadOrCompileShader(
		std::wstring_view sourceFileName,
		std::wstring_view compiledFileName,
		const char* entryPoint,
		const char* profile,
		UINT compileFlags,
		const char* debugLabel)
	{
		const std::filesystem::path sourcePath = ResolveShaderSourcePath(sourceFileName);
		const std::filesystem::path compiledPath = ResolveShaderCachePath(compiledFileName);

		winrt::com_ptr<ID3DBlob> shaderBlob;

		HRESULT hr = E_FAIL;
		const bool needsRecompile = NeedsShaderRecompile(sourcePath, compiledPath);
		if (!needsRecompile && std::filesystem::exists(compiledPath))
		{
			hr = D3DReadFileToBlob(compiledPath.c_str(), shaderBlob.put());
		}

		if (needsRecompile || FAILED(hr) || !shaderBlob)
		{
			// Keep the UI thread pumping messages while shader compilation runs so
			// Windows does not flag the startup window as hung.
			shaderBlob = RunWithMessagePump([=]() {
				winrt::com_ptr<ID3DBlob> compiledShaderBlob;
				winrt::com_ptr<ID3DBlob> compileErrorBlob;

				const HRESULT compileHr = D3DCompileFromFile(
					sourcePath.c_str(),
					nullptr,
					D3D_COMPILE_STANDARD_FILE_INCLUDE,
					entryPoint,
					profile,
					compileFlags,
					0,
					compiledShaderBlob.put(),
					compileErrorBlob.put());

				if (FAILED(compileHr))
				{
					if (compileErrorBlob)
					{
						OutputDebugStringA(static_cast<char*>(compileErrorBlob->GetBufferPointer()));
					}
					ThrowIfFailedEx(compileHr, debugLabel, FILENAME, __LINE__);
				}

				ThrowIfFailedEx(
					D3DWriteBlobToFile(compiledShaderBlob.get(), compiledPath.c_str(), TRUE),
					"Write compiled shader cache",
					FILENAME,
					__LINE__);

				return compiledShaderBlob;
			});
		}

		return shaderBlob;
	}

	D3D12_SHADER_BYTECODE MakeShaderBytecode(ID3DBlob* blob) noexcept
	{
		return D3D12_SHADER_BYTECODE{
			.pShaderBytecode = blob ? blob->GetBufferPointer() : nullptr,
			.BytecodeLength = blob ? blob->GetBufferSize() : 0
		};
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC MakeScenePipelineDesc(
		ID3D12RootSignature* rootSignature,
		ID3DBlob* vertexShader,
		ID3DBlob* pixelShader,
		UINT sampleCount,
		UINT sampleQuality)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = rootSignature;
		pso.VS = MakeShaderBytecode(vertexShader);
		pso.PS = MakeShaderBytecode(pixelShader);
		pso.InputLayout = { kPmxInputLayout, _countof(kPmxInputLayout) };
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		pso.DepthStencilState.DepthEnable = TRUE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

		auto& renderTargetBlend = pso.BlendState.RenderTarget[0];
		renderTargetBlend.BlendEnable = TRUE;
		renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
		renderTargetBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
		renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
		renderTargetBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;

		pso.NumRenderTargets = 1;
		pso.RTVFormats[0] = kSceneRenderTargetFormat;
		pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pso.SampleDesc.Count = sampleCount;
		pso.SampleDesc.Quality = sampleQuality;
		pso.SampleMask = UINT_MAX;
		return pso;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC MakeShadowPipelineDesc(
		ID3D12RootSignature* rootSignature,
		ID3DBlob* vertexShader,
		ID3DBlob* pixelShader)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = rootSignature;
		pso.VS = MakeShaderBytecode(vertexShader);
		pso.PS = MakeShaderBytecode(pixelShader);
		pso.InputLayout = { kPmxInputLayout, _countof(kPmxInputLayout) };
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		pso.DepthStencilState.DepthEnable = TRUE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		pso.SampleMask = UINT_MAX;
		pso.NumRenderTargets = 0;
		pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pso.SampleDesc.Count = 1;
		return pso;
	}

	void CreateGraphicsPipelineState(
		ID3D12Device* device,
		const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
		winrt::com_ptr<ID3D12PipelineState>& pipelineState)
	{
		DX_CALL(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipelineState.put())));
	}
}

void RenderPipelineManager::Initialize(Dx12Context* ctx)
{
	m_ctx = ctx;
}

void RenderPipelineManager::CreatePmxRootSignature()
{
	CD3DX12_ROOT_PARAMETER params[5]{};

	params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
	params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_DESCRIPTOR_RANGE srvRange{};
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0);
	params[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

	params[3].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);

	CD3DX12_DESCRIPTOR_RANGE shadowRange{};
	shadowRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0);
	params[4].InitAsDescriptorTable(1, &shadowRange, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC samplers[3]{};
	samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
	samplers[0].MaxAnisotropy = 16;
	samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	samplers[0].MinLOD = 0.0f;
	samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
	samplers[0].MipLODBias = -0.5f;
	samplers[0].ShaderRegister = 0;
	samplers[0].RegisterSpace = 0;
	samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	samplers[1] = samplers[0];
	samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[1].MaxAnisotropy = 1;
	samplers[1].MipLODBias = 0.0f;
	samplers[1].ShaderRegister = 1;

	samplers[2] = samplers[1];
	samplers[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	samplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	samplers[2].ShaderRegister = 2;

	CD3DX12_ROOT_SIGNATURE_DESC rsDesc{};
	rsDesc.Init(
		_countof(params), params,
		_countof(samplers), samplers,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	winrt::com_ptr<ID3DBlob> sigBlob;
	winrt::com_ptr<ID3DBlob> errBlob;

	HRESULT hr = D3D12SerializeRootSignature(
		&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		sigBlob.put(), errBlob.put());

	if (FAILED(hr))
	{
		if (errBlob)
		{
			OutputDebugStringA(static_cast<char*>(errBlob->GetBufferPointer()));
		}
		ThrowIfFailedEx(hr, "D3D12SerializeRootSignature", FILENAME, __LINE__);
	}

	DX_CALL(m_ctx->Device()->CreateRootSignature(
		0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
		IID_PPV_ARGS(m_pmxRootSig.put())));
}

void RenderPipelineManager::CreatePmxPipeline(UINT msaaSampleCount, UINT msaaQuality)
{
	if (!m_pmxRootSig)
	{
		CreatePmxRootSignature();
	}

	const auto vsBlob = LoadOrCompileShader(
		L"PMX_VS.hlsl",
		L"Compiled_PMX_VS.cso",
		"VSMain",
		"vs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile PMX VS");
	const auto psBlob = LoadOrCompileShader(
		L"PMX_PS.hlsl",
		L"Compiled_PMX_PS.cso",
		"PSMain",
		"ps_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile PMX PS");

	auto MakeBaseDesc = [&]() {
		return MakeScenePipelineDesc(
			m_pmxRootSig.get(),
			vsBlob.get(),
			psBlob.get(),
			msaaSampleCount,
			msaaQuality);
	};

	{
		auto pso = MakeBaseDesc();
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoOpaque);
	}
	{
		auto pso = MakeBaseDesc();
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoOpaqueNoCull);
	}
	{
		auto pso = MakeBaseDesc();
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoTrans);
	}
	{
		auto pso = MakeBaseDesc();
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoTransNoCull);
	}
	{
		auto pso = MakeBaseDesc();
		pso.BlendState.AlphaToCoverageEnable = (msaaSampleCount > 1);
		auto& rt = pso.BlendState.RenderTarget[0];
		rt.BlendEnable = FALSE;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ZERO;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ZERO;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoCutout);
	}
	{
		auto pso = MakeBaseDesc();
		pso.BlendState.AlphaToCoverageEnable = (msaaSampleCount > 1);
		auto& rt = pso.BlendState.RenderTarget[0];
		rt.BlendEnable = FALSE;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ZERO;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ZERO;
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_pmxPsoCutoutNoCull);
	}
}

void RenderPipelineManager::CreateEdgePipeline(UINT msaaSampleCount, UINT msaaQuality)
{
	if (!m_pmxRootSig)
	{
		CreatePmxRootSignature();
	}

	const auto vsBlob = LoadOrCompileShader(
		L"Edge_VS.hlsl",
		L"Compiled_Edge_VS.cso",
		"VSMain",
		"vs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Edge VS");
	const auto psBlob = LoadOrCompileShader(
		L"Edge_PS.hlsl",
		L"Compiled_Edge_PS.cso",
		"PSMain",
		"ps_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Edge PS");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = MakeScenePipelineDesc(
		m_pmxRootSig.get(),
		vsBlob.get(),
		psBlob.get(),
		msaaSampleCount,
		msaaQuality);
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	CreateGraphicsPipelineState(m_ctx->Device(), pso, m_edgePso);
}

void RenderPipelineManager::CreateShadowPipeline()
{
	if (!m_pmxRootSig)
	{
		CreatePmxRootSignature();
	}

	const auto vsBlob = LoadOrCompileShader(
		L"Shadow_VS.hlsl",
		L"Compiled_Shadow_VS.cso",
		"VSMain",
		"vs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Shadow VS");
	const auto psBlob = LoadOrCompileShader(
		L"Shadow_PS.hlsl",
		L"Compiled_Shadow_PS.cso",
		"PSMain",
		"ps_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Shadow PS");

	auto makeDesc = [&]() {
		return MakeShadowPipelineDesc(
			m_pmxRootSig.get(),
			vsBlob.get(),
			psBlob.get());
	};

	{
		auto pso = makeDesc();
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_shadowPso);
	}
	{
		auto pso = makeDesc();
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		CreateGraphicsPipelineState(m_ctx->Device(), pso, m_shadowPsoNoCull);
	}
}

void RenderPipelineManager::CreatePostProcessRootSignature()
{
	CD3DX12_ROOT_PARAMETER params[6]{};

	params[0].InitAsConstants(16, 0);

	CD3DX12_DESCRIPTOR_RANGE sceneSrvRange{};
	sceneSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	params[1].InitAsDescriptorTable(1, &sceneSrvRange, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_DESCRIPTOR_RANGE ssaoSrvRange{};
	ssaoSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
	params[2].InitAsDescriptorTable(1, &ssaoSrvRange, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_DESCRIPTOR_RANGE bloomSrvRange{};
	bloomSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
	params[3].InitAsDescriptorTable(1, &bloomSrvRange, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_DESCRIPTOR_RANGE depthSrvRange{};
	depthSrvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
	params[4].InitAsDescriptorTable(1, &depthSrvRange, D3D12_SHADER_VISIBILITY_ALL);

	CD3DX12_DESCRIPTOR_RANGE uavRange{};
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[5].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

	D3D12_STATIC_SAMPLER_DESC samplers[2]{};
	samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samplers[0].ShaderRegister = 0;
	samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	samplers[1] = samplers[0];
	samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	samplers[1].ShaderRegister = 1;

	CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init(6, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	winrt::com_ptr<ID3DBlob> sigBlob, errBlob;
	DX_CALL(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, sigBlob.put(), errBlob.put()));
	DX_CALL(m_ctx->Device()->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(m_postProcessRootSig.put())));
}

void RenderPipelineManager::CreateSsaoPipeline()
{
	if (!m_postProcessRootSig)
	{
		CreatePostProcessRootSignature();
	}

	const auto csBlob = LoadOrCompileShader(
		L"SSAO_CS.hlsl",
		L"Compiled_SSAO_CS.cso",
		"MainCS",
		"cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile SSAO CS");

	D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = m_postProcessRootSig.get();
	pso.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

	DX_CALL(m_ctx->Device()->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_ssaoPso.put())));
}

void RenderPipelineManager::CreateBloomPipeline()
{
	if (!m_postProcessRootSig)
	{
		CreatePostProcessRootSignature();
	}

	const auto downBlob = LoadOrCompileShader(
		L"BloomDownsample_CS.hlsl",
		L"Compiled_BloomDownsample_CS.cso",
		"MainCS",
		"cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Bloom Downsample CS");

	D3D12_COMPUTE_PIPELINE_STATE_DESC downPso{};
	downPso.pRootSignature = m_postProcessRootSig.get();
	downPso.CS = { downBlob->GetBufferPointer(), downBlob->GetBufferSize() };
	DX_CALL(m_ctx->Device()->CreateComputePipelineState(&downPso, IID_PPV_ARGS(m_bloomDownPso.put())));

	const auto blurBlob = LoadOrCompileShader(
		L"BloomBlur_CS.hlsl",
		L"Compiled_BloomBlur_CS.cso",
		"MainCS",
		"cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile Bloom Blur CS");

	D3D12_COMPUTE_PIPELINE_STATE_DESC blurPso{};
	blurPso.pRootSignature = m_postProcessRootSig.get();
	blurPso.CS = { blurBlob->GetBufferPointer(), blurBlob->GetBufferSize() };
	DX_CALL(m_ctx->Device()->CreateComputePipelineState(&blurPso, IID_PPV_ARGS(m_bloomBlurPso.put())));
}

void RenderPipelineManager::CreateToneMapPipeline()
{
	if (!m_postProcessRootSig)
	{
		CreatePostProcessRootSignature();
	}

	const auto vsBlob = LoadOrCompileShader(
		L"FXAA_VS.hlsl",
		L"Compiled_FXAA_VS.cso",
		"VSMain",
		"vs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile ToneMap VS");
	const auto psBlob = LoadOrCompileShader(
		L"ToneMap_PS.hlsl",
		L"Compiled_ToneMap_PS.cso",
		"PSMain",
		"ps_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3,
		"D3DCompile ToneMap PS");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
	pso.pRootSignature = m_postProcessRootSig.get();
	pso.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
	pso.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
	pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pso.DepthStencilState.DepthEnable = FALSE;
	pso.DepthStencilState.StencilEnable = FALSE;
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = kPresentRenderTargetFormat;
	pso.SampleDesc.Count = 1;

	DX_CALL(m_ctx->Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(m_toneMapPso.put())));
}
