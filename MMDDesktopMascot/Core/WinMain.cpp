#include "App.hpp"
#include "BoneSolver.hpp"
#include "FileUtil.hpp"
#include "MmdPhysicsWorld.hpp"
#include "PhysicsDebugLog.hpp"
#include "PmxLoader.hpp"
#include "Settings.hpp"
#include "StringUtil.hpp"
#include "Win32UiUtil.hpp"

#include <array>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#define PROFILER_ATTACHED

namespace
{
	struct PhysicsDiagnosticOptions
	{
		bool enabled{ false };
		std::filesystem::path modelPath;
		std::filesystem::path outputPath;
		int frames{ 600 };
	};

	void SetProcessEcoQoS(bool enable)
	{
		PROCESS_POWER_THROTTLING_STATE state{};
		state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask = enable ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;

		SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
	}

	class ScopedComInitializer
	{
	public:
		ScopedComInitializer()
			: m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
		{
		}

		~ScopedComInitializer()
		{
			if (SUCCEEDED(m_result))
			{
				CoUninitialize();
			}
		}

		bool Initialized() const
		{
			return SUCCEEDED(m_result);
		}

		bool ChangedMode() const
		{
			return m_result == RPC_E_CHANGED_MODE;
		}

	private:
		HRESULT m_result{};
	};

	std::vector<std::wstring> GetCommandLineArgs()
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
		{
			return {};
		}

		std::vector<std::wstring> args;
		args.reserve(static_cast<size_t>(argc));
		for (int i = 0; i < argc; ++i)
		{
			args.emplace_back(argv[i]);
		}

		LocalFree(argv);
		return args;
	}

	bool HasCommandLineOption(const std::vector<std::wstring>& args, const wchar_t* option)
	{
		for (size_t i = 1; i < args.size(); ++i)
		{
			if (_wcsicmp(args[i].c_str(), option) == 0)
			{
				return true;
			}
		}

		return false;
	}

	const std::wstring* FindCommandLineValue(const std::vector<std::wstring>& args, const wchar_t* option)
	{
		for (size_t i = 1; i + 1 < args.size(); ++i)
		{
			if (_wcsicmp(args[i].c_str(), option) == 0)
			{
				return &args[i + 1];
			}
		}

		return nullptr;
	}

	int ParsePositiveIntOrDefault(const std::wstring& text, int fallback)
	{
		try
		{
			const int value = std::stoi(text);
			return (value > 0) ? value : fallback;
		}
		catch (...)
		{
			return fallback;
		}
	}

	std::filesystem::path ResolveModelPath(
		const std::filesystem::path& baseDir,
		const AppSettings& settings,
		const std::filesystem::path& overridePath)
	{
		std::filesystem::path modelPath = overridePath.empty() ? settings.modelPath : overridePath;
		if (modelPath.empty())
		{
			modelPath = baseDir / L"Models" / L"default.pmx";
		}
		else if (!modelPath.is_absolute())
		{
			modelPath = baseDir / modelPath;
		}

		return modelPath;
	}

	bool TryParsePhysicsDiagnosticOptions(
		const std::vector<std::wstring>& args,
		PhysicsDiagnosticOptions& outOptions)
	{
		if (!HasCommandLineOption(args, L"--diagnose-physics") &&
			!HasCommandLineOption(args, L"--dump-physics"))
		{
			return false;
		}

		outOptions.enabled = true;
		if (const std::wstring* model = FindCommandLineValue(args, L"--model"))
		{
			outOptions.modelPath = *model;
		}
		if (const std::wstring* output = FindCommandLineValue(args, L"--output"))
		{
			outOptions.outputPath = *output;
		}
		if (const std::wstring* frames = FindCommandLineValue(args, L"--frames"))
		{
			outOptions.frames = ParsePositiveIntOrDefault(*frames, outOptions.frames);
		}

		return true;
	}

	int RunPhysicsDiagnostic(const PhysicsDiagnosticOptions& options)
	{
		const std::filesystem::path baseDir = FileUtil::GetExecutableDir();
		const std::filesystem::path defaultModel = baseDir / L"Models" / L"default.pmx";
		AppSettings settings = SettingsManager::Load(baseDir, defaultModel);
		settings.modelPath = ResolveModelPath(baseDir, settings, {});

		const std::filesystem::path modelPath = ResolveModelPath(baseDir, settings, options.modelPath);
		const std::filesystem::path outputPath = options.outputPath.empty()
			? (baseDir / L"physics_debug.log")
			: (options.outputPath.is_absolute() ? options.outputPath : (baseDir / options.outputPath));

		PhysicsSettings physicsSettings = settings.physics;
		std::wstring presetNote = L"none";
		if (SettingsManager::HasPreset(baseDir, modelPath))
		{
			const PresetMode presetMode = SettingsManager::ResolvePresetMode(settings, baseDir, modelPath);
			if (presetMode == PresetMode::AlwaysLoad)
			{
				LightSettings ignoredLight = settings.light;
				if (SettingsManager::LoadPreset(baseDir, modelPath, ignoredLight, physicsSettings))
				{
					presetNote = L"applied";
				}
			}
			else if (presetMode == PresetMode::Ask)
			{
				presetNote = L"present_but_skipped_ask_mode";
			}
			else
			{
				presetNote = L"present_but_disabled";
			}
		}

		if (!std::filesystem::exists(modelPath))
		{
			throw std::runtime_error("Diagnostic model path does not exist.");
		}

		SetEnvironmentVariableW(L"MMD_PHYSICS_DEBUG", L"1");
		SetEnvironmentVariableW(L"MMD_PHYSICS_DEBUG_LOG", outputPath.wstring().c_str());

		PmxModel model;
		if (!PmxLoader::LoadModel(modelPath, model))
		{
			throw std::runtime_error("Failed to load PMX model for physics diagnosis.");
		}

		BoneSolver boneSolver;
		boneSolver.Initialize(&model);
		BonePose bindPose{};
		boneSolver.ApplyPose(bindPose);
		boneSolver.UpdateMatricesBeforePhysics();

		MmdPhysicsWorld physicsWorld;
		physicsWorld.GetSettings() = physicsSettings;
		physicsWorld.BuildFromModel(model, boneSolver);

		std::ostringstream diagHeader;
		diagHeader << "DiagnosticRun"
			<< " model=\"" << mmd::physics::debuglog::ToUtf8Lossy(modelPath.wstring()) << "\""
			<< " frames=" << options.frames
			<< " fixedTimeStep=" << physicsSettings.fixedTimeStep
			<< " preset=\"" << mmd::physics::debuglog::ToUtf8Lossy(presetNote) << "\"";
		mmd::physics::debuglog::AppendLine(diagHeader.str());

		const double dt = (physicsSettings.fixedTimeStep > 0.0f)
			? static_cast<double>(physicsSettings.fixedTimeStep)
			: (1.0 / 60.0);
		const std::vector<float> morphWeights(model.Morphs().size(), 0.0f);
		for (int frame = 0; frame < options.frames; ++frame)
		{
			physicsWorld.Step(dt, model, boneSolver, morphWeights);
			boneSolver.UpdateMatricesAfterPhysics();
		}

		std::ostringstream diagFooter;
		diagFooter << "DiagnosticComplete"
			<< " frames=" << options.frames
			<< " output=\"" << mmd::physics::debuglog::ToUtf8Lossy(outputPath.wstring()) << "\"";
		mmd::physics::debuglog::AppendLine(diagFooter.str());
		return 0;
	}

	bool HasEnvVar(const wchar_t* name)
	{
		wchar_t buffer[2];
		SetLastError(ERROR_SUCCESS);
		const DWORD size = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
		return size != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
	}

	void RelaunchWithOpenMpPassiveIfNeeded()
	{
		constexpr const wchar_t* kMarker = L"MMD_OMP_BOOTSTRAP_DONE";
		if (HasEnvVar(kMarker))
		{
			return;
		}

		wchar_t policy[64]{};
		const DWORD size = GetEnvironmentVariableW(L"OMP_WAIT_POLICY", policy, static_cast<DWORD>(std::size(policy)));
		if (size > 0 && _wcsicmp(policy, L"PASSIVE") == 0)
		{
			SetEnvironmentVariableW(kMarker, L"1");
			return;
		}

		SetEnvironmentVariableW(L"OMP_WAIT_POLICY", L"PASSIVE");
		SetEnvironmentVariableW(kMarker, L"1");

		std::wstring commandLine = GetCommandLineW();

		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};

		if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &processInfo))
		{
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			ExitProcess(0);
		}
		else
		{
			OutputDebugStringW(L"CreateProcessW() failed.");
			if (MessageBoxW(
				nullptr,
				L"Failed to set OpenMP environment variable. Performance may be reduced. Continue?",
				L"MMDDesktopMascot",
				MB_ICONINFORMATION | MB_YESNO) == IDNO)
			{
				ExitProcess(0);
			}
			SetEnvironmentVariableW(kMarker, nullptr);
		}
	}
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
	try
	{
		const std::vector<std::wstring> args = GetCommandLineArgs();
		PhysicsDiagnosticOptions diagnosticOptions{};
		if (TryParsePhysicsDiagnosticOptions(args, diagnosticOptions))
		{
			return RunPhysicsDiagnostic(diagnosticOptions);
		}

		Win32UiUtil::RequireWindows11();

#ifdef __AVX2__
		if (!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))
		{
			throw std::runtime_error("AVX2 instruction set is not available on this CPU.");
		}
#endif

		Win32UiUtil::EnablePerMonitorDpiAwareness();
		Win32UiUtil::InitializeDarkModeSupport();

		if (!IsDebuggerPresent() && !HasCommandLineOption(args, L"--no-restart"))
		{
			RelaunchWithOpenMpPassiveIfNeeded();
		}

		// This process is animation/rendering heavy. Background throttling hurts frame pacing
		// noticeably on hybrid/mobile CPUs, so keep the default scheduler policy.
		SetProcessEcoQoS(false);

		ScopedComInitializer comInitializer;
		if (!comInitializer.Initialized() && !comInitializer.ChangedMode())
		{
			throw std::runtime_error("Failed to initialize COM.");
		}

		App app{ hInstance };
		return app.Run();
	}
	catch (const std::exception& exception)
	{
		std::wstring message = L"Fatal error:\n";
		message += StringUtil::ExceptionMessageToWide(exception);
		MessageBoxW(nullptr, message.c_str(), L"MMDDesktopMascot", MB_ICONERROR);
		OutputDebugStringW(message.c_str());
		return -1;
	}
	catch (...)
	{
		MessageBoxW(nullptr, L"Fatal error: unknown exception occurred.", L"MMDDesktopMascot", MB_ICONERROR);
		return -1;
	}
}
