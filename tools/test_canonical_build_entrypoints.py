import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
WRAPPERS = ("build_debug_with_vsdevcmd.cmd", "build_ninja.bat")
TEMP_PARENT = ROOT


def write_text(path: Path, text: str, newline: str = "\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline=newline) as stream:
        stream.write(text)


class CanonicalBuildWrapperBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="mgif build entrypoints ", dir=TEMP_PARENT)
        self.addCleanup(self.temp_dir.cleanup)
        self.repo = Path(self.temp_dir.name) / "repo with spaces"
        self.repo.mkdir(parents=True)
        self.assertIn(" ", str(self.repo))

        for wrapper in WRAPPERS:
            shutil.copy2(ROOT / wrapper, self.repo / wrapper)

        self.mock_bin = self.repo / "mock tools"
        self.mock_bin.mkdir()
        self.cmake_log = self.repo / "cmake calls.jsonl"
        self.run_index = 0
        self.compiler = (
            self.repo
            / "fake Visual Studio"
            / "VC"
            / "Tools"
            / "MSVC"
            / "14.99.99999"
            / "bin"
            / "Hostx64"
            / "x64"
            / "cl.exe"
        )
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_bytes(b"mock cl")

        self.toolchain = self.mock_bin / "toolchain setup.cmd"
        write_text(
            self.toolchain,
            textwrap.dedent(
                """\
                @echo off
                set "VSCMD_ARG_TGT_ARCH=x64"
                if defined MGIF_TEST_TOOLCHAIN_ARCH set "VSCMD_ARG_TGT_ARCH=%MGIF_TEST_TOOLCHAIN_ARCH%"
                if defined MGIF_TEST_TOOLCHAIN_EXIT exit /b %MGIF_TEST_TOOLCHAIN_EXIT%
                exit /b 0
                """
            ),
            newline="\r\n",
        )

        mock_cmake = self.mock_bin / "mock_cmake.py"
        write_text(
            mock_cmake,
            textwrap.dedent(
                r"""
                import json
                import os
                from pathlib import Path
                import sys


                def env_int(name: str) -> int:
                    return int(os.environ.get(name, "0"))


                def write_tree(build_dir: Path, source_root: Path) -> None:
                    build_dir.mkdir(parents=True, exist_ok=True)
                    cxx_compiler = Path(os.environ["MGIF_TEST_COMPILER"])
                    c_compiler = Path(os.environ.get("MGIF_TEST_C_COMPILER", str(cxx_compiler)))
                    home = os.environ.get("MGIF_TEST_HOME") or source_root.as_posix()
                    generator = os.environ.get("MGIF_TEST_GENERATOR", "Ninja")
                    build_type = os.environ.get("MGIF_TEST_BUILD_TYPE", "Debug")
                    cxx_compiler_id = os.environ.get("MGIF_TEST_COMPILER_ID", "MSVC")
                    cxx_compiler_arch = os.environ.get("MGIF_TEST_COMPILER_ARCH", "x64")
                    c_compiler_id = os.environ.get("MGIF_TEST_C_COMPILER_ID", "MSVC")
                    c_compiler_arch = os.environ.get("MGIF_TEST_C_COMPILER_ARCH", "x64")
                    graph_output = os.environ.get("MGIF_TEST_GRAPH_OUTPUT", "Demo.exe")
                    graph_alias = os.environ.get("MGIF_TEST_GRAPH_ALIAS", graph_output)
                    graph_config = os.environ.get("MGIF_TEST_GRAPH_CONFIG", "Debug")

                    cache = "\n".join(
                        (
                            f"CMAKE_BUILD_TYPE:STRING={build_type}",
                            f"CMAKE_CXX_COMPILER:STRING={cxx_compiler.as_posix()}",
                            f"CMAKE_C_COMPILER:STRING={c_compiler.as_posix()}",
                            "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=9",
                            "CMAKE_CACHE_MINOR_VERSION:INTERNAL=9",
                            "CMAKE_CACHE_PATCH_VERSION:INTERNAL=9",
                            f"CMAKE_GENERATOR:INTERNAL={generator}",
                            f"CMAKE_HOME_DIRECTORY:INTERNAL={home}",
                        )
                    )
                    (build_dir / "CMakeCache.txt").write_text(cache + "\n", encoding="utf-8")

                    metadata_dir = build_dir / "CMakeFiles" / "9.9.9"
                    metadata_dir.mkdir(parents=True, exist_ok=True)
                    for language, compiler, compiler_id, compiler_arch in (
                        ("CXX", cxx_compiler, cxx_compiler_id, cxx_compiler_arch),
                        ("C", c_compiler, c_compiler_id, c_compiler_arch),
                    ):
                        (metadata_dir / f"CMake{language}Compiler.cmake").write_text(
                            "\n".join(
                                (
                                    f'set(CMAKE_{language}_COMPILER "{compiler.as_posix()}")',
                                    f'set(CMAKE_{language}_COMPILER_ID "{compiler_id}")',
                                    f'set(CMAKE_{language}_COMPILER_ARCHITECTURE_ID "{compiler_arch}")',
                                )
                            )
                            + "\n",
                            encoding="utf-8",
                        )
                    (build_dir / "build.ninja").write_text(
                        f"build {graph_output}: CXX_EXECUTABLE_LINKER__Demo_{graph_config} input.obj\n"
                        f"build Demo: phony {graph_alias}\n",
                        encoding="utf-8",
                    )


                args = sys.argv[1:]
                log_path = Path(os.environ["MGIF_TEST_CMAKE_LOG"])
                with log_path.open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(args) + "\n")

                if args and args[0] == "-S":
                    configure_exit = env_int("MGIF_TEST_CONFIGURE_EXIT")
                    if configure_exit:
                        raise SystemExit(configure_exit)
                    source_root = Path(args[args.index("-S") + 1])
                    build_dir = Path(args[args.index("-B") + 1])
                    write_tree(build_dir, source_root)
                    raise SystemExit(0)

                if args and args[0] == "--build":
                    build_dir = Path(args[args.index("--build") + 1])
                    if "-n" in args:
                        dry_run_exit = env_int("MGIF_TEST_DRY_RUN_EXIT")
                        if dry_run_exit:
                            raise SystemExit(dry_run_exit)
                        if os.environ.get("MGIF_TEST_DRY_RUN_LINK_PENDING") == "1":
                            print("[dry-run] Linking CXX executable Demo.exe")
                        else:
                            print("ninja: no work to do.")
                        if os.environ.get("MGIF_TEST_MUTATE_GRAPH_DURING_DRY_RUN") == "1":
                            (build_dir / "build.ninja").write_text(
                                "build Debug/Demo.exe: CXX_EXECUTABLE_LINKER__Demo_Debug input.obj\n"
                                "build Demo: phony Debug/Demo.exe\n",
                                encoding="utf-8",
                            )
                    else:
                        build_exit = env_int("MGIF_TEST_BUILD_EXIT")
                        if build_exit:
                            raise SystemExit(build_exit)
                        if os.environ.get("MGIF_TEST_CREATE_DEMO", "1") == "1":
                            (build_dir / "Demo.exe").write_bytes(b"built-by-mock")
                            print("Linking CXX executable Demo.exe")
                        else:
                            print("ninja: no work to do.")
                        if os.environ.get("MGIF_TEST_MUTATE_GRAPH_AFTER_BUILD") == "1":
                            (build_dir / "build.ninja").write_text(
                                "build Debug/Demo.exe: CXX_EXECUTABLE_LINKER__Demo_Debug input.obj\n"
                                "build Demo: phony Debug/Demo.exe\n",
                                encoding="utf-8",
                            )
                    raise SystemExit(0)

                raise SystemExit(64)
                """
            ),
        )

        cmake_cmd = self.mock_bin / "cmake.cmd"
        write_text(
            cmake_cmd,
            textwrap.dedent(
                f"""\
                @echo off
                "{sys.executable}" -B "%~dp0mock_cmake.py" %*
                exit /b %errorlevel%
                """
            ),
            newline="\r\n",
        )

        self.base_env = os.environ.copy()
        for key in list(self.base_env):
            if key.startswith("MGIF_TEST_") or key == "MGIF_TOOLCHAIN_SETUP":
                self.base_env.pop(key, None)
        self.base_env.update(
            {
                "MGIF_TOOLCHAIN_SETUP": str(self.toolchain),
                "MGIF_TEST_CMAKE_LOG": str(self.cmake_log),
                "MGIF_TEST_COMPILER": str(self.compiler),
                "MGIF_TEST_C_COMPILER": str(self.compiler),
                "PATH": str(self.mock_bin) + os.pathsep + self.base_env.get("PATH", ""),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )

    @property
    def build_dir(self) -> Path:
        return self.repo / "out" / "build" / "x64-debug"

    @property
    def demo(self) -> Path:
        return self.build_dir / "Demo.exe"

    def reset_build(self) -> None:
        shutil.rmtree(self.repo / "out", ignore_errors=True)
        self.cmake_log.unlink(missing_ok=True)

    def write_tree(
        self,
        *,
        generator: str = "Ninja",
        build_type: str = "Debug",
        home: str | Path | None = None,
        compiler: Path | None = None,
        compiler_id: str = "MSVC",
        compiler_arch: str = "x64",
        c_compiler: Path | None = None,
        c_compiler_id: str = "MSVC",
        c_compiler_arch: str = "x64",
        graph_output: str = "Demo.exe",
        graph_alias: str | None = None,
        graph_config: str = "Debug",
    ) -> None:
        compiler = compiler or self.compiler
        c_compiler = c_compiler or compiler
        for compiler_path in (compiler, c_compiler):
            compiler_path.parent.mkdir(parents=True, exist_ok=True)
            compiler_path.touch(exist_ok=True)
        home_value = self.repo.as_posix() if home is None else str(home).replace("\\", "/")
        graph_alias = graph_output if graph_alias is None else graph_alias

        self.build_dir.mkdir(parents=True, exist_ok=True)
        cache = "\n".join(
            (
                f"CMAKE_BUILD_TYPE:STRING={build_type}",
                f"CMAKE_CXX_COMPILER:STRING={compiler.as_posix()}",
                f"CMAKE_C_COMPILER:STRING={c_compiler.as_posix()}",
                "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=9",
                "CMAKE_CACHE_MINOR_VERSION:INTERNAL=9",
                "CMAKE_CACHE_PATCH_VERSION:INTERNAL=9",
                f"CMAKE_GENERATOR:INTERNAL={generator}",
                f"CMAKE_HOME_DIRECTORY:INTERNAL={home_value}",
            )
        )
        (self.build_dir / "CMakeCache.txt").write_text(cache + "\n", encoding="utf-8")

        metadata_dir = self.build_dir / "CMakeFiles" / "9.9.9"
        metadata_dir.mkdir(parents=True, exist_ok=True)
        for language, compiler_path, metadata_id, metadata_arch in (
            ("CXX", compiler, compiler_id, compiler_arch),
            ("C", c_compiler, c_compiler_id, c_compiler_arch),
        ):
            (metadata_dir / f"CMake{language}Compiler.cmake").write_text(
                "\n".join(
                    (
                        f'set(CMAKE_{language}_COMPILER "{compiler_path.as_posix()}")',
                        f'set(CMAKE_{language}_COMPILER_ID "{metadata_id}")',
                        f'set(CMAKE_{language}_COMPILER_ARCHITECTURE_ID "{metadata_arch}")',
                    )
                )
                + "\n",
                encoding="utf-8",
            )
        (self.build_dir / "build.ninja").write_text(
            f"build {graph_output}: CXX_EXECUTABLE_LINKER__Demo_{graph_config} input.obj\n"
            f"build Demo: phony {graph_alias}\n",
            encoding="utf-8",
        )

    def run_wrapper(
        self,
        wrapper: str,
        *args: str,
        env_overrides: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = self.base_env.copy()
        if env_overrides:
            env.update(env_overrides)
        self.run_index += 1
        stdout_path = self.repo / f"wrapper-{self.run_index}.stdout.txt"
        stderr_path = self.repo / f"wrapper-{self.run_index}.stderr.txt"
        with stdout_path.open("wb") as stdout_stream, stderr_path.open("wb") as stderr_stream:
            completed = subprocess.run(
                [env.get("COMSPEC", "cmd.exe"), "/d", "/c", wrapper, *args],
                cwd=self.repo,
                env=env,
                stdout=stdout_stream,
                stderr=stderr_stream,
                timeout=15,
                check=False,
            )
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        return subprocess.CompletedProcess(completed.args, completed.returncode, stdout, stderr)

    def cmake_calls(self) -> list[list[str]]:
        if not self.cmake_log.exists():
            return []
        return [
            json.loads(line)
            for line in self.cmake_log.read_text(encoding="utf-8").splitlines()
            if line
        ]

    def assert_no_proof_files(self) -> None:
        self.assertEqual(list(self.build_dir.glob(".mgif-demo-*")), [])

    def test_valid_existing_cache_is_reused_with_no_work_semantics(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                self.reset_build()
                self.write_tree(home=self.repo.as_posix() + "/./")
                self.demo.write_bytes(b"up-to-date-canonical-demo")

                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_CREATE_DEMO": "0"},
                )

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                build_call = ["--build", str(self.build_dir), "--target", "Demo"]
                self.assertEqual(self.cmake_calls(), [build_call, build_call + ["--", "-n"]])
                self.assertEqual(self.demo.read_bytes(), b"up-to-date-canonical-demo")
                self.assertIn("definitively current", result.stdout)
                self.assert_no_proof_files()

    def test_missing_cache_configures_ninja_then_builds_canonical_demo(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                self.reset_build()

                result = self.run_wrapper(wrapper)

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                calls = self.cmake_calls()
                self.assertEqual(len(calls), 3)
                self.assertEqual(
                    calls[0],
                    [
                        "-S",
                        str(self.repo),
                        "-B",
                        str(self.build_dir),
                        "-G",
                        "Ninja",
                        "-DCMAKE_BUILD_TYPE=Debug",
                    ],
                )
                build_call = ["--build", str(self.build_dir), "--target", "Demo"]
                self.assertEqual(calls[1:], [build_call, build_call + ["--", "-n"]])
                self.assertEqual(self.demo.read_bytes(), b"built-by-mock")
                self.assertIn("newly produced or updated", result.stdout)
                self.assert_no_proof_files()

    def test_incompatible_existing_cache_fails_before_cmake_build(self) -> None:
        wrong_compiler = self.repo / "fake LLVM" / "bin" / "clang-cl.exe"
        second_msvc_compiler = (
            self.repo
            / "other Visual Studio"
            / "VC"
            / "Tools"
            / "MSVC"
            / "14.88.88888"
            / "bin"
            / "Hostx64"
            / "x64"
            / "cl.exe"
        )
        cases = (
            ("generator prefix", {"generator": "Ninja Multi-Config"}, "CMAKE_GENERATOR=Ninja"),
            ("build type prefix", {"build_type": "DebugSanitized"}, "CMAKE_BUILD_TYPE=Debug"),
            ("relative home", {"home": "."}, "CMAKE_HOME_DIRECTORY must be an absolute path"),
            (
                "wrong home",
                {"home": self.repo.parent / "different repo"},
                "CMAKE_HOME_DIRECTORY mismatch",
            ),
            ("wrong CXX architecture", {"compiler_arch": "x86"}, "CMAKE_CXX_COMPILER_ARCHITECTURE_ID=x64"),
            ("wrong CXX id", {"compiler_id": "Clang"}, "CMAKE_CXX_COMPILER_ID=MSVC"),
            ("mixed CXX plus x86 C", {"c_compiler_arch": "x86"}, "CMAKE_C_COMPILER_ARCHITECTURE_ID=x64"),
            ("mixed CXX plus Clang C", {"c_compiler_id": "Clang"}, "CMAKE_C_COMPILER_ID=MSVC"),
            (
                "different valid-looking C compiler",
                {"c_compiler": second_msvc_compiler},
                "must resolve to the same active x64 MSVC cl.exe",
            ),
            (
                "wrong compiler path",
                {"compiler": wrong_compiler},
                "not an x64 MSVC cl.exe",
            ),
        )
        for wrapper in WRAPPERS:
            for label, overrides, expected_error in cases:
                with self.subTest(wrapper=wrapper, case=label):
                    self.reset_build()
                    self.write_tree(**overrides)
                    self.demo.write_bytes(b"stale-demo")

                    result = self.run_wrapper(wrapper)

                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(self.cmake_calls(), [])
                    self.assertEqual(self.demo.read_bytes(), b"stale-demo")
                    self.assertIn(expected_error, result.stdout + result.stderr)
                    self.assert_no_proof_files()

    def test_c_compiler_cache_and_active_metadata_are_required(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper, case="missing cache entry"):
                self.reset_build()
                self.write_tree()
                cache_path = self.build_dir / "CMakeCache.txt"
                cache = cache_path.read_text(encoding="utf-8")
                cache_path.write_text(
                    "\n".join(
                        line
                        for line in cache.splitlines()
                        if not line.startswith("CMAKE_C_COMPILER:")
                    )
                    + "\n",
                    encoding="utf-8",
                )

                result = self.run_wrapper(wrapper)

                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.cmake_calls(), [])
                self.assertIn("exactly one CMAKE_C_COMPILER entry", result.stdout + result.stderr)

            with self.subTest(wrapper=wrapper, case="missing active metadata"):
                self.reset_build()
                self.write_tree()
                (self.build_dir / "CMakeFiles" / "9.9.9" / "CMakeCCompiler.cmake").unlink()

                result = self.run_wrapper(wrapper)

                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.cmake_calls(), [])
                self.assertIn("Missing active compiler metadata", result.stdout + result.stderr)

    def test_wrong_toolchain_architecture_fails_before_cache_or_build(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                self.reset_build()
                self.write_tree()

                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_TOOLCHAIN_ARCH": "x86"},
                )

                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.cmake_calls(), [])
                self.assertIn("must be exactly x64", result.stdout + result.stderr)

    def test_stale_demo_cannot_hide_failed_or_preexisting_divergent_build(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper, case="failed build"):
                self.reset_build()
                self.write_tree()
                self.demo.write_bytes(b"stale-demo")

                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_BUILD_EXIT": "23"},
                )

                self.assertEqual(result.returncode, 23)
                self.assertEqual(
                    self.cmake_calls(),
                    [["--build", str(self.build_dir), "--target", "Demo"]],
                )
                self.assertEqual(self.demo.read_bytes(), b"stale-demo")
                self.assert_no_proof_files()

            with self.subTest(wrapper=wrapper, case="preexisting divergent graph"):
                self.reset_build()
                self.write_tree(
                    graph_output="Debug/Demo.exe",
                    graph_alias="Debug/Demo.exe",
                )
                self.demo.write_bytes(b"stale-demo")

                result = self.run_wrapper(wrapper)

                self.assertEqual(result.returncode, 1)
                self.assertEqual(self.cmake_calls(), [])
                self.assertEqual(self.demo.read_bytes(), b"stale-demo")
                self.assertIn("canonical Demo.exe", result.stdout + result.stderr)
                self.assert_no_proof_files()

    def test_post_build_revalidation_rejects_rerun_cmake_divergence(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                self.reset_build()
                self.write_tree()
                self.demo.write_bytes(b"stale-canonical-demo")

                result = self.run_wrapper(
                    wrapper,
                    env_overrides={
                        "MGIF_TEST_CREATE_DEMO": "0",
                        "MGIF_TEST_MUTATE_GRAPH_AFTER_BUILD": "1",
                    },
                )

                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(
                    self.cmake_calls(),
                    [["--build", str(self.build_dir), "--target", "Demo"]],
                )
                self.assertEqual(self.demo.read_bytes(), b"stale-canonical-demo")
                self.assertIn("canonical Demo.exe", result.stdout + result.stderr)
                self.assert_no_proof_files()

    def test_stale_demo_requires_post_build_currentness_evidence(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                self.reset_build()
                self.write_tree()
                self.demo.write_bytes(b"stale-canonical-demo")

                result = self.run_wrapper(
                    wrapper,
                    env_overrides={
                        "MGIF_TEST_CREATE_DEMO": "0",
                        "MGIF_TEST_DRY_RUN_LINK_PENDING": "1",
                    },
                )

                build_call = ["--build", str(self.build_dir), "--target", "Demo"]
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(self.cmake_calls(), [build_call, build_call + ["--", "-n"]])
                self.assertEqual(self.demo.read_bytes(), b"stale-canonical-demo")
                self.assertIn("still schedules its linker rule", result.stdout + result.stderr)
                self.assert_no_proof_files()

    def test_configure_toolchain_build_dry_run_and_usage_exit_codes_are_preserved(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper, case="configure"):
                self.reset_build()
                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_CONFIGURE_EXIT": "17"},
                )
                self.assertEqual(result.returncode, 17)
                self.assertEqual(len(self.cmake_calls()), 1)
                self.assertFalse((self.build_dir / "CMakeCache.txt").exists())

            with self.subTest(wrapper=wrapper, case="toolchain"):
                self.reset_build()
                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_TOOLCHAIN_EXIT": "31"},
                )
                self.assertEqual(result.returncode, 31)
                self.assertEqual(self.cmake_calls(), [])

            with self.subTest(wrapper=wrapper, case="build"):
                self.reset_build()
                self.write_tree()
                result = self.run_wrapper(
                    wrapper,
                    env_overrides={"MGIF_TEST_BUILD_EXIT": "23"},
                )
                self.assertEqual(result.returncode, 23)
                self.assert_no_proof_files()

            with self.subTest(wrapper=wrapper, case="dry run"):
                self.reset_build()
                self.write_tree()
                result = self.run_wrapper(
                    wrapper,
                    "--dry-run",
                    env_overrides={"MGIF_TEST_DRY_RUN_EXIT": "29"},
                )
                self.assertEqual(result.returncode, 29)
                self.assertEqual(
                    self.cmake_calls(),
                    [["--build", str(self.build_dir), "--target", "Demo", "--", "-n"]],
                )

            with self.subTest(wrapper=wrapper, case="usage"):
                self.reset_build()
                result = self.run_wrapper(wrapper, "--unknown")
                self.assertEqual(result.returncode, 2)
                self.assertEqual(self.cmake_calls(), [])
                self.assertIn("Usage:", result.stdout)

    def test_wrapper_dry_run_revalidates_graph_without_creating_demo(self) -> None:
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper, case="valid"):
                self.reset_build()
                self.write_tree()

                result = self.run_wrapper(wrapper, "--dry-run")

                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(
                    self.cmake_calls(),
                    [["--build", str(self.build_dir), "--target", "Demo", "--", "-n"]],
                )
                self.assertFalse(self.demo.exists())
                self.assertIn("no work to do", result.stdout)

            with self.subTest(wrapper=wrapper, case="graph mutates during dry run"):
                self.reset_build()
                self.write_tree()

                result = self.run_wrapper(
                    wrapper,
                    "--dry-run",
                    env_overrides={"MGIF_TEST_MUTATE_GRAPH_DURING_DRY_RUN": "1"},
                )

                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(len(self.cmake_calls()), 1)
                self.assertFalse(self.demo.exists())
                self.assertIn("canonical Demo.exe", result.stdout + result.stderr)

class Phase7GateBehaviorTests(unittest.TestCase):
    def test_manual_smoke_prompt_uses_canonical_demo_path(self) -> None:
        spec = importlib.util.spec_from_file_location(
            "verify_phase7_gate_behavior_test",
            ROOT / "tools" / "verify_phase7_gate.py",
        )
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.results.clear()

        def fake_step(name, argv, gate="hard", skip_reason=None, cwd=None):
            module.results.append(
                {
                    "name": name,
                    "status": "SKIP" if skip_reason else "PASS",
                    "gate": gate,
                    "reason": skip_reason or "",
                }
            )
            return True

        def fake_covered(name, reason):
            module.results.append(
                {"name": name, "status": "covered", "gate": "n/a", "reason": reason}
            )

        with tempfile.TemporaryDirectory(prefix="mgif phase7 root ", dir=TEMP_PARENT) as temp_root:
            root = Path(temp_root) / "repo with spaces"
            root.mkdir()
            report = root / "report.md"
            output = io.StringIO()
            argv = [
                "verify_phase7_gate.py",
                "--root",
                str(root),
                "--report",
                str(report),
                "--skip-build",
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(module, "step", side_effect=fake_step),
                mock.patch.object(module, "step_covered", side_effect=fake_covered),
                mock.patch.object(module.os.path, "isdir", return_value=False),
                mock.patch.object(module, "write_report"),
                contextlib.redirect_stdout(output),
                self.assertRaises(SystemExit) as exit_context,
            ):
                module.main()

            self.assertEqual(exit_context.exception.code, 0)
            canonical_demo = os.path.join(
                str(root), "out", "build", "x64-debug", "Demo.exe"
            )
            rendered = output.getvalue()
            self.assertIn(f"Run {canonical_demo}", rendered)
            self.assertNotIn("x64-debug\\Debug\\Demo.exe", rendered)
            self.assertNotIn("ninja-x64-debug", rendered)


if __name__ == "__main__":
    unittest.main()
