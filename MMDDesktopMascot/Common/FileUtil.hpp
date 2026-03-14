#pragma once
#include <filesystem>
#include <string>

namespace FileUtil
{
	std::filesystem::path GetExecutablePath();
	std::filesystem::path GetExecutableDir();
	bool IEquals(const std::wstring& a, const std::wstring& b);
}
