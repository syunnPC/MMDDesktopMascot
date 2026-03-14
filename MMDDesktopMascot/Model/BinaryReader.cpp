#include "BinaryReader.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>

BinaryReader::BinaryReader(const std::filesystem::path& path)
{
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) throw std::runtime_error("Failed to open file.");

	ifs.seekg(0, std::ios::end);
	auto size = static_cast<std::streamsize>(ifs.tellg());
	ifs.seekg(0, std::ios::beg);

	if (size < 0) throw std::runtime_error("Invalid file size.");

#if defined _M_IX86 && !defined X86_NO_SAFE_MEMORY_SIZE
	if (size > 1 * 1024 * 1024 * 1024)
	{
		throw std::runtime_error("The model file is too large.");
	}
#endif

	m_buf.resize(static_cast<size_t>(size));
	if (!m_buf.empty())
	{
		ifs.read(reinterpret_cast<char*>(m_buf.data()), size);
		if (!ifs) throw std::runtime_error("Failed to read file.");
	}
}

size_t BinaryReader::Remaining() const
{
	if (m_pos > m_buf.size()) return 0;
	return m_buf.size() - m_pos;
}

void BinaryReader::Seek(size_t pos)
{
	if (pos > m_buf.size()) throw std::runtime_error("Seek out of range.");
	m_pos = pos;
}

void BinaryReader::Skip(size_t bytes)
{
	if (bytes > Remaining())
	{
		throw std::runtime_error("Skip out of range.");
	}

	m_pos += bytes;
}

std::span<const std::uint8_t> BinaryReader::ReadSpan(size_t n)
{
	if (n > Remaining()) throw std::runtime_error("ReadSpan out of range.");

	const auto* start = m_buf.data() + m_pos;
	m_pos += n;
	return { start, n };
}

std::vector<std::uint8_t> BinaryReader::ReadBytes(size_t n)
{
	const auto bytes = ReadSpan(n);
	return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::string BinaryReader::ReadStringUtf8WithLength()
{
	auto len = Read<std::int32_t>();
	if (len < 0) throw std::runtime_error("Negative string length.");
	const auto bytes = ReadSpan(static_cast<size_t>(len));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::u16string BinaryReader::ReadStringUtf16LeWithLength()
{
	auto lenBytes = Read<std::int32_t>();
	if (lenBytes < 0 || (lenBytes % 2) != 0) throw std::runtime_error("Invalid UTF16 length.");
	const auto bytes = ReadSpan(static_cast<size_t>(lenBytes));

	std::u16string s;
	s.resize(bytes.size() / 2);
	for (size_t i = 0; i < s.size(); ++i)
	{
		std::uint16_t lo = bytes[i * 2 + 0];
		std::uint16_t hi = bytes[i * 2 + 1];
		s[i] = static_cast<char16_t>((hi << 8) | lo);
	}
	return s;
}

template <class T>
T BinaryReader::Read()
{
	static_assert(std::is_trivially_copyable_v<T>);

	if (sizeof(T) > Remaining()) throw std::runtime_error("Read<T> out of range.");

	T v{};
	std::memcpy(&v, m_buf.data() + m_pos, sizeof(T));
	m_pos += sizeof(T);
	return v;
}

template std::uint8_t  BinaryReader::Read<std::uint8_t>();
template std::int32_t  BinaryReader::Read<std::int32_t>();
template std::uint32_t BinaryReader::Read<std::uint32_t>();
template float         BinaryReader::Read<float>();
template std::int8_t   BinaryReader::Read<std::int8_t>();
template std::int16_t  BinaryReader::Read<std::int16_t>();
template std::uint16_t BinaryReader::Read<std::uint16_t>();
