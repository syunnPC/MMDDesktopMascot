#include "ModelLoadWorker.hpp"

#include "DcompRenderer.hpp"
#include "PhysicsDebugLog.hpp"
#include "PmxModel.hpp"
#include "StringUtil.hpp"

#include <exception>
#include <format>
#include <sstream>
#include <windows.h>

namespace mmd::app
{
	namespace
	{
		void LogModelFeatureSummary(const std::filesystem::path& path, const PmxModel& model)
		{
			size_t boneMorphCount = 0;
			size_t flipMorphCount = 0;
			size_t impulseMorphCount = 0;
			size_t externalParentBoneCount = 0;
			size_t unsupportedSoftBodyShapeCount = 0;
			unsigned softBodyFlagsMask = 0;

			for (const auto& morph : model.Morphs())
			{
				switch (morph.type)
				{
				case PmxModel::Morph::Type::Bone:
					++boneMorphCount;
					break;
				case PmxModel::Morph::Type::Flip:
					++flipMorphCount;
					break;
				case PmxModel::Morph::Type::Impulse:
					++impulseMorphCount;
					break;
				default:
					break;
				}
			}

			for (const auto& bone : model.Bones())
			{
				if (bone.IsExternalParent())
				{
					++externalParentBoneCount;
				}
			}

			for (const auto& softBody : model.SoftBodies())
			{
				softBodyFlagsMask |= softBody.flags;
				if (softBody.shape != 0)
				{
					++unsupportedSoftBodyShapeCount;
				}
			}

			std::ostringstream oss;
			oss << "Model Features: file=\"" << mmd::physics::debuglog::ToUtf8Lossy(path.filename().wstring()) << "\""
				<< " bones=" << model.Bones().size()
				<< " morphs=" << model.Morphs().size()
				<< " boneMorphs=" << boneMorphCount
				<< " flipMorphs=" << flipMorphCount
				<< " impulseMorphs=" << impulseMorphCount
				<< " externalParentBones=" << externalParentBoneCount
				<< " rigidBodies=" << model.RigidBodies().size()
				<< " joints=" << model.Joints().size()
				<< " softBodies=" << model.SoftBodies().size()
				<< " softBodyFlagsMask=0x" << std::hex << softBodyFlagsMask << std::dec
				<< " unsupportedSoftBodyShapes=" << unsupportedSoftBodyShapeCount;
			const std::string message = oss.str();
			OutputDebugStringA((message + "\n").c_str());
			mmd::physics::debuglog::AppendLine(message);
		}

		ProgressCallback MakeStageProgressCallback(
			const ProgressCallback& onProgress,
			std::stop_token stopToken,
			float rangeStart,
			float rangeEnd)
		{
			return [=](float progress, const wchar_t* message)
			{
				if (stopToken.stop_requested() || !onProgress)
				{
					return;
				}

				const float scaledProgress = rangeStart + (rangeEnd - rangeStart) * progress;
				onProgress(scaledProgress, message);
			};
		}
	}

	ModelLoadResult LoadModelWithTextures(
		const std::filesystem::path& path,
		DcompRenderer& renderer,
		const ProgressCallback& onProgress,
		std::stop_token stopToken)
	{
		ModelLoadResult result{};

		try
		{
			auto newModel = std::make_unique<PmxModel>();
			const bool loaded = newModel->Load(
				path,
				MakeStageProgressCallback(onProgress, stopToken, 0.0f, 0.6f));

			if (loaded && !stopToken.stop_requested())
			{
				LogModelFeatureSummary(path, *newModel);
				renderer.LoadTexturesForModel(
					newModel.get(),
					MakeStageProgressCallback(onProgress, stopToken, 0.0f, 1.0f),
					0.6f,
					1.0f);
			}

			if (loaded && !stopToken.stop_requested())
			{
				result.model = std::move(newModel);
			}
		}
		catch (const std::exception& e)
		{
			const auto log = std::format("Model Load Error: {}\n", e.what());
			OutputDebugStringA(log.c_str());
			result.errorMessage = StringUtil::ExceptionMessageToWide(e);
		}
		catch (...)
		{
			OutputDebugStringA("Model Load Error: Unknown exception\n");
			result.errorMessage = L"モデルの読み込み中に不明な例外が発生しました。";
		}

		return result;
	}
}
