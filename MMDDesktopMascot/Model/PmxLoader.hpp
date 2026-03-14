#pragma once

#include <filesystem>
#include "PmxModel.hpp"

class PmxLoader
{
public:
	static bool LoadModel(const std::filesystem::path& pmxPath, PmxModel& outModel, PmxModel::ProgressCallback onProgress = nullptr);
};