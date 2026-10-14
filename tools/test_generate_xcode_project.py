import importlib.util
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "generate_xcode_project.py"
SPEC = importlib.util.spec_from_file_location("generate_xcode_project", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
GENERATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GENERATOR
SPEC.loader.exec_module(GENERATOR)


def create_xcode(root: Path, name: str, version: str, build: str) -> Path:
    app = root / name
    contents = app / "Contents"
    xcodebuild = contents / "Developer" / "usr" / "bin" / "xcodebuild"
    xcodebuild.parent.mkdir(parents=True)
    xcodebuild.write_text("mock xcodebuild\n", encoding="utf-8")
    with (contents / "Info.plist").open("wb") as stream:
        plistlib.dump(
            {
                "CFBundleIdentifier": "com.apple.dt.Xcode",
                "CFBundleShortVersionString": version,
                "DTXcodeBuild": build,
            },
            stream,
        )
    with (contents / "version.plist").open("wb") as stream:
        plistlib.dump(
            {
                "CFBundleShortVersionString": version,
                "ProductBuildVersion": build,
            },
            stream,
        )
    return app


class XcodeSelectionTests(unittest.TestCase):
    def test_xcode_27_beta_wins_over_xcode_26_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            create_xcode(root, "Xcode 26.app", "26.3", "17C529")
            beta = create_xcode(root, "Xcode-27-beta.app", "27.0", "27A5237l")

            installations = GENERATOR.discover_xcodes(
                search_roots=(root,), include_system_selection=False
            )
            selected = GENERATOR.select_xcode(installations)

            self.assertEqual(selected.app_path, beta.absolute())
            self.assertEqual(selected.major, 27)
            self.assertTrue(selected.is_prerelease)

    def test_release_wins_prerelease_tie(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            create_xcode(root, "Xcode-27-beta.app", "27.0", "27A5000a")
            release = create_xcode(root, "Xcode 27.app", "27.0", "27A5000b")

            selected = GENERATOR.select_xcode(
                GENERATOR.discover_xcodes(
                    search_roots=(root,), include_system_selection=False
                )
            )

            self.assertEqual(selected.app_path, release.absolute())
            self.assertFalse(selected.is_prerelease)

    def test_unsupported_xcode_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            create_xcode(root, "Xcode 16.app", "16.4", "16F6")

            installations = GENERATOR.discover_xcodes(
                search_roots=(root,), include_system_selection=False
            )
            with self.assertRaisesRegex(GENERATOR.GenerationError, "Xcode 26 or newer"):
                GENERATOR.select_xcode(installations)

    def test_default_directory_and_command_use_selected_major(self) -> None:
        source = Path("/source tree")
        build = GENERATOR.default_build_dir(source, "macos", 27)
        command = GENERATOR.configure_command(source, build, "macos", ("-DBUILD_TESTING=OFF",))

        self.assertEqual(build, Path("/source tree/build-macos-xcode27"))
        self.assertEqual(command[:7], ["cmake", "-S", str(source), "-B", str(build), "-G", "Xcode"])
        self.assertIn("-DCMAKE_OSX_SYSROOT=macosx", command)
        self.assertEqual(command[-1], "-DBUILD_TESTING=OFF")

    def test_explicit_developer_directory_is_supported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            app = create_xcode(
                Path(temporary_directory), "Xcode-27-beta.app", "27.0", "27A5237l"
            )

            selected = GENERATOR.resolve_explicit_xcode(app / "Contents" / "Developer")

            self.assertEqual(selected.app_path, app.absolute())
            self.assertEqual(selected.major, 27)


if __name__ == "__main__":
    unittest.main()
