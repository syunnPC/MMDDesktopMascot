#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "FileUtil.hpp"
#include <windows.h>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <format>

namespace FileUtil
{

	std::filesystem::path GetExecutablePath()
	{
		std::wstring buf;
		buf.resize(32768);

		DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
		if (len == 0 || len >= buf.size())
		{
			throw std::runtime_error("GetModuleFileNameW failed.");
		}
		buf.resize(len);
		return std::filesystem::path(buf);
	}

	std::filesystem::path GetExecutableDir()
	{
		auto p = GetExecutablePath();
		return p.parent_path();
	}

	bool IEquals(const std::wstring& a, const std::wstring& b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if (towlower(a[i]) != towlower(b[i])) return false;
		}
		return true;
	}
}
