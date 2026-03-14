#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

class DcompRenderer;
class PmxModel;

namespace mmd::app
{
	struct ModelLoadResult
	{
		std::unique_ptr<PmxModel> model;
		std::wstring errorMessage;
	};

	using ProgressCallback = std::function<void(float progress, const wchar_t* message)>;

	ModelLoadResult LoadModelWithTextures(
		const std::filesystem::path& path,
		DcompRenderer& renderer,
		const ProgressCallback& onProgress,
		std::stop_token stopToken);
}
