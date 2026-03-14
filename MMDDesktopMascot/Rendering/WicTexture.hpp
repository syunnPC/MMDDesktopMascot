#pragma once
#include <filesystem>
#include <vector>
#include <cstdint>

struct WicImage
{
	uint32_t width{};
	uint32_t height{};
	std::vector<uint8_t> rgba; // size = width * height * 4
	bool hasTransparentPixels{};
	bool hasTranslucentPixels{};
};

namespace WicTexture
{
	WicImage LoadRgba(const std::filesystem::path& path);
}
