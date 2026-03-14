#include "PmxLoader.hpp"
#include "BinaryReader.hpp"

#include <atomic>

namespace
{
	std::atomic<uint64_t> g_revisionCounter{ 1 };
}

bool PmxLoader::LoadModel(const std::filesystem::path& pmxPath, PmxModel& model, PmxModel::ProgressCallback onProgress)
{
	model.ResetForLoad(pmxPath);

	if (onProgress) onProgress(0.05f, L"ヘッダー解析中...");
	BinaryReader reader(pmxPath);
	model.LoadHeader(reader);

	if (onProgress) onProgress(0.10f, L"頂点データを読み込み中...");
	model.LoadVertices(reader);

	if (onProgress) onProgress(0.30f, L"インデックスデータを読み込み中...");
	model.LoadIndices(reader);

	if (onProgress) onProgress(0.40f, L"テクスチャ定義を読み込み中...");
	model.LoadTextures(reader);

	if (onProgress) onProgress(0.40f, L"マテリアル定義を読み込み中...");
	model.LoadMaterials(reader);

	if (onProgress) onProgress(0.50f, L"ボーン構造を読み込み中...");
	model.LoadBones(reader);

	model.LoadMorphs(reader);
	model.SkipDisplayFrames(reader);
	model.LoadRigidBodies(reader);
	model.LoadJoints(reader);
	model.LoadSoftBodies(reader);

	if (onProgress) onProgress(0.60f, L"PMX解析完了");
	model.m_revision = g_revisionCounter++;
	return true;
}
