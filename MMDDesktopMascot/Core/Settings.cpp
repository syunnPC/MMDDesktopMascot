#include "Settings.hpp"
#include "StringUtil.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string_view>

#include <algorithm>
#include <cmath>

namespace
{
	constexpr wchar_t kSettingsFileName[] = L"settings.ini";
	constexpr wchar_t kPresetsDirName[] = L"Presets";
	constexpr wchar_t kPhysicsPrefix[] = L"physics.";
	constexpr wchar_t kModelPresetPrefix[] = L"modelPreset_";
	constexpr int kMinTargetFps = 1;
	constexpr int kTrayMenuThemeIdMin = 0;
	constexpr int kTrayMenuThemeIdMax = 5;

	std::filesystem::path GetHashedPresetPath(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath);

	std::wstring Trim(const std::wstring& s)
	{
		const auto first = s.find_first_not_of(L" \t\r\n");
		if (first == std::wstring::npos) return L"";
		const auto last = s.find_last_not_of(L" \t\r\n");
		return s.substr(first, last - first + 1);
	}

	std::optional<std::wstring> ReadUtf8File(const std::filesystem::path& path)
	{
		std::ifstream fin(path, std::ios::binary);
		if (!fin) return std::nullopt;

		std::ostringstream buffer;
		buffer << fin.rdbuf();
		if (!fin && !fin.eof()) return std::nullopt;

		try
		{
			return StringUtil::Utf8ToWide(buffer.str());
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	bool WriteUtf8File(const std::filesystem::path& path, const std::wstring& content)
	{
		std::ofstream fout(path, std::ios::binary);
		if (!fout) return false;

		try
		{
			auto bytes = StringUtil::WideToUtf8(content);
			fout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(fout);
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	std::filesystem::path SettingsPath(const std::filesystem::path& baseDir)
	{
		return baseDir / kSettingsFileName;
	}

	std::filesystem::path PresetsDirPath(const std::filesystem::path& baseDir)
	{
		return baseDir / kPresetsDirName;
	}

	bool EnsurePresetsDirExists(const std::filesystem::path& baseDir)
	{
		const auto presetsDir = PresetsDirPath(baseDir);
		if (std::filesystem::exists(presetsDir))
		{
			return true;
		}

		std::error_code ec;
		return std::filesystem::create_directory(presetsDir, ec) || std::filesystem::exists(presetsDir);
	}

	std::filesystem::path GetPresetPath(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath)
	{
		return GetHashedPresetPath(baseDir, modelPath);
	}

	std::wstring ToLowerCopy(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(),
			[](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
		return value;
	}

	std::wstring SanitizePresetFileStem(std::wstring stem)
	{
		if (stem.empty())
		{
			stem = L"preset";
		}

		for (wchar_t& ch : stem)
		{
			switch (ch)
			{
				case L'<':
				case L'>':
				case L':':
				case L'"':
				case L'/':
				case L'\\':
				case L'|':
				case L'?':
				case L'*':
					ch = L'_';
					break;
				default:
					break;
			}
		}

		return stem;
	}

	std::uint64_t HashPresetKey(std::wstring_view key) noexcept
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (wchar_t ch : key)
		{
			hash ^= static_cast<std::uint64_t>(ch);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::wstring MakePresetFileName(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath)
	{
		const std::wstring key = SettingsManager::MakeModelPresetKey(baseDir, modelPath);
		if (key.empty())
		{
			return {};
		}

		std::wostringstream name;
		name << SanitizePresetFileStem(modelPath.stem().wstring())
			 << L"_"
			 << std::hex
			 << std::setw(16)
			 << std::setfill(L'0')
			 << HashPresetKey(key)
			 << L".ini";
		return name.str();
	}

	std::filesystem::path GetHashedPresetPath(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath)
	{
		if (modelPath.empty()) return {};
		const auto presetsDir = PresetsDirPath(baseDir);

		const std::wstring fileName = MakePresetFileName(baseDir, modelPath);
		if (fileName.empty())
		{
			return {};
		}

		return presetsDir / fileName;
	}

	float ParseFloat(const std::wstring& s, float defaultVal)
	{
		try
		{
			return std::stof(s);
		}
		catch (...)
		{
			return defaultVal;
		}
	}

	std::wstring FloatToWString(float v)
	{
		std::wostringstream oss;
		oss << v;
		return oss.str();
	}

	int ParseInt(const std::wstring& s, int defaultVal)
	{
		try
		{
			return std::stoi(s);
		}
		catch (...)
		{
			return defaultVal;
		}
	}

	std::wstring IntToWString(int v)
	{
		std::wostringstream oss;
		oss << v;
		return oss.str();
	}

	bool ParseBool(const std::wstring& s, bool defaultVal)
	{
		if (s == L"1" || s == L"true" || s == L"True") return true;
		if (s == L"0" || s == L"false" || s == L"False") return false;
		return defaultVal;
	}

	std::wstring BoolToWString(bool v)
	{
		return v ? L"1" : L"0";
	}

	bool NearlyEqual(float lhs, float rhs) noexcept
	{
		return std::abs(lhs - rhs) <= 1e-5f;
	}

	bool NearlyEqual(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs) noexcept
	{
		return NearlyEqual(lhs.x, rhs.x) &&
			NearlyEqual(lhs.y, rhs.y) &&
			NearlyEqual(lhs.z, rhs.z);
	}

	template <typename TObject>
	struct FloatFieldDesc
	{
		const wchar_t* key;
		float TObject::*member;
	};

	template <typename TObject>
	struct IntFieldDesc
	{
		const wchar_t* key;
		int TObject::*member;
	};

	template <typename TObject>
	struct BoolFieldDesc
	{
		const wchar_t* key;
		bool TObject::*member;
	};

	template <typename TObject, size_t N>
	bool ParseFloatField(const std::wstring& key,
						 const std::wstring& value,
						 TObject& object,
						 const FloatFieldDesc<TObject>(&fields)[N])
	{
		for (const auto& field : fields)
		{
			if (key == field.key)
			{
				float& target = object.*(field.member);
				target = ParseFloat(value, target);
				return true;
			}
		}
		return false;
	}

	template <typename TObject, size_t N>
	bool ParseIntField(const std::wstring& key,
					   const std::wstring& value,
					   TObject& object,
					   const IntFieldDesc<TObject>(&fields)[N])
	{
		for (const auto& field : fields)
		{
			if (key == field.key)
			{
				int& target = object.*(field.member);
				target = ParseInt(value, target);
				return true;
			}
		}
		return false;
	}

	template <typename TObject, size_t N>
	bool ParseBoolField(const std::wstring& key,
						const std::wstring& value,
						TObject& object,
						const BoolFieldDesc<TObject>(&fields)[N])
	{
		for (const auto& field : fields)
		{
			if (key == field.key)
			{
				bool& target = object.*(field.member);
				target = ParseBool(value, target);
				return true;
			}
		}
		return false;
	}

	template <typename TObject, size_t N>
	void WriteFloatFields(std::wostream& os,
						  const TObject& object,
						  const FloatFieldDesc<TObject>(&fields)[N],
						  std::wstring_view keyPrefix = {})
	{
		for (const auto& field : fields)
		{
			os << keyPrefix << field.key << L"=" << FloatToWString(object.*(field.member)) << L"\n";
		}
	}

	template <typename TObject, size_t N>
	void WriteIntFields(std::wostream& os,
						const TObject& object,
						const IntFieldDesc<TObject>(&fields)[N],
						std::wstring_view keyPrefix = {})
	{
		for (const auto& field : fields)
		{
			os << keyPrefix << field.key << L"=" << IntToWString(object.*(field.member)) << L"\n";
		}
	}

	template <typename TObject, size_t N>
	void WriteBoolFields(std::wostream& os,
						 const TObject& object,
						 const BoolFieldDesc<TObject>(&fields)[N],
						 std::wstring_view keyPrefix = {})
	{
		for (const auto& field : fields)
		{
			os << keyPrefix << field.key << L"=" << BoolToWString(object.*(field.member)) << L"\n";
		}
	}

	template <typename TObject, size_t N>
	bool CompareFloatFields(const TObject& lhs,
							const TObject& rhs,
							const FloatFieldDesc<TObject>(&fields)[N]) noexcept
	{
		for (const auto& field : fields)
		{
			if (!NearlyEqual(lhs.*(field.member), rhs.*(field.member)))
			{
				return false;
			}
		}

		return true;
	}

	template <typename TObject, size_t N>
	bool CompareIntFields(const TObject& lhs,
						  const TObject& rhs,
						  const IntFieldDesc<TObject>(&fields)[N]) noexcept
	{
		for (const auto& field : fields)
		{
			if (lhs.*(field.member) != rhs.*(field.member))
			{
				return false;
			}
		}

		return true;
	}

	template <typename TObject, size_t N>
	bool CompareBoolFields(const TObject& lhs,
						   const TObject& rhs,
						   const BoolFieldDesc<TObject>(&fields)[N]) noexcept
	{
		for (const auto& field : fields)
		{
			if (lhs.*(field.member) != rhs.*(field.member))
			{
				return false;
			}
		}

		return true;
	}

	constexpr FloatFieldDesc<LightSettings> kLightFloatFields[] = {
		{ L"brightness", &LightSettings::brightness },
		{ L"ambientStrength", &LightSettings::ambientStrength },
		{ L"globalSaturation", &LightSettings::globalSaturation },
		{ L"keyLightDirX", &LightSettings::keyLightDirX },
		{ L"keyLightDirY", &LightSettings::keyLightDirY },
		{ L"keyLightDirZ", &LightSettings::keyLightDirZ },
		{ L"keyLightColorR", &LightSettings::keyLightColorR },
		{ L"keyLightColorG", &LightSettings::keyLightColorG },
		{ L"keyLightColorB", &LightSettings::keyLightColorB },
		{ L"keyLightIntensity", &LightSettings::keyLightIntensity },
		{ L"fillLightDirX", &LightSettings::fillLightDirX },
		{ L"fillLightDirY", &LightSettings::fillLightDirY },
		{ L"fillLightDirZ", &LightSettings::fillLightDirZ },
		{ L"fillLightColorR", &LightSettings::fillLightColorR },
		{ L"fillLightColorG", &LightSettings::fillLightColorG },
		{ L"fillLightColorB", &LightSettings::fillLightColorB },
		{ L"fillLightIntensity", &LightSettings::fillLightIntensity },
		{ L"modelScale", &LightSettings::modelScale },
		{ L"outlineWidthScale", &LightSettings::outlineWidthScale },
		{ L"outlineOpacityScale", &LightSettings::outlineOpacityScale },
		{ L"toonContrast", &LightSettings::toonContrast },
		{ L"shadowHueShiftDeg", &LightSettings::shadowHueShiftDeg },
		{ L"shadowSaturationBoost", &LightSettings::shadowSaturationBoost },
		{ L"shadowRampShift", &LightSettings::shadowRampShift },
		{ L"rimWidth", &LightSettings::rimWidth },
		{ L"rimIntensity", &LightSettings::rimIntensity },
		{ L"specularStep", &LightSettings::specularStep },
		{ L"shadowDeepThreshold", &LightSettings::shadowDeepThreshold },
		{ L"shadowDeepSoftness", &LightSettings::shadowDeepSoftness },
		{ L"shadowDeepMul", &LightSettings::shadowDeepMul },
		{ L"faceShadowMul", &LightSettings::faceShadowMul },
		{ L"faceToonContrastMul", &LightSettings::faceToonContrastMul },
		{ L"ssaoIntensity", &LightSettings::ssaoIntensity },
		{ L"bloomIntensity", &LightSettings::bloomIntensity },
		{ L"exposure", &LightSettings::exposure },
		{ L"normalMapIntensity", &LightSettings::normalMapIntensity },
	};

	constexpr IntFieldDesc<LightSettings> kLightIntFields[] = {
		{ L"antiAliasingMode", &LightSettings::antiAliasingMode },
		{ L"msaaSampleCount", &LightSettings::msaaSampleCount },
		{ L"shadowMapSize", &LightSettings::shadowMapSize },
	};

	constexpr BoolFieldDesc<LightSettings> kLightBoolFields[] = {
		{ L"toonEnabled", &LightSettings::toonEnabled },
		{ L"selfShadowEnabled", &LightSettings::selfShadowEnabled },
		{ L"outlineEnabled", &LightSettings::outlineEnabled },
		{ L"faceMaterialOverridesEnabled", &LightSettings::faceMaterialOverridesEnabled },
		{ L"ssaoEnabled", &LightSettings::ssaoEnabled },
		{ L"bloomEnabled", &LightSettings::bloomEnabled },
		{ L"normalMapEnabled", &LightSettings::normalMapEnabled },
		{ L"filmicToneMapEnabled", &LightSettings::filmicToneMapEnabled },
	};

	constexpr FloatFieldDesc<PhysicsSettings> kPhysicsFloatFields[] = {
		{ L"fixedTimeStep", &PhysicsSettings::fixedTimeStep },
		{ L"kinematicPositionThreshold", &PhysicsSettings::kinematicPositionThreshold },
		{ L"kinematicRotationThreshold", &PhysicsSettings::kinematicRotationThreshold },
		{ L"minKinematicVelocityClip", &PhysicsSettings::minKinematicVelocityClip },
		{ L"jointStopErp", &PhysicsSettings::jointStopErp },
		{ L"ccdThresholdScale", &PhysicsSettings::ccdThresholdScale },
		{ L"sleepLinearThreshold", &PhysicsSettings::sleepLinearThreshold },
		{ L"sleepAngularThreshold", &PhysicsSettings::sleepAngularThreshold },
		{ L"writebackAngleThresholdDeg", &PhysicsSettings::writebackAngleThresholdDeg },
	};

	constexpr IntFieldDesc<PhysicsSettings> kPhysicsIntFields[] = {
		{ L"maxSubSteps", &PhysicsSettings::maxSubSteps },
		{ L"warmupSteps", &PhysicsSettings::warmupSteps },
	};

	constexpr IntFieldDesc<AppSettings> kAppIntFields[] = {
		{ L"targetFps", &AppSettings::targetFps },
		{ L"trayMenuTheme", &AppSettings::trayMenuThemeId },
		{ L"windowWidth", &AppSettings::windowWidth },
		{ L"windowHeight", &AppSettings::windowHeight },
	};

	constexpr BoolFieldDesc<AppSettings> kAppBoolFields[] = {
		{ L"alwaysOnTop", &AppSettings::alwaysOnTop },
		{ L"unlimitedFps", &AppSettings::unlimitedFps },
		{ L"limitFpsToMonitorRefreshRate", &AppSettings::limitFpsToMonitorRefreshRate },
		{ L"vsyncEnabled", &AppSettings::vsyncEnabled },
		{ L"lookAtEnabled", &AppSettings::lookAtEnabled },
		{ L"autoBlinkEnabled", &AppSettings::autoBlinkEnabled },
		{ L"breathingEnabled", &AppSettings::breathingEnabled },
	};

	void ParseLightSettingLine(const std::wstring& key, const std::wstring& value, LightSettings& light)
	{
		if (ParseFloatField(key, value, light, kLightFloatFields)) return;
		if (ParseIntField(key, value, light, kLightIntFields)) return;
		(void)ParseBoolField(key, value, light, kLightBoolFields);
	}

	void WriteLightSettings(std::wostream& os, const LightSettings& light)
	{
		WriteFloatFields(os, light, kLightFloatFields);
		WriteIntFields(os, light, kLightIntFields);
		WriteBoolFields(os, light, kLightBoolFields);
	}

	bool ParsePhysicsSettingLine(const std::wstring& key, const std::wstring& value, PhysicsSettings& physics)
	{
		if (key.rfind(kPhysicsPrefix, 0) != 0)
		{
			return false;
		}

		const size_t prefixLen = wcslen(kPhysicsPrefix);
		const std::wstring subKey = key.substr(prefixLen);

		if (subKey == L"gravityX")
		{
			physics.gravity.x = ParseFloat(value, physics.gravity.x);
			return true;
		}
		if (subKey == L"gravityY")
		{
			physics.gravity.y = ParseFloat(value, physics.gravity.y);
			return true;
		}
		if (subKey == L"gravityZ")
		{
			physics.gravity.z = ParseFloat(value, physics.gravity.z);
			return true;
		}

		if (ParseFloatField(subKey, value, physics, kPhysicsFloatFields)) return true;
		if (ParseIntField(subKey, value, physics, kPhysicsIntFields)) return true;

		return false;
	}

	void WritePhysicsSettings(std::wostream& os, const PhysicsSettings& physics)
	{
		constexpr std::wstring_view kPrefix = L"physics.";
		os << kPrefix << L"gravityX=" << FloatToWString(physics.gravity.x) << L"\n";
		os << kPrefix << L"gravityY=" << FloatToWString(physics.gravity.y) << L"\n";
		os << kPrefix << L"gravityZ=" << FloatToWString(physics.gravity.z) << L"\n";
		WriteFloatFields(os, physics, kPhysicsFloatFields, kPrefix);
		WriteIntFields(os, physics, kPhysicsIntFields, kPrefix);
	}

	int NormalizeTargetFps(int value) noexcept
	{
		return std::max(value, kMinTargetFps);
	}

	int NormalizeTrayMenuThemeId(int value) noexcept
	{
		return (value >= kTrayMenuThemeIdMin && value <= kTrayMenuThemeIdMax) ? value : 0;
	}

	void NormalizeAppSettings(AppSettings& settings) noexcept
	{
		settings.targetFps = NormalizeTargetFps(settings.targetFps);
		if (settings.limitFpsToMonitorRefreshRate || settings.vsyncEnabled)
		{
			settings.unlimitedFps = false;
		}
		settings.trayMenuThemeId = NormalizeTrayMenuThemeId(settings.trayMenuThemeId);
	}

	PresetMode ParsePresetModeValue(const std::wstring& value, PresetMode fallback)
	{
		switch (ParseInt(value, static_cast<int>(fallback)))
		{
			case static_cast<int>(PresetMode::Ask):
				return PresetMode::Ask;
			case static_cast<int>(PresetMode::AlwaysLoad):
				return PresetMode::AlwaysLoad;
			case static_cast<int>(PresetMode::NeverLoad):
				return PresetMode::NeverLoad;
			default:
				return fallback;
		}
	}

	std::filesystem::path MakeStoredModelPath(const std::filesystem::path& baseDir,
											  const std::filesystem::path& modelPath)
	{
		if (modelPath.empty() || !modelPath.is_absolute())
		{
			return modelPath;
		}

		std::error_code ec;
		auto relativePath = std::filesystem::relative(modelPath, baseDir, ec);
		return ec ? modelPath : relativePath;
	}

	std::wstring MakeModelPresetKeyImpl(const std::filesystem::path& baseDir,
										const std::filesystem::path& modelPath)
	{
		const auto storedModelPath = MakeStoredModelPath(baseDir, modelPath);
		if (storedModelPath.empty())
		{
			return {};
		}

		return ToLowerCopy(storedModelPath.lexically_normal().generic_wstring());
	}

	PresetMode ResolvePresetModeImpl(const AppSettings& settings,
									 const std::filesystem::path& baseDir,
									 const std::filesystem::path& modelPath)
	{
		const std::wstring key = MakeModelPresetKeyImpl(baseDir, modelPath);
		if (key.empty())
		{
			return settings.globalPresetMode;
		}

		const auto it = settings.perModelPresetSettings.find(key);
		return (it != settings.perModelPresetSettings.end())
			? it->second
			: settings.globalPresetMode;
	}

	void ParseAppSettingLine(const std::wstring& key,
							 const std::wstring& value,
							 AppSettings& settings)
	{
		if (key == L"model")
		{
			if (!value.empty())
			{
				settings.modelPath = value;
			}
			return;
		}

		if (key == L"trayMenuThemeId")
		{
			settings.trayMenuThemeId = ParseInt(value, settings.trayMenuThemeId);
			return;
		}

		if (ParseBoolField(key, value, settings, kAppBoolFields))
		{
			return;
		}

		if (key == L"globalPresetMode")
		{
			settings.globalPresetMode = ParsePresetModeValue(value, settings.globalPresetMode);
			return;
		}

		if (key.rfind(kModelPresetPrefix, 0) == 0)
		{
			const std::wstring filename = key.substr(wcslen(kModelPresetPrefix));
			if (!filename.empty())
			{
				settings.perModelPresetSettings[filename] = ParsePresetModeValue(value, PresetMode::Ask);
			}
			return;
		}

		if (ParseIntField(key, value, settings, kAppIntFields))
		{
			return;
		}

		if (!ParsePhysicsSettingLine(key, value, settings.physics))
		{
			ParseLightSettingLine(key, value, settings.light);
		}
	}

	void ParseAppSettings(std::wistream& input, AppSettings& settings)
	{
		std::wstring line;
		while (std::getline(input, line))
		{
			const auto separator = line.find(L'=');
			if (separator == std::wstring::npos)
			{
				continue;
			}

			const auto key = Trim(line.substr(0, separator));
			const auto value = Trim(line.substr(separator + 1));
			ParseAppSettingLine(key, value, settings);
		}

		NormalizeAppSettings(settings);
	}

	void WriteAppSettings(std::wostream& output,
						  const std::filesystem::path& baseDir,
						  const AppSettings& settings)
	{
		const auto storedModelPath = MakeStoredModelPath(baseDir, settings.modelPath);

		output << L"model=" << storedModelPath.wstring() << L"\n";
		WriteBoolFields(output, settings, kAppBoolFields);
		WriteIntFields(output, settings, kAppIntFields);
		output << L"globalPresetMode=" << IntToWString(static_cast<int>(settings.globalPresetMode)) << L"\n";

		for (const auto& [filename, mode] : settings.perModelPresetSettings)
		{
			output << kModelPresetPrefix << filename << L"=" << IntToWString(static_cast<int>(mode)) << L"\n";
		}

		WriteLightSettings(output, settings.light);
		WritePhysicsSettings(output, settings.physics);
	}

	void ParsePresetSettings(std::wistream& input,
							 LightSettings& outLightSettings,
							 PhysicsSettings& outPhysicsSettings)
	{
		std::wstring line;
		while (std::getline(input, line))
		{
			const auto separator = line.find(L'=');
			if (separator == std::wstring::npos)
			{
				continue;
			}

			const auto key = Trim(line.substr(0, separator));
			const auto value = Trim(line.substr(separator + 1));
			if (!ParsePhysicsSettingLine(key, value, outPhysicsSettings))
			{
				ParseLightSettingLine(key, value, outLightSettings);
			}
		}
	}
}

bool operator==(const LightSettings& a, const LightSettings& b) noexcept
{
	return CompareFloatFields(a, b, kLightFloatFields) &&
		CompareIntFields(a, b, kLightIntFields) &&
		CompareBoolFields(a, b, kLightBoolFields);
}

bool operator!=(const LightSettings& a, const LightSettings& b) noexcept
{
	return !(a == b);
}

bool operator==(const PhysicsSettings& a, const PhysicsSettings& b) noexcept
{
	return NearlyEqual(a.gravity, b.gravity) &&
		CompareFloatFields(a, b, kPhysicsFloatFields) &&
		CompareIntFields(a, b, kPhysicsIntFields);
}

bool operator!=(const PhysicsSettings& a, const PhysicsSettings& b) noexcept
{
	return !(a == b);
}

bool operator==(const AppSettings& a, const AppSettings& b) noexcept
{
	return a.modelPath == b.modelPath &&
		a.alwaysOnTop == b.alwaysOnTop &&
		a.targetFps == b.targetFps &&
		a.unlimitedFps == b.unlimitedFps &&
		a.limitFpsToMonitorRefreshRate == b.limitFpsToMonitorRefreshRate &&
		a.vsyncEnabled == b.vsyncEnabled &&
		a.lookAtEnabled == b.lookAtEnabled &&
		a.autoBlinkEnabled == b.autoBlinkEnabled &&
		a.breathingEnabled == b.breathingEnabled &&
		a.trayMenuThemeId == b.trayMenuThemeId &&
		a.windowWidth == b.windowWidth &&
		a.windowHeight == b.windowHeight &&
		a.globalPresetMode == b.globalPresetMode &&
		a.perModelPresetSettings == b.perModelPresetSettings &&
		a.light == b.light &&
		a.physics == b.physics;
}

bool operator!=(const AppSettings& a, const AppSettings& b) noexcept
{
	return !(a == b);
}

std::wstring SettingsManager::MakeModelPresetKey(const std::filesystem::path& baseDir,
												 const std::filesystem::path& modelPath)
{
	return MakeModelPresetKeyImpl(baseDir, modelPath);
}

PresetMode SettingsManager::ResolvePresetMode(const AppSettings& settings,
											  const std::filesystem::path& baseDir,
											  const std::filesystem::path& modelPath)
{
	return ResolvePresetModeImpl(settings, baseDir, modelPath);
}

AppSettings SettingsManager::Load(const std::filesystem::path& baseDir,
								  const std::filesystem::path& defaultModelPath)
{
	AppSettings settings{};
	settings.modelPath = defaultModelPath;
	settings.alwaysOnTop = true;

	const auto path = SettingsPath(baseDir);
	if (!std::filesystem::exists(path))
	{
		return settings;
	}

	const auto content = ReadUtf8File(path);
	if (!content)
	{
		return settings;
	}

	std::wstringstream input(*content);
	ParseAppSettings(input, settings);
	return settings;
}

void SettingsManager::Save(const std::filesystem::path& baseDir,
						   const AppSettings& settings)
{
	const auto path = SettingsPath(baseDir);
	std::wostringstream output;
	WriteAppSettings(output, baseDir, settings);
	WriteUtf8File(path, output.str());
}

bool SettingsManager::HasPreset(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath)
{
	const auto path = GetPresetPath(baseDir, modelPath);
	return !path.empty() && std::filesystem::exists(path);
}

void SettingsManager::SavePreset(const std::filesystem::path& baseDir,
								 const std::filesystem::path& modelPath,
								 const LightSettings& lightSettings,
								 const PhysicsSettings& physicsSettings)
{
	const auto path = GetPresetPath(baseDir, modelPath);
	if (path.empty())
	{
		return;
	}
	if (!EnsurePresetsDirExists(baseDir))
	{
		return;
	}

	std::wostringstream output;
	output << L"; Preset for " << modelPath.filename().wstring() << L"\n";
	WriteLightSettings(output, lightSettings);
	WritePhysicsSettings(output, physicsSettings);
	WriteUtf8File(path, output.str());
}

bool SettingsManager::LoadPreset(const std::filesystem::path& baseDir,
								 const std::filesystem::path& modelPath,
								 LightSettings& outLightSettings,
								 PhysicsSettings& outPhysicsSettings)
{
	auto path = GetPresetPath(baseDir, modelPath);
	if (path.empty() || !std::filesystem::exists(path))
	{
		return false;
	}

	const auto content = ReadUtf8File(path);
	if (!content)
	{
		return false;
	}

	std::wstringstream input(*content);
	ParsePresetSettings(input, outLightSettings, outPhysicsSettings);
	return true;
}
