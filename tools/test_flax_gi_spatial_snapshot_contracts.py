import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
COMMON_SHADER = (REPO_ROOT / "shaders" / "flax_ddgi_common.slang").read_text(
    encoding="utf-8"
)
DEBUG_MODEL = (REPO_ROOT / "render" / "FlaxGIDebugModel.h").read_text(
    encoding="utf-8"
)
PASS_HEADER = (REPO_ROOT / "render" / "passes" / "FlaxDDGIPass.h").read_text(
    encoding="utf-8"
)
PASS_SOURCE = (REPO_ROOT / "render" / "passes" / "FlaxDDGIPass.cpp").read_text(
    encoding="utf-8"
)


class FlaxGISpatialSnapshotContractTests(unittest.TestCase):
    def test_cascade_coverage_uses_blend_origin_but_probe_lookup_stays_snapped(self) -> None:
        cascade_sampler = COMMON_SHADER[
            COMMON_SHADER.index("float3 sampleFlaxDDGIIrradianceCascade(") :
            COMMON_SHADER.index("// Main entry: sample cascaded DDGI irradiance")
        ]
        cascade_selector = COMMON_SHADER[
            COMMON_SHADER.index("// Main entry: sample cascaded DDGI irradiance") :
        ]

        self.assertIn(
            "float3 probesOrigin = ddgi.probesOriginAndSpacing[cascadeIndex].xyz",
            cascade_sampler,
        )
        self.assertIn("float3 blendOrigin = ddgi.blendOrigin[c].xyz", cascade_selector)
        self.assertIn(
            "float3 edgeDistances = extent - abs(biasedPos - blendOrigin)",
            cascade_selector,
        )
        self.assertNotIn(
            "float3 origin = ddgi.probesOriginAndSpacing[c].xyz",
            cascade_selector,
        )

    def test_output_state_publishes_versioned_spatial_metadata_with_atlas(self) -> None:
        self.assertIn("struct FlaxGISpatialSnapshot", DEBUG_MODEL)
        self.assertIn("std::array<float, 3> origin", DEBUG_MODEL)
        self.assertIn("float spacing", DEBUG_MODEL)
        self.assertIn("std::array<float, 3> blendOrigin", DEBUG_MODEL)
        self.assertIn("std::array<int32_t, 3> scrollOffset", DEBUG_MODEL)
        self.assertIn("uint64_t version", DEBUG_MODEL)
        self.assertIn("bool valid", DEBUG_MODEL)
        self.assertIn("FlaxGISpatialSnapshot publishedSpatial", DEBUG_MODEL)
        self.assertIn("spatial.version = nextPublishedVersion++", DEBUG_MODEL)
        self.assertIn("publishedSpatial = spatial", DEBUG_MODEL)
        self.assertIn(
            "FlaxGI atlas parity and spatial metadata must publish atomically",
            DEBUG_MODEL,
        )

    def test_pass_exposes_read_only_snapshot_accessors(self) -> None:
        self.assertIn(
            "FlaxGIOutputSnapshot getLightingOutputSnapshot(", PASS_HEADER
        )
        self.assertIn(
            "FlaxGIOutputSnapshot getPublishedOutputSnapshot() const", PASS_HEADER
        )
        self.assertIn(
            "FlaxGIOutputSnapshot FlaxDDGIPass::getLightingOutputSnapshot(",
            PASS_SOURCE,
        )
        self.assertIn(
            "m_outputState.selectSnapshotForFrame(", PASS_SOURCE
        )

    def test_same_current_snapshot_drives_uniform_upload_and_publication(self) -> None:
        execute_body = PASS_SOURCE[PASS_SOURCE.index("void FlaxDDGIPass::execute(") :]
        self.assertIn(
            "const FlaxGISpatialSnapshot currentSpatial = captureSpatialSnapshot()",
            execute_body,
        )
        self.assertIn(
            "writeFlaxDDGIDataToBuffer(frameIndex, currentSpatial)", execute_body
        )
        self.assertIn("m_outputState.publishPending(currentSpatial)", execute_body)

        capture_body = PASS_SOURCE[
            PASS_SOURCE.index("FlaxGISpatialSnapshot FlaxDDGIPass::captureSpatialSnapshot") :
            PASS_SOURCE.index("static shaderio::FlaxDDGIData buildFlaxDDGIData")
        ]
        self.assertIn("cascade.snappedOrigin", capture_body)
        self.assertIn("cascade.blendOrigin", capture_body)
        self.assertIn("cascade.scrollOffset", capture_body)
        self.assertIn("snapshot.valid = complete", capture_body)


if __name__ == "__main__":
    unittest.main()
