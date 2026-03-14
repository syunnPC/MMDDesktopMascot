#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <format>
#include <stdexcept>
#include <string>

// Helper utility converts D3D API failures into exceptions.
inline void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr))
	{
		throw std::runtime_error(std::format("{:#010X}", static_cast<unsigned long>(hr)));
	}
}

inline void ThrowIfFailedEx(HRESULT hr, const char* expr, const char* file, int line)
{
	if (SUCCEEDED(hr)) return;

	std::string msg = std::format("{} failed. hr=0x{:08X} ({}:{})", expr, (unsigned)hr, file, line);
	OutputDebugStringA(msg.c_str());
	OutputDebugStringA("\n");

	throw std::runtime_error(msg);
}

#define DX_CALL(x) ThrowIfFailedEx((x), #x, FILENAME, __LINE__)
