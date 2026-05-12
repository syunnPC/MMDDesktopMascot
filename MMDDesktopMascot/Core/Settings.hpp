#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <DirectXMath.h>

enum class PresetMode
{
	Ask = 0,
	AlwaysLoad = 1,
	NeverLoad = 2
};

enum class AntiAliasingMode
{
	Off = 0,
	Fxaa = 1,
	Msaa = 2,
	MsaaFxaa = 3,
	Smaa = 4,
	MsaaSmaa = 5
};

enum class ToonDebugView
{
	Final = 0,
	BaseColor = 1,
	ShadowColor = 2,
	NdotL = 3,
	SpecularMask = 4,
	Normal = 5,
	MaterialId = 6,
	OutlineMask = 7,
	DepthEdge = 8,
	NormalEdge = 9,
	MaterialEdge = 10,
	FinalOutline = 11,
	Rim = 12
};

enum class RenderResolutionMode
{
	ClientSize = 0,
	ScalePercent = 1,
	Custom = 2
};

struct LightSettings
{
	float brightness = 1.5f;
	float ambientStrength = 0.55f;

	float globalSaturation = 1.1f;


	float keyLightDirX = 0.25f;
	float keyLightDirY = 0.85f;
	float keyLightDirZ = -0.5f;
	float keyLightColorR = 1.0f;
	float keyLightColorG = 1.0f;
	float keyLightColorB = 1.0f;
	float keyLightIntensity = 1.2f;


	float fillLightDirX = -0.65f;
	float fillLightDirY = 0.25f;
	float fillLightDirZ = -0.15f;
	float fillLightColorR = 1.0f;
	float fillLightColorG = 1.0f;
	float fillLightColorB = 1.0f;
	float fillLightIntensity = 0.65f;


	float modelScale = 1.0f;
	float outlineWidthScale = 1.0f;
	float outlineOpacityScale = 1.0f;
	float outlineSilhouetteAngleTolerance = 0.0f;
	float outlineNonSilhouetteWidthScale = 1.0f;
	float outlineNonSilhouetteOpacityScale = 1.0f;
	float outlineNonSilhouetteAngleTolerance = 0.45f;


	bool toonEnabled = true;
	bool selfShadowEnabled = true;
	bool outlineEnabled = true;
	bool outlineSilhouetteModeEnabled = false;
	bool outlineNonSilhouetteEnabled = true;
	int antiAliasingMode = static_cast<int>(AntiAliasingMode::MsaaFxaa);
	int msaaSampleCount = 2;
	int shadowMapSize = 1024;
	float toonContrast = 1.15f;
	float shadowHueShiftDeg = -8.0f;
	float shadowSaturationBoost = 0.25f;
	float rimWidth = 0.6f;
	float rimIntensity = 0.25f;
	float specularStep = 0.3f;

	float shadowRampShift = 0.0f;

	float shadowDeepThreshold = 0.28f;
	float shadowDeepSoftness = 0.03f;
	float shadowDeepMul = 0.65f;

	// Post-processing settings
	bool ssaoEnabled = true;
	float ssaoIntensity = 0.65f;
	bool bloomEnabled = true;
	float bloomIntensity = 0.35f;
	float exposure = 1.0f;
	bool normalMapEnabled = true;
	float normalMapIntensity = 1.0f;
	bool filmicToneMapEnabled = true;
	int toonDebugView = static_cast<int>(ToonDebugView::Final);
};

enum class CollisionMaskSemantics : int
{
	Auto   = 0,
	Direct = 1,
	Inverse = 2
};

struct PhysicsSettings
{
	float fixedTimeStep{ 1.0f / 60.0f };
	int maxSubSteps{ 4 };
	int warmupSteps{ 60 };

	DirectX::XMFLOAT3 gravity{ 0.0f, -98.0f, 0.0f };

	float kinematicPositionThreshold{ 1.0e-12f };
	float kinematicRotationThreshold{ 1.0e-8f };
	float minKinematicVelocityClip{ 1.0e-4f };
	float jointStopErp{ 0.475f };
	float ccdThresholdScale{ 1.0f };
	float sleepLinearThreshold{ 0.1f };
	float sleepAngularThreshold{ 0.1f };
	float writebackAngleThresholdDeg{ 0.0f };

	int collisionMaskSemantics{ static_cast<int>(CollisionMaskSemantics::Auto) };
	float globalDampingScale{ 1.0f };
	float velocitySettleThreshold{ 1.0e-3f };
	float minAngularDamping{ 0.05f };
};

bool operator==(const LightSettings& a, const LightSettings& b) noexcept;
bool operator!=(const LightSettings& a, const LightSettings& b) noexcept;
bool operator==(const PhysicsSettings& a, const PhysicsSettings& b) noexcept;
bool operator!=(const PhysicsSettings& a, const PhysicsSettings& b) noexcept;

struct AppSettings
{
	std::filesystem::path modelPath;
	bool alwaysOnTop{ true };
	int targetFps{ 60 };
	bool unlimitedFps{ false };
	bool limitFpsToMonitorRefreshRate{ false };
	bool vsyncEnabled{ false };
	bool lookAtEnabled{ false };
	bool autoBlinkEnabled{ false };
	bool breathingEnabled{ false };



	int trayMenuThemeId{ 0 };


	int windowWidth{ 0 };
	int windowHeight{ 0 };
	int renderResolutionMode{ static_cast<int>(RenderResolutionMode::ClientSize) };
	int renderScalePercent{ 100 };
	int renderCustomWidth{ 0 };
	int renderCustomHeight{ 0 };


	PresetMode globalPresetMode{ PresetMode::Ask };
	std::map<std::wstring, PresetMode> perModelPresetSettings;

	LightSettings light;
	PhysicsSettings physics;
};

bool operator==(const AppSettings& a, const AppSettings& b) noexcept;
bool operator!=(const AppSettings& a, const AppSettings& b) noexcept;

class SettingsManager
{
public:
	static std::wstring MakeModelPresetKey(const std::filesystem::path& baseDir,
										   const std::filesystem::path& modelPath);
	static PresetMode ResolvePresetMode(const AppSettings& settings,
										const std::filesystem::path& baseDir,
										const std::filesystem::path& modelPath);

	static AppSettings Load(const std::filesystem::path& baseDir,
							const std::filesystem::path& defaultModelPath);

	static void Save(const std::filesystem::path& baseDir,
					 const AppSettings& settings);


	static bool HasPreset(const std::filesystem::path& baseDir, const std::filesystem::path& modelPath);


	static void SavePreset(const std::filesystem::path& baseDir,
						   const std::filesystem::path& modelPath,
						   const LightSettings& lightSettings,
						   const PhysicsSettings& physicsSettings);


	static bool LoadPreset(const std::filesystem::path& baseDir,
						   const std::filesystem::path& modelPath,
						   LightSettings& outLightSettings,
						   PhysicsSettings& outPhysicsSettings);
};
