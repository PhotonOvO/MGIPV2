import copy
import hashlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
HARNESS = (REPO_ROOT / "tools" / "run_csm_shadow_motion_smoke.py").read_text(
    encoding="utf-8"
)
COMPARATOR = (REPO_ROOT / "tools" / "rdc_csm_motion_compare.py").read_text(
    encoding="utf-8"
)
SHADOW_EDGE = (REPO_ROOT / "tools" / "rdc_shadow_edge_metrics.py").read_text(
    encoding="utf-8"
)
APP_HEADER = (REPO_ROOT / "app" / "MinimalLatestApp.h").read_text(encoding="utf-8")
APP_SOURCE = (REPO_ROOT / "app" / "MinimalLatestApp.cpp").read_text(encoding="utf-8")
RENDER_TYPES = (REPO_ROOT / "render" / "RenderTypes.h").read_text(encoding="utf-8")
CSM_PASS = (
    REPO_ROOT / "render" / "passes" / "GPUDrivenCSMShadowPass.cpp"
).read_text(encoding="utf-8")


def braced_source(source: str, pattern: str) -> str:
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError(f"Could not find source pattern: {pattern}")
    opening_brace = source.find("{", match.end())
    if opening_brace < 0:
        raise AssertionError(f"Could not find opening brace after: {pattern}")
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace : index + 1]
    raise AssertionError(f"Could not find closing brace after: {pattern}")


class RenderDocCaptureProtocolContractTests(unittest.TestCase):
    def test_capture_sync_arms_one_application_frame_before_target_frames(self) -> None:
        handshake = braced_source(
            APP_HEADER,
            r"MinimalLatestApp::automationCaptureHandshakeMarkerForFrame\s*\(",
        )
        self.assertIn('return "arm-last-moving";', handshake)
        self.assertIn('return "arm-settled";', handshake)
        self.assertRegex(handshake, r"frame\s*\+\s*1u\s*==\s*lastMovingFrame")
        self.assertRegex(
            handshake,
            r"frame\s*\+\s*1u\s*==\s*automationSettledFrame\s*\(\s*\)",
        )
        self.assertIn("automationBoundaryMarkerForFrame(frame)", handshake)

        wait = braced_source(
            APP_HEADER,
            r"MinimalLatestApp::waitForAutomationCaptureHandshake\s*\(",
        )
        self.assertIn(
            "automationCaptureHandshakeMarkerForFrame(m_automationFrame)",
            wait,
        )
        self.assertIn('".ready.json"', wait)
        self.assertIn('".continue"', wait)

    def test_short_capture_hold_is_rejected_to_keep_handshakes_distinct(self) -> None:
        self.assertIn(
            "publishes one ready/continue marker per application frame",
            APP_SOURCE,
        )
        self.assertRegex(
            APP_SOURCE,
            r"!options\.captureSyncDirectory\.empty\(\)\s*&&\s*"
            r"options\.holdFrames\s*<\s*3u",
        )
        self.assertIn(
            "first-still, arm-settled, and settled use distinct application frames",
            APP_SOURCE,
        )

        warmup, motion, hold = 8, 24, 8  # Production RenderDoc capture configuration.
        last_moving = warmup + motion - 1
        frames = {
            "arm-last-moving": last_moving - 1,
            "last-moving": last_moving,
            "first-still": last_moving + 1,
            "arm-settled": warmup + motion + hold - 2,
            "settled": warmup + motion + hold - 1,
        }
        self.assertEqual(len(set(frames.values())), len(frames), frames)

    def test_target_frame_identity_is_nested_inside_stable_csm_marker(self) -> None:
        self.assertIn("std::string automationDebugMarker{}", RENDER_TYPES)
        self.assertIn(
            "automationTargetBoundaryMarkerForFrame(m_automationFrame)",
            APP_HEADER,
        )
        target_boundaries = braced_source(
            APP_HEADER,
            r"MinimalLatestApp::automationTargetBoundaryMarkerForFrame\s*\(",
        )
        self.assertIn('return "last-moving";', target_boundaries)
        self.assertIn('return "first-still";', target_boundaries)
        self.assertIn('return "settled";', target_boundaries)
        self.assertNotIn("control-still", target_boundaries)
        self.assertRegex(
            APP_HEADER,
            r'std::string\("CSM_AUTOMATION_FRAME mode="\)\s*\+\s*automationModeName\(\)'
            r'\s*\+\s*" boundary="\s*\+\s*boundary'
            r'\s*\+\s*" frame="\s*\+\s*std::to_string\(m_automationFrame\)',
        )
        self.assertNotIn('"CSM_AUTOMATION mode="', APP_HEADER)

        execute = braced_source(
            CSM_PASS,
            r"GPUDrivenCSMShadowPass::execute\s*\(",
        )
        fixed_begin = execute.index(
            'context.commandBuffer->beginEvent("GPUDrivenCSMShadow")'
        )
        dynamic_begin = execute.index(
            "context.commandBuffer->beginEvent("
            "context.params->automationDebugMarker.c_str())"
        )
        self.assertLess(fixed_begin, dynamic_begin)
        self.assertIn("hasAutomationDebugMarker", execute)
        self.assertGreaterEqual(execute.count("context.commandBuffer->endEvent();"), 4)

    def test_protocol_does_not_depend_on_renderdoc_compile_time_headers(self) -> None:
        combined = "\n".join((APP_HEADER, RENDER_TYPES, CSM_PASS)).lower()
        self.assertNotIn("renderdoc_app.h", combined)
        self.assertNotIn("renderdoc_api_", combined)


    def test_cleanup_uses_stable_native_handles_without_pid_only_fallback(self) -> None:
        combined = HARNESS + "\n" + COMPARATOR
        lower_combined = combined.lower()
        self.assertNotIn("taskkill", lower_combined)
        self.assertNotIn("terminate_process_tree", combined)
        self.assertNotIn('["close", "--shutdown"]', combined)
        for token in (
            "DuplicateHandle",
            "GetProcessTimes",
            "WaitForSingleObject",
            "TerminateProcess",
            "StableProcessIdentity.from_launch_owned_process(",
            "creation_filetime_ticks",
            "renderdoc_module.InjectIntoProcess(",
            "NtResumeProcess",
            "target_control_pid_matches_owned_handle",
            'target_binding.get("duplicated_from_launcher_handle") is True',
            'target_binding.get("pid_lookup_used") is False',
            "same_native_handle",
            "refusing pid-based fallback",
            "shutdown_owned_rdc_session_direct",
            "_shutdown_owned_replay_session_direct",
            "rdc.daemon_client import send_request",
            "rdc.protocol import shutdown_request",
        ):
            self.assertIn(token, combined)
        self.assertIn("process_identity=daemon_process_identity", HARNESS)
        self.assertIn('"replacement_untouched": True', HARNESS)
        for token in (
            "STATE_PUBLICATION_BOUNDARY_SCHEMA",
            "STATE_VOLUME_EVIDENCE_SCHEMA",
            "RDC_CLI_SESSION_ORDERING_SCHEMA",
            "GetFileTime",
            "GetFinalPathNameByHandleW",
            "GetDriveTypeW",
            "GetVolumeInformationByHandleW",
            "WINDOWS_FILE_NAME_NORMALIZED",
            "WINDOWS_FINAL_PATH_FLAGS",
            "final_path_flags",
            "SUPPORTED_STATE_FILE_SYSTEMS",
            "creation_time_unix_ns",
            "positive_post_state_tolerance_ticks",
            "state_publication_boundary_exact",
            "exact_creation_order_clock_match",
        ):
            self.assertIn(token, combined)
        self.assertNotIn("state_modified_time + 0.001", HARNESS)
        self.assertNotIn("state_modified + 2.0", COMPARATOR)
        self.assertNotIn("float(open_started_wall) - 2.0", COMPARATOR)
        self.assertNotIn("creation_ticks <= publication_ticks", combined)
        self.assertNotIn("GetVolumePathNameW", combined)
        self.assertNotIn("GetVolumeInformationW", combined)
        self.assertIn("path_fallback_used", combined)
        self.assertIn(
            "process_creation_filetime_ticks < state_publication_filetime_ticks",
            combined,
        )
    def test_persistent_target_control_batches_and_quarantines_candidates(self) -> None:
        for token in (
            "class PersistentTargetControl",
            "baseline_capture_ids",
            "batch_epoch",
            "quarantine_directory",
            "controller.pump",
            "TriggerCapture(2)",
            "TriggerCapture(1)",
        ):
            self.assertIn(token, HARNESS)
        self.assertRegex(
            HARNESS,
            r"wait_for_value\([\s\S]*?pump=controller\.pump",
        )
        self.assertRegex(
            HARNESS,
            r"wait_for_ready_marker\([\s\S]*?pump=controller\.pump",
        )
        self.assertIn("captureId=0", HARNESS)

    def test_harness_candidate_validation_requires_real_taa_resolve_draw(self) -> None:
        for token in (
            '"GPUDrivenTAAResolve"',
            '"GPUDrivenTAAResolvePass"',
            '"fragmentTAAResolveMain"',
            "Draw(3,1)",
            '4: ("scene_color_hdr",',
            '7: ("velocity",',
            '8: ("history_read",',
            "RGBA16F",
        ):
            self.assertIn(token, HARNESS)
        self.assertIn(
            "GPUDrivenTAAResolvePass is not accepted",
            HARNESS,
        )

    def test_manifest_binds_each_capture_run_to_executable_content(self) -> None:
        for token in (
            "hashlib.sha256()",
            '"resolved_path"',
            '"size_bytes"',
            '"sha256"',
            "prepare_immutable_executable_binding",
            "LockedExecutableImage",
            "_FILE_SHARE_READ",
            "args.launch_exe",
            "cwd=args.launch_cwd",
            "source_executable_evidence_baseline",
            "launch_executable_evidence_baseline",
            "target_process_binding",
            "same_native_handle_reserved_for_cleanup",
        ):
            self.assertIn(token, HARNESS)
        self.assertIn("immutable launch image", HARNESS)
        self.assertIn("image_path_matches", HARNESS)

    def test_cleanup_manifest_uses_owned_daemon_contract_and_fails_closed(self) -> None:
        for token in (
            '"rdc-session-cleanup-v2"',
            "process_access_denied_count",
            "owned_added_daemons",
            "external_added_daemons",
            "daemon_ownership",
            "state_path_match",
            "state_capture_path_match",
            "daemon_capture_path_metadata_match",
        ):
            self.assertIn(token, HARNESS)
        self.assertRegex(
            HARNESS,
            r'ownership\.get\("established"\)\s+is\s+True',
        )
        self.assertIn("errors", HARNESS)

    def test_comparator_release_gate_is_exact_and_uses_independent_roi_api(self) -> None:
        for token in (
            "rdc_shadow_edge_metrics",
            "CameraMatrices",
            "ShadowFrame",
            "ShadowEdgeMetricsConfig",
            "evaluate_shadow_edge_metrics",
            "scene_depth",
            "world_normals",
            "base_color",
            "scene_color_hdr",
            "history_read",
            "history_write",
            "final",
            "framebuffer_y_to_ndc",
            "view_from_world",
            "world_from_view",
            "_host_rdc_session_state_path",
            "_shutdown_owned_replay_session_direct",
            "_validate_direct_shutdown_cleanup_evidence",
            '"mgif-rdc-direct-token-shutdown-v1"',
            '"close_subprocess_used"',
            '"status_subprocess_used"',
            "absent_after_cleanup",
            "held replay-daemon handle absent plus exact state file absent",
            "release_evidence",
        ):
            self.assertIn(token, COMPARATOR)
        self.assertIn("top_to_negative_one", COMPARATOR)
        self.assertIn("exactly two target cases", COMPARATOR)
        self.assertIn("expected [1, 0, 1]", COMPARATOR)
        self.assertIn("taa.passthrough is forbidden for release", COMPARATOR)
        self.assertNotIn("def evaluate_shadow_edge_metrics(", COMPARATOR)
    def test_extractor_readback_and_npy_contract_is_strict(self) -> None:
        for token in (
            "samples==1",
            "depth==1",
            "mip==0",
            "expected_raw_bytes",
            "raw_byte_length_exact",
            "tightly_packed_required",
            "_d_validate_saved_npy",
            "reopened_after_write",
            "channel_order",
            "source_format",
            "screen_extent",
            '"evidence_only": bool(evidence_only)',
            '"history_read_semantics": "evidence-only"',
        ):
            self.assertIn(token, COMPARATOR)
        self.assertNotIn("array[: width * height]", COMPARATOR)
        self.assertNotIn("count=expected", COMPARATOR)
        self.assertIn("got {actual} bytes, expected exactly {expected}", COMPARATOR)

    def test_light_final_toolchain_disk_and_deadline_release_gates(self) -> None:
        for token in (
            '"fragment_entry": "fragmentHdrMain"',
            '"fragment_entry": "fragmentFinalColorMain"',
            '"GPUDrivenLightPass"',
            '"GPUDrivenFinalColor"',
            "instance_count_raw",
            "output_written_in_marker",
            "TOOLCHAIN_EVIDENCE_SCHEMA",
            "collect_toolchain_snapshot",
            "smoke_disk_space_preflight",
            "required_free_bytes",
            "safety_margin_bytes",
        ):
            self.assertIn(token, HARNESS + "\n" + COMPARATOR)
        for token in (
            "--total-timeout",
            "CAPTURE_CLEANUP_RESERVE_SECONDS",
            "_session_cleanup_terminal_result",
            "direct_shutdown_passed",
            "owned_daemon_absent",
            "state_absent",
            "handle_closed",
            "deadline_exceeded",
            "same_handle_recovery",
            "pid_only_fallback",
            "tree_cleanup_requested",
        ):
            self.assertIn(token, COMPARATOR)
        self.assertNotIn('base + ["close", "--shutdown"]', COMPARATOR)
        self.assertIn('"toolchain_evidence": {', HARNESS)
        self.assertIn('"disk_preflight": disk_preflight', HARNESS)
        self.assertIn('case["toolchain_bundle_sha256"]', HARNESS)
    def test_zero_byte_runtime_dependency_files_are_valid_but_missing_or_nonregular_fail(self) -> None:
        from tools import rdc_csm_motion_compare as comparator
        from tools import run_csm_shadow_motion_smoke as harness

        empty_sha = hashlib.sha256(b"").hexdigest()
        with tempfile.TemporaryDirectory(prefix="rdc_protocol_zero_byte_") as directory:
            root = Path(directory)
            empty = root / "empty.py"
            empty.write_bytes(b"")
            for module, error_type in (
                (harness, harness.SmokeFailure),
                (comparator, comparator.ComparatorError),
            ):
                record = module._stable_dependency_file_record(
                    empty,
                    relative_path="pkg/empty.py",
                    role="self_test_dependency",
                )
                self.assertEqual(0, record["size_bytes"])
                self.assertEqual(empty_sha, record["sha256"])
                with self.assertRaises(error_type):
                    module._stable_dependency_file_record(
                        root / "missing.py",
                        relative_path="pkg/missing.py",
                        role="self_test_dependency",
                    )
                with self.assertRaises(error_type):
                    module._stable_dependency_file_record(
                        root,
                        relative_path="pkg/not-a-file",
                        role="self_test_dependency",
                    )
        package = harness.collect_rdc_cli_package_evidence()
        zero_records = {
            row["relative_path"]: row
            for row in package["files"]
            if row["size_bytes"] == 0
        }
        self.assertEqual(len(zero_records), package["zero_byte_file_count"])
        for relative in (
            "rdc/_skills/__init__.py",
            "rdc/vfs/__init__.py",
            "rdc_cli-0.6.1.dist-info/REQUESTED",
        ):
            self.assertIn(relative, zero_records)
            self.assertEqual(empty_sha, zero_records[relative]["sha256"])

    def test_release_gate_rejects_unowned_target_missing_direct_shutdown_and_stale_demo(self) -> None:
        from tools import rdc_csm_motion_compare as comparator

        with tempfile.TemporaryDirectory(prefix="rdc_protocol_process_gate_") as directory:
            root = Path(directory)
            source, _, _ = comparator._self_test_case_fixture(root)

            bad_target = copy.deepcopy(source)
            binding = bad_target["cases"][0]["executable_evidence"][
                "target_process_binding"
            ]
            binding["pid"] = 0
            errors = comparator._validate_release_manifest_contract(
                bad_target,
                root / "manifest.json",
            )
            self.assertTrue(any("target process PID is not positive" in row for row in errors))

            bad_cleanup = copy.deepcopy(source)
            del bad_cleanup["rdc_session_cleanup"]["named_replay_sessions"][0][
                "direct_shutdown"
            ]
            errors = comparator._validate_release_cleanup_contract(bad_cleanup)
            self.assertTrue(any("direct token shutdown evidence is missing" in row for row in errors))

            stale = copy.deepcopy(source)
            stale_path = root / "out" / "build" / "x64-debug" / "Demo.csm_shadow_reactive_test.exe"
            stale["formal_source_executable_contract"][
                "selected_source_executable"
            ] = str(stale_path.resolve())
            errors = comparator._validate_release_manifest_contract(
                stale,
                root / "manifest.json",
            )
            self.assertTrue(any("formal source executable paths do not bind" in row for row in errors))
    def test_raw_num_instances_zero_is_rejected_by_evidence_not_wording(self) -> None:
        from tools import rdc_csm_motion_compare as comparator

        combined = HARNESS + "\n" + COMPARATOR
        self.assertNotRegex(
            combined,
            r"max\s*\(\s*1\s*,\s*instance_count_raw\s*\)",
        )
        with tempfile.TemporaryDirectory(prefix="rdc_protocol_raw_instances_") as directory:
            root = Path(directory)
            pristine = comparator._self_test_release_extraction_fixture(
                root / "pristine",
                boundary="last-moving",
                history_write_index=1,
            )
            self.assertEqual(
                [],
                comparator._validate_release_extraction_contract(
                    pristine,
                    root / "pristine",
                    label="pristine",
                    render_mode="taa-on",
                ),
            )
            for role in ("light", "taa", "final"):
                manifest = copy.deepcopy(pristine)
                action = manifest["release_resource_evidence"]["draw_evidence"][
                    role
                ]["action"]
                self.assertEqual(1, action["instance_count_raw"])
                self.assertEqual(1, action["instance_count"])
                action["instance_count_raw"] = 0
                errors = comparator._validate_release_extraction_contract(
                    manifest,
                    root / "pristine",
                    label=f"raw-zero-{role}",
                    render_mode="taa-on",
                )
                self.assertTrue(errors, role)
                self.assertEqual(0, action["instance_count_raw"])
                self.assertEqual(1, action["instance_count"])

    def test_state_file_handle_volume_policy_is_pure_fail_closed_and_in_parity(self) -> None:
        from tools import rdc_csm_motion_compare as comparator
        from tools import run_csm_shadow_motion_smoke as harness

        publication_ticks = harness.WINDOWS_FILETIME_EPOCH_OFFSET_TICKS + 50_000_000
        final_path = (
            r"\\?\Volume{1082ed61-3991-4ccf-8007-161d89cd277c}"
            r"\rdc\sessions\formal.json"
        )
        volume_root = "\\\\?\\Volume{1082ed61-3991-4ccf-8007-161d89cd277c}\\"

        def snapshot(
            *,
            file_system: str = "NTFS",
            drive_type_code: int = 3,
            ticks: int = publication_ticks,
            native_handle_value: int = 123,
            handle_path: str = final_path,
            valid: bool = True,
        ):
            drive_names = {
                0: "unknown",
                2: "removable",
                3: "fixed",
                4: "remote",
            }
            return {
                "schema": harness.STATE_HANDLE_SNAPSHOT_SCHEMA,
                "valid": valid,
                "source": (
                    "msvcrt.get_osfhandle + GetFileTime + "
                    "GetFinalPathNameByHandleW(VOLUME_NAME_GUID|FILE_NAME_NORMALIZED) + "
                    "GetVolumeInformationByHandleW + GetDriveTypeW"
                ),
                "path_fallback_used": False,
                "original_path_used": False,
                "final_path_flags": {
                    "value": harness.WINDOWS_FINAL_PATH_FLAGS,
                    "volume_name": "VOLUME_NAME_GUID",
                    "file_name": "FILE_NAME_NORMALIZED",
                },
                "native_handle_value": native_handle_value,
                "filetime_ticks": ticks,
                "final_path_guid": handle_path,
                "volume_guid_root": volume_root,
                "volume_serial_number": 42,
                "maximum_component_length": 255,
                "file_system_flags": 0,
                "file_system": file_system,
                "drive_type_code": drive_type_code,
                "drive_type": drive_names.get(drive_type_code, "invalid"),
                **({} if valid else {"error": "injected native API failure"}),
            }

        evaluators = (
            harness._evaluate_state_file_handle_policy,
            comparator._host_evaluate_state_file_handle_policy,
        )
        for file_system in ("NTFS", "ReFS"):
            results = [
                evaluator(
                    snapshot(file_system=file_system),
                    snapshot(file_system=file_system),
                )
                for evaluator in evaluators
            ]
            self.assertEqual(results[0], results[1])
            self.assertTrue(results[0]["verified"], results[0])
            self.assertTrue(results[0]["same_handle_path_identity"])
            self.assertTrue(results[0]["raw_filetime_stable"])
            self.assertFalse(results[0]["path_fallback_used"])

        missing_identity_before = snapshot()
        missing_identity_after = snapshot()
        missing_identity_before.pop("volume_guid_root")
        missing_identity_after.pop("volume_guid_root")
        missing_flags_before = snapshot()
        missing_flags_after = snapshot()
        missing_flags_before.pop("final_path_flags")
        missing_flags_after.pop("final_path_flags")
        negative_pairs = {
            "missing-volume-identity": (
                missing_identity_before,
                missing_identity_after,
            ),
            "missing-final-path-flags": (
                missing_flags_before,
                missing_flags_after,
            ),
            "remote": (
                snapshot(drive_type_code=4),
                snapshot(drive_type_code=4),
            ),
            "removable": (
                snapshot(drive_type_code=2),
                snapshot(drive_type_code=2),
            ),
            "fat": (
                snapshot(file_system="FAT32"),
                snapshot(file_system="FAT32"),
            ),
            "unknown-filesystem": (
                snapshot(file_system="UNKNOWN"),
                snapshot(file_system="UNKNOWN"),
            ),
            "api-failure": (
                snapshot(valid=False),
                snapshot(valid=False),
            ),
            "filetime-changed": (
                snapshot(),
                snapshot(ticks=publication_ticks + 1),
            ),
            "handle-changed": (
                snapshot(),
                snapshot(native_handle_value=124),
            ),
            "final-path-changed": (
                snapshot(),
                snapshot(handle_path=final_path + ".replacement"),
            ),
        }
        for label, (before, after) in negative_pairs.items():
            results = [evaluator(before, after) for evaluator in evaluators]
            self.assertEqual(results[0], results[1], label)
            self.assertFalse(results[0]["verified"], (label, results[0]))
        changed_time = evaluators[0](*negative_pairs["filetime-changed"])
        self.assertFalse(changed_time["raw_filetime_stable"])
        changed_path = evaluators[0](*negative_pairs["final-path-changed"])
        self.assertFalse(changed_path["same_handle_path_identity"])
    def test_daemon_creation_must_strictly_precede_native_state_publication(self) -> None:
        from tools import rdc_csm_motion_compare as comparator
        from tools import run_csm_shadow_motion_smoke as harness

        with tempfile.TemporaryDirectory(prefix="rdc_protocol_publication_boundary_") as directory:
            root = Path(directory)
            capture = root / "capture.rdc"
            capture.write_bytes(b"rdc")
            state_path = root / "session.json"
            image_path = str((root / "rdc.exe").resolve())
            state_path.write_text(
                json.dumps(
                    {
                        "capture": str(capture.resolve()),
                        "current_eid": 0,
                        "opened_at": "2026-07-29T00:00:00+00:00",
                        "host": "127.0.0.1",
                        "port": 12345,
                        "token": "protocol-test-token",
                        "pid": 77,
                    }
                ),
                encoding="utf-8",
            )

            harness_state = harness.rdc_session_state_record(state_path)
            comparator_state = comparator._host_session_state_record(state_path)
            self.assertTrue(harness_state["valid"], harness_state)
            self.assertTrue(comparator_state["valid"], comparator_state)
            harness_boundary = harness_state["publication_boundary"]
            comparator_boundary = comparator_state["publication_boundary"]
            publication_ticks = int(harness_boundary["filetime_ticks"])
            self.assertEqual(publication_ticks, comparator_boundary["filetime_ticks"])
            for boundary in (harness_boundary, comparator_boundary):
                self.assertEqual(
                    "GetFileTime before/after read on the same held state-file handle, with GUID final-path and handle-derived volume proof",
                    boundary["source"],
                )
                self.assertTrue(boundary["native_filetime_read_before_after"])
                self.assertTrue(boundary["state_file_handle_held_during_read"])
                self.assertFalse(boundary["divisibility_used_as_granularity_proof"])
                self.assertFalse(boundary["timestamp_equality_accepted"])
                self.assertEqual(
                    "process_creation_filetime_ticks < state_publication_filetime_ticks",
                    boundary["ordering_rule"],
                )
                volume = boundary["volume"]
                self.assertTrue(boundary["same_handle_path_identity"])
                self.assertTrue(boundary["raw_filetime_stable"])
                self.assertFalse(boundary["path_fallback_used"])
                self.assertTrue(volume["verified"], volume)
                self.assertTrue(volume["same_handle_path_identity"], volume)
                self.assertTrue(volume["raw_filetime_stable"], volume)
                self.assertFalse(volume["path_fallback_used"], volume)
                self.assertFalse(volume["original_path_used"], volume)
                self.assertTrue(volume["local"], volume)
                self.assertEqual(harness.WINDOWS_DRIVE_FIXED, volume["drive_type_code"])
                self.assertIn(volume["file_system"], harness.SUPPORTED_STATE_FILE_SYSTEMS)
                expected_flags = {
                    "value": harness.WINDOWS_FINAL_PATH_FLAGS,
                    "volume_name": "VOLUME_NAME_GUID",
                    "file_name": "FILE_NAME_NORMALIZED",
                }
                before_handle = volume["before_snapshot"]
                after_handle = volume["after_snapshot"]
                self.assertEqual(expected_flags, before_handle["final_path_flags"])
                self.assertEqual(expected_flags, after_handle["final_path_flags"])
                self.assertEqual(
                    before_handle["native_handle_value"],
                    after_handle["native_handle_value"],
                )
                self.assertEqual(
                    before_handle["final_path_guid"], after_handle["final_path_guid"]
                )
                self.assertTrue(
                    before_handle["final_path_guid"].startswith(
                        before_handle["volume_guid_root"]
                    )
                )
            # Native handle values are allocation-local and may differ across the two
            # independent opens. The held-handle identity checks above require each
            # before/after pair to be stable; parity applies to the remaining evidence.
            harness_volume = copy.deepcopy(harness_boundary["volume"])
            comparator_volume = copy.deepcopy(comparator_boundary["volume"])
            for volume in (harness_volume, comparator_volume):
                volume["before_snapshot"].pop("native_handle_value", None)
                volume["after_snapshot"].pop("native_handle_value", None)
            self.assertEqual(harness_volume, comparator_volume)

            before_snapshot = {
                "available": True,
                "errors": [],
                "process_access_denied_count": 0,
                "sessions": {},
                "daemons": {},
            }

            def harness_evidence(state_record, creation_ticks: int):
                creation_ns = (
                    creation_ticks - harness.WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
                ) * harness.WINDOWS_FILETIME_TICK_NS
                key = f"winfiletime:{creation_ticks}"
                identity = f"77@{key}"
                stable = {
                    "pid": 77,
                    "identity": identity,
                    "creation_time_key": key,
                    "creation_time_unix_seconds": creation_ns / 1_000_000_000.0,
                    "creation_time_unix_ns": creation_ns,
                    "creation_filetime_ticks": creation_ticks,
                    "image_path": image_path,
                    "native_handle_held": True,
                    "terminate_access": True,
                }
                daemon = {
                    "pid": 77,
                    "identity": identity,
                    "creation_time_key": key,
                    "creation_filetime_ticks": creation_ticks,
                    "creation_time_unix_ns": creation_ns,
                    "create_time": creation_ns / 1_000_000_000.0,
                    "image_path": image_path,
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                    "command": [
                        "python",
                        "-m",
                        "rdc.daemon_server",
                        "--capture",
                        str(capture.resolve()),
                    ],
                    "native_handle_verified_during_inspection": True,
                }
                snapshot = {
                    "available": True,
                    "errors": [],
                    "process_access_denied_count": 0,
                    "sessions": {str(state_path.resolve()): state_record},
                    "daemons": {identity: daemon},
                }
                return stable, snapshot

            class FakeIdentity:
                def __init__(self, creation_ticks: int) -> None:
                    self.creation_ticks = creation_ticks
                    self.creation_ns = (
                        creation_ticks - comparator.WINDOWS_FILETIME_EPOCH_OFFSET_TICKS
                    ) * comparator.WINDOWS_FILETIME_TICK_NS
                    self.identity = f"77@winfiletime:{creation_ticks}"
                    self.closed = False
                    self.terminate_calls = 0
                    self.metadata_calls = 0

                def metadata(self):
                    self.metadata_calls += 1
                    return {
                        "pid": 77,
                        "identity": self.identity,
                        "creation_time_key": f"winfiletime:{self.creation_ticks}",
                        "creation_time_unix_seconds": self.creation_ns / 1_000_000_000.0,
                        "creation_time_unix_ns": self.creation_ns,
                        "creation_filetime_ticks": self.creation_ticks,
                        "image_path": image_path,
                        "native_handle_held": True,
                        "terminate_access": True,
                    }

                def close(self):
                    self.closed = True
                    return {"closed": True}

                def terminate(self, *, timeout: float):
                    self.terminate_calls += 1
                    return {"passed": True}

            def process_metadata(identity: FakeIdentity):
                return {
                    "pid": 77,
                    "identity": identity.identity,
                    "creation_time_key": f"winfiletime:{identity.creation_ticks}",
                    "creation_filetime_ticks": identity.creation_ticks,
                    "creation_time_unix_ns": identity.creation_ns,
                    "create_time": identity.creation_ns / 1_000_000_000.0,
                    "image_path": image_path,
                    "process_name": "rdc.exe",
                    "command": [
                        "python",
                        "-m",
                        "rdc.daemon_server",
                        "--capture",
                        str(capture.resolve()),
                    ],
                    "is_rdc_daemon": True,
                    "capture": str(capture.resolve()),
                    "native_handle_verified_during_metadata_collection": True,
                }

            def state_variant(base, volume_kind: str | None):
                variant = copy.deepcopy(base)
                if volume_kind is None:
                    return variant
                volume = variant["publication_boundary"]["volume"]
                volume.update(
                    {
                        "verified": False,
                        "supported_file_system": False,
                        "native_filetime_semantics": False,
                        "file_system": volume_kind,
                        "error": f"injected unsupported filesystem {volume_kind}",
                    }
                )
                if volume_kind == "UNKNOWN":
                    volume["local"] = False
                    volume["drive_type_code"] = 0
                    volume["drive_type"] = "unsupported"
                return variant

            open_started_ns = int(harness_boundary["modified_ns"]) - 2_000_000_000
            cases = (
                ("c-equals-p-minus-1", -1, None, True),
                ("c-equals-p", 0, None, False),
                ("c-equals-p-plus-1", 1, None, False),
                ("later-positive", 10_000_000, None, False),
                ("unknown-filesystem", -1, "UNKNOWN", False),
                ("coarse-filesystem", -1, "FAT32", False),
            )
            harness_outcomes = {}
            comparator_outcomes = {}
            for label, delta, volume_kind, expected_established in cases:
                creation_ticks = publication_ticks + delta
                harness_case_state = state_variant(harness_state, volume_kind)
                comparator_case_state = state_variant(comparator_state, volume_kind)
                stable, snapshot = harness_evidence(harness_case_state, creation_ticks)
                harness_ownership = harness.determine_named_session_daemon_ownership(
                    harness_case_state,
                    before_snapshot,
                    snapshot,
                    state_path,
                    capture,
                    stable,
                )
                direct_identity = FakeIdentity(creation_ticks)
                comparator_ownership = comparator._validate_owned_replay_daemon_binding(
                    state_record=comparator_case_state,
                    expected_state_path=state_path,
                    capture=capture,
                    stable_process_identity=direct_identity.metadata(),
                    process_metadata=process_metadata(direct_identity),
                    open_started_wall_ns=open_started_ns,
                )
                harness_outcomes[label] = (
                    harness_ownership["established"],
                    harness_ownership["strict_creation_precedes_publication"],
                    harness_ownership["creation_equals_publication"],
                    harness_ownership["state_file_volume_verified"],
                )
                comparator_outcomes[label] = (
                    comparator_ownership["established"],
                    comparator_ownership["strict_creation_precedes_publication"],
                    comparator_ownership["creation_equals_publication"],
                    comparator_ownership["state_file_volume_verified"],
                )
                self.assertEqual(
                    harness_outcomes[label],
                    comparator_outcomes[label],
                    label,
                )
                self.assertEqual(
                    expected_established,
                    harness_ownership["established"],
                    harness_ownership,
                )
                if expected_established:
                    self.assertTrue(
                        harness_ownership["strict_creation_precedes_publication"]
                    )
                    acquired = FakeIdentity(creation_ticks)
                    held, acquired_ownership = comparator._acquire_owned_replay_daemon(
                        state_record=comparator_case_state,
                        expected_state_path=state_path,
                        capture=capture,
                        open_started_wall_ns=open_started_ns,
                        process_identity_factory=lambda pid, require_terminate: acquired,
                        process_metadata_collector=process_metadata,
                    )
                    self.assertIs(held, acquired)
                    self.assertTrue(acquired_ownership["established"])
                    held.close()
                    continue

                rejected = FakeIdentity(creation_ticks)
                with self.assertRaises(comparator.ComparatorError):
                    comparator._acquire_owned_replay_daemon(
                        state_record=comparator_case_state,
                        expected_state_path=state_path,
                        capture=capture,
                        open_started_wall_ns=open_started_ns,
                        process_identity_factory=lambda pid, require_terminate: rejected,
                        process_metadata_collector=process_metadata,
                    )
                self.assertTrue(rejected.closed, label)
                self.assertEqual(0, rejected.terminate_calls, label)

                shutdown_calls = []
                harness_rejected = FakeIdentity(creation_ticks)
                shutdown = harness.shutdown_owned_rdc_session_direct(
                    state_path=state_path,
                    state_after_open=harness_case_state,
                    process_identity=harness_rejected,
                    ownership=harness_ownership,
                    timeout=1.0,
                    send_request_fn=lambda *args, **kwargs: shutdown_calls.append(
                        (args, kwargs)
                    ),
                    shutdown_request_fn=lambda *args, **kwargs: {"method": "shutdown"},
                )
                self.assertFalse(shutdown["graceful_requested"], label)
                self.assertFalse(shutdown["passed"], label)
                self.assertEqual([], shutdown_calls, label)
                self.assertEqual(0, harness_rejected.metadata_calls, label)
                self.assertEqual(0, harness_rejected.terminate_calls, label)

            self.assertEqual(
                {
                    "c-equals-p-minus-1": (True, True, False, True),
                    "c-equals-p": (False, False, True, True),
                    "c-equals-p-plus-1": (False, False, False, True),
                    "later-positive": (False, False, False, True),
                    "unknown-filesystem": (False, False, False, False),
                    "coarse-filesystem": (False, False, False, False),
                },
                harness_outcomes,
            )

    def test_rdc_cli_daemon_precedes_named_state_publication(self) -> None:
        from tools import rdc_csm_motion_compare as comparator
        from tools import run_csm_shadow_motion_smoke as harness

        harness_package = harness.collect_rdc_cli_package_evidence()
        comparator_package = comparator._collect_rdc_cli_package_evidence()
        self.assertEqual("0.6.1", harness_package["version"])
        self.assertEqual(harness_package["sha256"], comparator_package["sha256"])
        harness_ordering = harness_package["session_publication_ordering"]
        comparator_ordering = comparator_package["session_publication_ordering"]
        self.assertEqual(harness_ordering, comparator_ordering)
        self.assertTrue(harness_ordering["verified"])
        self.assertTrue(harness_ordering["daemon_created_before_state_publication"])
        self.assertTrue(harness_ordering["daemon_ready_before_state_publication"])
        lines = harness_ordering["call_lines"]
        self.assertLess(lines["start_daemon"], lines["wait_for_ping"])
        self.assertLess(lines["wait_for_ping"], lines["create_session"])
        self.assertEqual(
            [],
            comparator._validate_runtime_dependency_evidence(
                comparator_package,
                role="rdc_cli_package",
            ),
        )
        tampered = copy.deepcopy(comparator_package)
        tampered["session_publication_ordering"]["call_lines"]["create_session"] = lines[
            "start_daemon"
        ]
        errors = comparator._validate_runtime_dependency_evidence(
            tampered,
            role="rdc_cli_package",
        )
        self.assertTrue(any("call order" in error for error in errors), errors)
    def test_release_profiles_validate_taa_on_and_no_post_contracts(self) -> None:
        from tools import rdc_csm_motion_compare as comparator

        with tempfile.TemporaryDirectory(prefix="rdc_protocol_release_modes_") as directory:
            root = Path(directory)
            for render_mode in ("taa-on", "no-post"):
                manifest_root = root / render_mode
                source, _, _ = comparator._self_test_case_fixture(
                    manifest_root,
                    render_mode=render_mode,
                )
                profile = comparator._release_manifest_profile(source)
                self.assertTrue(profile["passed"], profile)
                self.assertEqual(render_mode, profile["render_mode"])
                self.assertEqual(
                    [],
                    comparator._validate_release_manifest_contract(
                        source,
                        manifest_root / "manifest.json",
                    ),
                )

    def test_shadow_edge_module_remains_the_single_roi_algorithm_owner(self) -> None:
        for token in (
            "class CameraMatrices",
            "class ShadowFrame",
            "class ShadowEdgeMetricsConfig",
            "def evaluate_shadow_edge_metrics(",
            'STAGE_NAMES = ("scene_color_hdr", "history_write", "final")',
        ):
            self.assertIn(token, SHADOW_EDGE)

if __name__ == "__main__":
    unittest.main()
