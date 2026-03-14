#include "PhysicsDebugLog.hpp"
#include "StringUtil.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace mmd::physics::debuglog
{
	namespace
	{
		std::filesystem::path ResolveLogPath()
		{
			wchar_t buffer[32768]{};
			const DWORD size = GetEnvironmentVariableW(
				L"MMD_PHYSICS_DEBUG_LOG",
				buffer,
				static_cast<DWORD>(std::size(buffer)));
			if (size > 0 && size < std::size(buffer))
			{
				return std::filesystem::path(buffer);
			}

			return std::filesystem::path("physics_debug.log");
		}
	}

	bool IsEnabled()
	{
		static const bool enabled = []() -> bool
			{
				wchar_t buffer[8]{};
				const DWORD size = GetEnvironmentVariableW(
					L"MMD_PHYSICS_DEBUG",
					buffer,
					static_cast<DWORD>(std::size(buffer)));
				if (size == 0 || size >= std::size(buffer))
				{
					return false;
				}

				return buffer[0] == L'1';
			}();
		return enabled;
	}

	void Truncate()
	{
		if (!IsEnabled())
		{
			return;
		}

		std::ofstream ofs(ResolveLogPath(), std::ios::binary | std::ios::trunc);
		(void)ofs;
	}

	void AppendLine(std::string_view line)
	{
		if (!IsEnabled())
		{
			return;
		}

		std::ofstream ofs(ResolveLogPath(), std::ios::binary | std::ios::app);
		if (!ofs)
		{
			return;
		}

		ofs << line << '\n';
	}

	std::string ToUtf8Lossy(std::wstring_view text)
	{
		try
		{
			return StringUtil::WideToUtf8(std::wstring(text));
		}
		catch (const std::exception&)
		{
			return "<utf8_error>";
		}
	}
}
