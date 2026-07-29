from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def braced_source(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class MeshletSceneInstanceTransformContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpp = (ROOT / "render" / "GPUDrivenRenderer.cpp").read_text(
            encoding="utf-8"
        )
        cls.header = (ROOT / "render" / "GPUDrivenRenderer.h").read_text(
            encoding="utf-8"
        )
        cls.types = (ROOT / "render" / "RenderTypes.h").read_text(encoding="utf-8")
        cls.registry_cpp = (ROOT / "render" / "GPUSceneRegistry.cpp").read_text(
            encoding="utf-8"
        )
        cls.registry_header = (ROOT / "render" / "GPUSceneRegistry.h").read_text(
            encoding="utf-8"
        )
        cls.meshlet_buffer_cpp = (ROOT / "render" / "GPUMeshletBuffer.cpp").read_text(
            encoding="utf-8"
        )
        cls.meshlet_buffer_header = (ROOT / "render" / "GPUMeshletBuffer.h").read_text(
            encoding="utf-8"
        )
        cls.scene_rebuild = braced_source(
            cls.cpp, "void GPUDrivenRenderer::rebuildGPUDrivenScene(const SceneAsset& asset"
        )
        cls.instance_update = braced_source(
            cls.cpp, "void GPUDrivenRenderer::updateSceneInstanceTransform"
        )
        cls.persistent_prepare = braced_source(
            cls.cpp, "void GPUDrivenRenderer::preparePersistentDrawData()"
        )
        cls.render_body = braced_source(
            cls.cpp, "void GPUDrivenRenderer::render(const RenderParams& params)"
        )
        cls.single_upload = braced_source(
            cls.cpp, "GltfUploadResult GPUDrivenRenderer::uploadGltfModel"
        )
        cls.scene_commit = braced_source(
            cls.cpp, "SceneUploadResult GPUDrivenRenderer::commitSceneUploadPlan"
        )
        cls.batch_upload = braced_source(
            cls.cpp, "void GPUDrivenRenderer::uploadGltfModelBatch"
        )
        cls.begin_replace = braced_source(
            cls.cpp, "void GPUDrivenRenderer::beginSceneReplacement()"
        )
        cls.clear_scene = braced_source(
            cls.cpp, "void GPUDrivenRenderer::clearGPUDrivenScene()"
        )
        cls.remove_instance = braced_source(
            cls.cpp, "bool GPUDrivenRenderer::removeSceneInstance"
        )
        cls.apply_remap = braced_source(
            cls.cpp, "bool GPUDrivenRenderer::applySceneObjectDenseRemap"
        )
        cls.mark_metadata_rewrite = braced_source(
            cls.cpp, "void GPUDrivenRenderer::markMeshletMetadataForFullRewrite"
        )
        cls.upload_after_idle = braced_source(
            cls.cpp, "void GPUDrivenRenderer::uploadPendingMeshletsAfterIdle"
        )
        cls.meshlet_buffer_upload = braced_source(
            cls.meshlet_buffer_cpp,
            "GPUMeshletBuffer::UploadRecord GPUMeshletBuffer::uploadMeshlets",
        )
        cls.registry_remove = braced_source(
            cls.registry_cpp, "GPUSceneRemoveResult GPUSceneRegistry::removeObject"
        )
        cls.registry_update = braced_source(
            cls.registry_cpp, "bool GPUSceneRegistry::updateTransform"
        )
        cls.registry_live = braced_source(
            cls.registry_cpp, "bool GPUSceneRegistry::isLiveHandle"
        )
        cls.next_generation = braced_source(
            cls.registry_cpp, "uint32_t GPUSceneRegistry::nextGeneration"
        )

    def assert_tokens(self, source: str, tokens: tuple[str, ...], message: str = "") -> None:
        for token in tokens:
            with self.subTest(token=token):
                self.assertIn(token, source, f"{message}{token}" if message else None)

    def test_scene_rebuild_preserves_instance_aware_meshlet_identity(self) -> None:
        self.assert_tokens(
            self.scene_rebuild,
            (
                "hasCompleteInstanceMeshletPayload",
                "packedIndexBaseByMesh",
                "meshlet.objectIndex = sceneObjectIndex",
                "bindPersistentDrawIdentity(drawIndex, drawRecordIndex, objectHandle, sceneObjectIndex)",
                "transformBoundsSphere(drawRecord.worldTransform, meshlet.boundsSphere)",
                "m_objectHandlesByMeshHandle[meshKey].push_back(objectHandle)",
            ),
            "missing instance-aware meshlet scene token: ",
        )
        base_guard = self.scene_rebuild.index("if (packedIndexBase == UINT32_MAX)")
        index_insert = self.scene_rebuild.index("m_meshletIndicesCpu.insert(", base_guard)
        meshlet_duplicate_loop = self.scene_rebuild.index(
            "for (uint32_t localMeshletIndex", index_insert
        )
        self.assertLess(base_guard, index_insert)
        self.assertLess(
            index_insert,
            meshlet_duplicate_loop,
            "meshlet geometry indices must be appended once per mesh before per-instance entry duplication",
        )

    def test_instance_transform_uses_stable_identity_and_history(self) -> None:
        self.assert_tokens(
            self.header,
            (
                "m_drawIdentityByDrawIndex",
                "m_drawIndicesByDrawRecord",
                "m_transformHistoryByDrawRecord",
                "GPUSceneObjectHandle registryObjectHandle",
            ),
        )
        self.assertIn("SubmittedTransformHistory", self.types)
        self.assert_tokens(
            self.instance_update,
            (
                "m_drawIndicesByDrawRecord[drawRecordIndex]",
                "m_sceneRegistry.updateTransform(",
                "objectHandle",
                "updateSubmittedTransformHistory(transformHistory, transform)",
                "transformHistory.previousWorldTransform",
            ),
        )
        self.assertNotIn(
            "m_objectHandleByMeshHandle",
            self.instance_update,
            "instance updates must never fall back to mesh-handle identity",
        )

    def test_persistent_draw_mapping_and_submit_history_contract(self) -> None:
        self.assert_tokens(
            self.persistent_prepare,
            (
                "m_drawIdentityByDrawIndex",
                "drawIdentity->drawRecordIndex",
                "transformHistory->currentWorldTransform",
                "transformHistory->previousWorldTransform",
            ),
        )
        self.assertNotIn(
            "drawIndex < m_sceneDrawRecords.size()",
            self.persistent_prepare,
            "meshlet draw indices must not be interpreted as draw-record indices",
        )
        failed_submit = self.render_body.index("if (!frameSubmitted)")
        failed_return = self.render_body.index("return;", failed_submit)
        history_commit = self.render_body.index(
            "commitSubmittedSceneTransformHistory();", failed_return
        )
        self.assertLess(failed_submit, failed_return)
        self.assertLess(
            failed_return,
            history_commit,
            "failed/no-op frame preparation must return before transform history advances",
        )

    def test_single_scene_uploads_replace_while_batch_upload_appends(self) -> None:
        for body, upload_token in (
            (self.single_upload, "m_renderer.uploadGltfModel"),
            (self.scene_commit, "m_renderer.commitSceneUploadPlan"),
        ):
            with self.subTest(upload_token=upload_token):
                upload = body.index(upload_token)
                replace = body.index("beginSceneReplacement();")
                publish = body.index("m_activeUploadResultStorage = result;")
                rebuild = body.index("rebuildGPUDrivenScene")
                self.assertLess(replace, upload)
                self.assertLess(upload, publish)
                self.assertLess(publish, rebuild)
        self.assertNotIn("beginSceneReplacement", self.batch_upload)

        wait_for_idle = self.begin_replace.index("m_renderer.waitForIdle();")
        clear_scene = self.begin_replace.index("clearGPUDrivenScene();")
        resize_hiz = self.begin_replace.index("m_hiZDepthPyramid.resize")
        self.assertLess(wait_for_idle, clear_scene)
        self.assertLess(clear_scene, resize_hiz)
        self.assert_tokens(
            self.clear_scene,
            (
                "m_sceneRegistry.clear();",
                "m_meshletDataCpu.clear();",
                "m_sceneDrawRecords.clear();",
                "m_drawIdentityByDrawIndex.clear();",
                "m_objectHandleByDrawRecord.clear();",
                "m_opaqueDrawIndices.clear();",
                "m_alphaTestDrawIndices.clear();",
                "m_transparentDrawIndices.clear();",
                "m_activeUploadResult = nullptr;",
            ),
            "scene replacement did not clear old topology state: ",
        )

    def test_registry_handles_are_generation_checked(self) -> None:
        self.assert_tokens(
            self.registry_header,
            (
                "struct GPUSceneObjectHandle",
                "uint32_t generation",
                "struct GPUSceneRemoveResult",
                "movedFromDenseIndex",
                "movedToDenseIndex",
            ),
        )
        self.assertIn("if (!isLiveHandle(object))", self.registry_remove)
        self.assertIn(
            "slot.generation = nextGeneration(slot.generation)", self.registry_remove
        )
        self.assertIn(
            "m_slots[object.index].generation == object.generation", self.registry_live
        )
        self.assertIn("if (!isLiveHandle(object))", self.registry_update)
        self.assertIn("++generation;", self.next_generation)
        self.assertIn(
            "return generation == 0u ? 1u : generation;", self.next_generation
        )

    def test_registry_removal_publishes_exact_dense_swap_remap(self) -> None:
        self.assertIn(
            "return removed && !movedObject.isNull() && movedFromDenseIndex != movedToDenseIndex;",
            self.registry_header,
        )
        self.assert_tokens(
            self.registry_remove,
            (
                "m_slots[movedObjectID].denseIndex = denseIndex",
                "result.movedObject = GPUSceneObjectHandle",
                "result.movedFromDenseIndex = lastDenseIndex",
                "result.movedToDenseIndex = denseIndex",
            ),
        )

    def test_removal_remaps_before_tombstoning_and_clears_draw_identity(self) -> None:
        unsupported_guard = self.remove_instance.index(
            "if (objectHandle.isNull() || !m_enableExperimentalMeshletPath || drawIndices.empty())"
        )
        remove_call = self.remove_instance.index(
            "m_sceneRegistry.removeObject(objectHandle)"
        )
        self.assertLess(
            unsupported_guard,
            remove_call,
            "unsupported single-object removal must fail before registry topology mutates",
        )
        remap_call = self.remove_instance.index(
            "applySceneObjectDenseRemap(removeResult)"
        )
        tombstone = self.remove_instance.index(
            "m_meshletDataCpu[drawIndex].objectIndex = UINT32_MAX"
        )
        self.assertLess(remove_call, remap_call)
        self.assertLess(remap_call, tombstone)
        self.assert_tokens(
            self.remove_instance,
            (
                "m_meshletCullObjectsCpu[drawIndex] = {};",
                "m_drawIdentityByDrawIndex[drawIndex] = {};",
                "m_activeUploadResultStorage.shadowPackedMeshes.erase",
                "markMeshletMetadataForFullRewrite();",
                "advanceSceneTopologyVersion();",
                "m_renderer.waitForIdle();",
                "clearGPUDrivenScene();",
            ),
            "incomplete safe removal path: ",
        )

    def test_dense_remap_rewrites_meshlet_identity_or_fails_closed(self) -> None:
        self.assertIn("if (!removeResult.hasDenseRemap())", self.apply_remap)
        self.assert_tokens(
            self.apply_remap,
            (
                "identity.registryObjectHandle != removeResult.movedObject",
                "identity.sceneObjectIndex = removeResult.movedToDenseIndex",
                "m_meshletDataCpu[drawIndex].objectIndex = removeResult.movedToDenseIndex",
                "markMeshletMetadataForFullRewrite();",
            ),
            "dense remap did not reach meshlet identity: ",
        )

    def test_same_size_meshlet_metadata_rewrite_skips_geometry_reupload(self) -> None:
        self.assert_tokens(
            self.meshlet_buffer_header,
            (
                "enum class MetadataUploadMode",
                "forceFullRewrite",
                "struct UploadRange",
                "struct UploadRecord",
                "UploadRange meshletMetadata",
                "UploadRange cullMetadata",
                "UploadRange indexGeometry",
            ),
        )
        self.assert_tokens(
            self.meshlet_buffer_upload,
            (
                "const bool forceFullMetadataRewrite",
                "const bool rewriteAllMetadata = rewriteAll || forceFullMetadataRewrite",
                "const uint32_t meshletStart = rewriteAllMetadata ? 0u",
                "const uint32_t cullObjectStart =",
                "rewriteAllMetadata ? 0u",
                "const uint32_t indexStart = rewriteAll ? 0u",
                "uploadRecord.meshletMetadata.byteCount",
                "uploadRecord.cullMetadata.byteCount",
                "uploadRecord.indexGeometry.byteCount",
            ),
            "meshlet buffer upload did not expose the required metadata-only rewrite contract: ",
        )
        meshlet_start = self.meshlet_buffer_upload.index(
            "const uint32_t meshletStart = rewriteAllMetadata ? 0u"
        )
        index_start = self.meshlet_buffer_upload.index(
            "const uint32_t indexStart = rewriteAll ? 0u"
        )
        self.assertLess(meshlet_start, index_start)
        self.assertNotIn(
            "const uint32_t indexStart = rewriteAllMetadata ? 0u",
            self.meshlet_buffer_upload,
            "force-full metadata rewrite must not force unchanged index geometry upload",
        )

        self.assert_tokens(
            self.mark_metadata_rewrite,
            (
                "m_meshletUploadDirty = true;",
                "m_meshletMetadataFullRewriteDirty = true;",
            ),
        )
        mode_select = self.upload_after_idle.index(
            "m_meshletMetadataFullRewriteDirty"
        )
        force_mode = self.upload_after_idle.index(
            "GPUMeshletBuffer::MetadataUploadMode::forceFullRewrite"
        )
        upload = self.upload_after_idle.index("m_meshletBuffer.uploadMeshlets(")
        upload_dirty_clear = self.upload_after_idle.index(
            "m_meshletUploadDirty = false;", upload
        )
        metadata_dirty_clear = self.upload_after_idle.index(
            "m_meshletMetadataFullRewriteDirty = false;", upload
        )
        self.assertLess(mode_select, force_mode)
        self.assertLess(force_mode, upload)
        self.assertLess(upload, upload_dirty_clear)
        self.assertLess(upload, metadata_dirty_clear)
        self.assertIn("bool m_meshletMetadataFullRewriteDirty{false}", self.header)
        self.assertIn(
            "m_meshletMetadataFullRewriteDirty = false;", self.clear_scene
        )

if __name__ == "__main__":
    unittest.main(verbosity=2)