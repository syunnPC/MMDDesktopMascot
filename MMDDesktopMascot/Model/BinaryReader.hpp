#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <filesystem>
#include <span>

class BinaryReader
{
public:
	explicit BinaryReader(const std::filesystem::path& path);

	size_t Remaining() const;
	size_t Position() const
	{
		return m_pos;
	}

	void Seek(size_t pos);
	void Skip(size_t bytes);

	template <class T>
	T Read();

	std::span<const std::uint8_t> ReadSpan(size_t n);
	std::vector<std::uint8_t> ReadBytes(size_t n);

	std::string ReadStringUtf8WithLength();
	std::u16string ReadStringUtf16LeWithLength();

private:
	const std::vector<std::uint8_t>& Buf() const
	{
		return m_buf;
	}

	std::vector<std::uint8_t> m_buf;
	size_t m_pos{};
};
