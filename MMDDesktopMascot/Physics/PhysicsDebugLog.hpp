#pragma once

#include <string>
#include <string_view>

namespace mmd::physics::debuglog
{
	bool IsEnabled();
	void Truncate();
	void AppendLine(std::string_view line);
	std::string ToUtf8Lossy(std::wstring_view text);
}
