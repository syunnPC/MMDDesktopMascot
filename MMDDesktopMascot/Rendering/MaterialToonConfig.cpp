#include "MaterialToonConfig.hpp"

#include "StringUtil.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <variant>

#include <windows.h>

namespace
{
	struct JsonValue;
	using JsonArray = std::vector<JsonValue>;
	using JsonObject = std::map<std::wstring, JsonValue>;

	struct JsonValue
	{
		using Storage = std::variant<std::nullptr_t, bool, double, std::wstring, JsonArray, JsonObject>;
		Storage value;

		const JsonObject* AsObject() const
		{
			return std::get_if<JsonObject>(&value);
		}

		const JsonArray* AsArray() const
		{
			return std::get_if<JsonArray>(&value);
		}

		const std::wstring* AsString() const
		{
			return std::get_if<std::wstring>(&value);
		}

		const double* AsNumber() const
		{
			return std::get_if<double>(&value);
		}

		const bool* AsBool() const
		{
			return std::get_if<bool>(&value);
		}
	};

	class JsonParser
	{
	public:
		explicit JsonParser(std::wstring_view text) : m_text(text) {}

		std::optional<JsonValue> Parse()
		{
			SkipWhitespace();
			auto value = ParseValue();
			if (!value)
			{
				return std::nullopt;
			}
			SkipWhitespace();
			return (m_pos == m_text.size()) ? value : std::nullopt;
		}

	private:
		std::optional<JsonValue> ParseValue()
		{
			SkipWhitespace();
			if (m_pos >= m_text.size())
			{
				return std::nullopt;
			}

			const wchar_t ch = m_text[m_pos];
			if (ch == L'{') return ParseObject();
			if (ch == L'[') return ParseArray();
			if (ch == L'"')
			{
				auto s = ParseString();
				if (!s) return std::nullopt;
				return JsonValue{ *s };
			}
			if (ch == L't') return ConsumeLiteral(L"true") ? std::optional<JsonValue>(JsonValue{ true }) : std::nullopt;
			if (ch == L'f') return ConsumeLiteral(L"false") ? std::optional<JsonValue>(JsonValue{ false }) : std::nullopt;
			if (ch == L'n') return ConsumeLiteral(L"null") ? std::optional<JsonValue>(JsonValue{ nullptr }) : std::nullopt;
			if (ch == L'-' || (ch >= L'0' && ch <= L'9')) return ParseNumber();
			return std::nullopt;
		}

		std::optional<JsonValue> ParseObject()
		{
			if (!Consume(L'{')) return std::nullopt;

			JsonObject object;
			SkipWhitespace();
			if (Consume(L'}'))
			{
				return JsonValue{ object };
			}

			while (m_pos < m_text.size())
			{
				SkipWhitespace();
				auto key = ParseString();
				if (!key || !Consume(L':'))
				{
					return std::nullopt;
				}

				auto value = ParseValue();
				if (!value)
				{
					return std::nullopt;
				}
				object.emplace(std::move(*key), std::move(*value));

				SkipWhitespace();
				if (Consume(L'}'))
				{
					return JsonValue{ object };
				}
				if (!Consume(L','))
				{
					return std::nullopt;
				}
			}

			return std::nullopt;
		}

		std::optional<JsonValue> ParseArray()
		{
			if (!Consume(L'[')) return std::nullopt;

			JsonArray array;
			SkipWhitespace();
			if (Consume(L']'))
			{
				return JsonValue{ array };
			}

			while (m_pos < m_text.size())
			{
				auto value = ParseValue();
				if (!value)
				{
					return std::nullopt;
				}
				array.push_back(std::move(*value));

				SkipWhitespace();
				if (Consume(L']'))
				{
					return JsonValue{ array };
				}
				if (!Consume(L','))
				{
					return std::nullopt;
				}
			}

			return std::nullopt;
		}

		std::optional<std::wstring> ParseString()
		{
			if (!Consume(L'"')) return std::nullopt;

			std::wstring result;
			while (m_pos < m_text.size())
			{
				wchar_t ch = m_text[m_pos++];
				if (ch == L'"')
				{
					return result;
				}
				if (ch != L'\\')
				{
					result.push_back(ch);
					continue;
				}

				if (m_pos >= m_text.size())
				{
					return std::nullopt;
				}
				const wchar_t escaped = m_text[m_pos++];
				switch (escaped)
				{
					case L'"': result.push_back(L'"'); break;
					case L'\\': result.push_back(L'\\'); break;
					case L'/': result.push_back(L'/'); break;
					case L'b': result.push_back(L'\b'); break;
					case L'f': result.push_back(L'\f'); break;
					case L'n': result.push_back(L'\n'); break;
					case L'r': result.push_back(L'\r'); break;
					case L't': result.push_back(L'\t'); break;
					case L'u':
					{
						if (m_pos + 4 > m_text.size()) return std::nullopt;
						unsigned int code = 0;
						for (int i = 0; i < 4; ++i)
						{
							const wchar_t h = m_text[m_pos++];
							code <<= 4;
							if (h >= L'0' && h <= L'9') code += static_cast<unsigned int>(h - L'0');
							else if (h >= L'a' && h <= L'f') code += static_cast<unsigned int>(h - L'a' + 10);
							else if (h >= L'A' && h <= L'F') code += static_cast<unsigned int>(h - L'A' + 10);
							else return std::nullopt;
						}
						result.push_back(static_cast<wchar_t>(code));
						break;
					}
					default:
						return std::nullopt;
				}
			}

			return std::nullopt;
		}

		std::optional<JsonValue> ParseNumber()
		{
			const size_t start = m_pos;
			if (m_text[m_pos] == L'-') ++m_pos;
			while (m_pos < m_text.size() && std::iswdigit(m_text[m_pos])) ++m_pos;
			if (m_pos < m_text.size() && m_text[m_pos] == L'.')
			{
				++m_pos;
				while (m_pos < m_text.size() && std::iswdigit(m_text[m_pos])) ++m_pos;
			}
			if (m_pos < m_text.size() && (m_text[m_pos] == L'e' || m_text[m_pos] == L'E'))
			{
				++m_pos;
				if (m_pos < m_text.size() && (m_text[m_pos] == L'+' || m_text[m_pos] == L'-')) ++m_pos;
				while (m_pos < m_text.size() && std::iswdigit(m_text[m_pos])) ++m_pos;
			}

			const std::wstring token(m_text.substr(start, m_pos - start));
			wchar_t* end = nullptr;
			const double value = std::wcstod(token.c_str(), &end);
			if (!end || *end != L'\0')
			{
				return std::nullopt;
			}
			return JsonValue{ value };
		}

		bool Consume(wchar_t ch)
		{
			SkipWhitespace();
			if (m_pos < m_text.size() && m_text[m_pos] == ch)
			{
				++m_pos;
				return true;
			}
			return false;
		}

		bool ConsumeLiteral(std::wstring_view literal)
		{
			if (m_text.substr(m_pos, literal.size()) == literal)
			{
				m_pos += literal.size();
				return true;
			}
			return false;
		}

		void SkipWhitespace()
		{
			while (m_pos < m_text.size())
			{
				const wchar_t ch = m_text[m_pos];
				if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n')
				{
					break;
				}
				++m_pos;
			}
		}

		std::wstring_view m_text;
		size_t m_pos{};
	};

	std::optional<std::wstring> ReadUtf8File(const std::filesystem::path& path)
	{
		std::ifstream fin(path, std::ios::binary);
		if (!fin) return std::nullopt;

		std::ostringstream buffer;
		buffer << fin.rdbuf();
		if (!fin && !fin.eof()) return std::nullopt;

		try
		{
			std::wstring text = StringUtil::Utf8ToWide(buffer.str());
			if (!text.empty() && text.front() == L'\xfeff')
			{
				text.erase(text.begin());
			}
			return text;
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	std::wstring ToLowerW(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(),
					   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
		return value;
	}

	const JsonValue* FindMember(const JsonObject& object, std::wstring_view key)
	{
		const auto it = object.find(std::wstring(key));
		return (it != object.end()) ? &it->second : nullptr;
	}

	std::optional<float> ReadFloat(const JsonObject& object, std::wstring_view key)
	{
		const JsonValue* value = FindMember(object, key);
		if (!value) return std::nullopt;
		if (const double* number = value->AsNumber())
		{
			return static_cast<float>(*number);
		}
		return std::nullopt;
	}

	std::optional<bool> ReadBool(const JsonObject& object, std::wstring_view key)
	{
		const JsonValue* value = FindMember(object, key);
		if (!value) return std::nullopt;
		if (const bool* boolean = value->AsBool())
		{
			return *boolean;
		}
		return std::nullopt;
	}

	std::optional<std::wstring> ReadString(const JsonObject& object, std::wstring_view key)
	{
		const JsonValue* value = FindMember(object, key);
		if (!value) return std::nullopt;
		if (const std::wstring* string = value->AsString())
		{
			return *string;
		}
		return std::nullopt;
	}

	std::optional<DirectX::XMFLOAT3> ReadFloat3(const JsonObject& object, std::wstring_view key)
	{
		const JsonValue* value = FindMember(object, key);
		const JsonArray* array = value ? value->AsArray() : nullptr;
		if (!array || array->size() < 3) return std::nullopt;

		const double* x = (*array)[0].AsNumber();
		const double* y = (*array)[1].AsNumber();
		const double* z = (*array)[2].AsNumber();
		if (!x || !y || !z) return std::nullopt;
		return DirectX::XMFLOAT3{ static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z) };
	}

	std::optional<DirectX::XMFLOAT4> ReadFloat4(const JsonObject& object, std::wstring_view key)
	{
		const JsonValue* value = FindMember(object, key);
		const JsonArray* array = value ? value->AsArray() : nullptr;
		if (!array || array->size() < 3) return std::nullopt;

		const double* x = (*array)[0].AsNumber();
		const double* y = (*array)[1].AsNumber();
		const double* z = (*array)[2].AsNumber();
		if (!x || !y || !z) return std::nullopt;

		float w = 1.0f;
		if (array->size() >= 4)
		{
			if (const double* a = (*array)[3].AsNumber())
			{
				w = static_cast<float>(*a);
			}
		}
		return DirectX::XMFLOAT4{ static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z), w };
	}

	std::optional<uint32_t> ParseMaterialClass(const std::wstring& value)
	{
		const std::wstring lower = ToLowerW(value);
		if (lower == L"cloth" || lower == L"default") return 0u;
		if (lower == L"skin") return 1u;
		if (lower == L"hair") return 2u;
		if (lower == L"face") return 3u;
		if (lower == L"eye" || lower == L"eyes") return 4u;
		if (lower == L"transparent" || lower == L"glass") return 5u;
		return std::nullopt;
	}

	MaterialToonOverride ParseOverrideObject(const JsonObject& object)
	{
		MaterialToonOverride override{};

		if (auto materialClass = ReadString(object, L"class"))
		{
			if (auto parsed = ParseMaterialClass(*materialClass))
			{
				override.hasClass = true;
				override.materialClass = *parsed;
			}
		}

		if (auto shadowMode = ReadString(object, L"shadowMode"))
		{
			if (ToLowerW(*shadowMode) == L"fixed")
			{
				override.shadowColorWeight = 1.0f;
			}
		}
		if (auto shadowColor = ReadFloat3(object, L"shadowColor"))
		{
			override.hasShadowColor = true;
			override.shadowColor = *shadowColor;
		}
		if (auto weight = ReadFloat(object, L"shadowColorWeight"))
		{
			override.shadowColorWeight = *weight;
		}

		if (auto value = ReadFloat(object, L"rimMask"))
		{
			override.hasRimMask = true;
			override.rimMask = *value;
		}
		if (auto value = ReadFloat(object, L"skinLightInfluence"))
		{
			override.hasSkinLightInfluence = true;
			override.skinLightInfluence = *value;
		}
		if (auto value = ReadFloat(object, L"shadowHueShift"))
		{
			override.hasShadowHueShift = true;
			override.shadowHueShift = *value;
		}
		if (auto value = ReadFloat(object, L"shadowSaturationScale"))
		{
			override.hasShadowSaturationScale = true;
			override.shadowSaturationScale = *value;
		}
		if (auto value = ReadFloat(object, L"shadowValueScale"))
		{
			override.hasShadowValueScale = true;
			override.shadowValueScale = *value;
		}
		if (auto value = ReadFloat(object, L"specularScale"))
		{
			override.hasSpecularScale = true;
			override.specularScale = *value;
		}
		if (auto value = ReadFloat(object, L"specularIntensity"))
		{
			override.hasSpecularScale = true;
			override.specularScale = *value;
		}
		if (auto value = ReadString(object, L"specularMode"))
		{
			const std::wstring lower = ToLowerW(*value);
			override.hairBandSpecular = (lower == L"hairband" || lower == L"hair_band" || lower == L"hair");
			override.disableSpecular = (lower == L"none" || lower == L"off");
		}
		if (auto value = ReadFloat(object, L"hairSpecCenter"))
		{
			override.hasHairSpecCenter = true;
			override.hairSpecCenter = *value;
		}
		if (auto value = ReadFloat(object, L"hairSpecWidth"))
		{
			override.hasHairSpecWidth = true;
			override.hairSpecWidth = *value;
		}
		if (auto value = ReadFloat(object, L"hairSpecIntensity"))
		{
			override.hasHairSpecIntensity = true;
			override.hairSpecIntensity = *value;
		}
		if (auto value = ReadString(object, L"hairSpecMask"))
		{
			override.hasHairSpecMask = true;
			override.hairSpecMask = *value;
		}
		if (auto value = ReadFloat4(object, L"edgeColor"))
		{
			override.hasEdgeColor = true;
			override.edgeColor = *value;
		}
		else if (auto outlineColor = ReadFloat4(object, L"outlineColor"))
		{
			override.hasEdgeColor = true;
			override.edgeColor = *outlineColor;
		}
		if (auto value = ReadFloat(object, L"edgeSize"))
		{
			override.hasEdgeSize = true;
			override.edgeSize = *value;
		}
		else if (auto outlineWidth = ReadFloat(object, L"outlineWidth"))
		{
			override.hasEdgeSize = true;
			override.edgeSize = *outlineWidth;
		}
		if (auto value = ReadBool(object, L"edgeEnabled"))
		{
			override.hasEdgeEnabled = true;
			override.edgeEnabled = *value;
		}
		if (auto value = ReadBool(object, L"alphaCutout"))
		{
			override.hasAlphaCutout = true;
			override.alphaCutout = *value;
		}

		return override;
	}

	void AppendConfigPaths(std::vector<std::filesystem::path>& paths, const std::filesystem::path& modelPath)
	{
		if (modelPath.empty()) return;
		const auto modelDir = modelPath.parent_path();
		const auto stem = modelPath.stem();

		paths.push_back(modelPath.parent_path() / (stem.wstring() + L".toon.json"));
		paths.push_back(modelPath.parent_path() / (stem.wstring() + L".material.json"));
		paths.push_back(modelDir / L"toon_materials.json");
		paths.push_back(modelDir / L"materials.toon.json");
	}

	bool IsDefaultKey(const std::wstring& key)
	{
		const std::wstring lower = ToLowerW(key);
		return lower == L"*" || lower == L"default" || lower == L"__default";
	}
}

MaterialToonConfig MaterialToonConfig::LoadForModel(const std::filesystem::path& modelPath)
{
	MaterialToonConfig config;

	std::vector<std::filesystem::path> paths;
	AppendConfigPaths(paths, modelPath);
	for (const auto& path : paths)
	{
		if (!std::filesystem::exists(path))
		{
			continue;
		}

		const auto content = ReadUtf8File(path);
		if (!content)
		{
			OutputDebugStringW((L"MaterialToonConfig: failed to read " + path.wstring() + L"\n").c_str());
			continue;
		}

		JsonParser parser(*content);
		const auto rootValue = parser.Parse();
		const JsonObject* root = rootValue ? rootValue->AsObject() : nullptr;
		const JsonValue* materialsValue = root ? FindMember(*root, L"materials") : nullptr;
		const JsonObject* materials = materialsValue ? materialsValue->AsObject() : nullptr;
		if (!materials)
		{
			OutputDebugStringW((L"MaterialToonConfig: missing materials object in " + path.wstring() + L"\n").c_str());
			continue;
		}

		for (const auto& [key, value] : *materials)
		{
			const JsonObject* object = value.AsObject();
			if (!object)
			{
				continue;
			}

			MaterialToonOverride override = ParseOverrideObject(*object);
			if (IsDefaultKey(key))
			{
				config.m_defaultOverride = override;
			}
			else
			{
				config.m_entries.push_back({ key, override });
			}
		}

		break;
	}

	return config;
}

const MaterialToonOverride* MaterialToonConfig::FindOverride(const PmxModel::Material& material) const
{
	auto matches = [&](const Entry& entry) {
		const std::wstring key = ToLowerW(entry.key);
		return key == ToLowerW(material.name) ||
			key == ToLowerW(material.nameEn) ||
			(!material.memo.empty() && ToLowerW(material.memo).find(key) != std::wstring::npos);
	};

	const auto it = std::find_if(m_entries.begin(), m_entries.end(), matches);
	return (it != m_entries.end())
		? &it->value
		: (m_defaultOverride ? &*m_defaultOverride : nullptr);
}
