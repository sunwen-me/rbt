from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "native_export", Path(__file__).resolve().parents[2] / "scripts/check_iceoryx2_rbt_export.py"
)
native_export = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(native_export)

PACKAGER_SPEC = importlib.util.spec_from_file_location(
    "package_release", Path(__file__).resolve().parents[2] / "packaging/package_release.py"
)
assert PACKAGER_SPEC is not None and PACKAGER_SPEC.loader is not None
package_release = importlib.util.module_from_spec(PACKAGER_SPEC)
sys.modules[PACKAGER_SPEC.name] = package_release
PACKAGER_SPEC.loader.exec_module(package_release)


class NativeExportTests(unittest.TestCase):
    def test_frozen_export_detects_tampering_missing_extra_files_and_manifest_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            code = source / "lib.rs"
            code.write_bytes(b"audited source\n")
            metadata = root / "UPSTREAM.toml"
            metadata.write_bytes(b"pinned provenance\n")
            manifest = root / "EXPORT_MANIFEST.json"
            manifest.write_text(json.dumps({
                "files": {"lib.rs": {"type": "file", "executable": False,
                    "sha256": hashlib.sha256(code.read_bytes()).hexdigest()}},
                "provenance": {"UPSTREAM.toml": hashlib.sha256(metadata.read_bytes()).hexdigest()},
            }))
            with patch.object(native_export, "MANIFEST_SHA256", hashlib.sha256(manifest.read_bytes()).hexdigest()):
                self.assertEqual(native_export.verify(source, manifest), 1)
                code.write_bytes(b"tampered\n")
                with self.assertRaisesRegex(ValueError, "content"):
                    native_export.verify(source, manifest)
                code.unlink()
                with self.assertRaisesRegex(ValueError, "missing"):
                    native_export.verify(source, manifest)
                code.write_bytes(b"audited source\n")
                extra = source / "extra.rs"
                extra.write_bytes(b"new code")
                with self.assertRaisesRegex(ValueError, "unexpected"):
                    native_export.verify(source, manifest)
                extra.unlink()
                metadata.write_bytes(b"tampered provenance")
                with self.assertRaisesRegex(ValueError, "provenance"):
                    native_export.verify(source, manifest)
                manifest.write_bytes(b"{}")
                with self.assertRaisesRegex(ValueError, "manifest"):
                    native_export.verify(source, manifest)

    def test_source_archive_excludes_robot_workspace_compatibility_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "framework"
            sibling = Path(temporary) / "robot-workspace"
            root.mkdir()
            sibling.mkdir()
            for directory in ("scripts", "packaging", "tests", "docs", "benchmarks"):
                (root / directory).mkdir()
            (root / "README.md").write_text("framework source\n", encoding="utf-8")
            (root / "PROJECT_STATUS.md").write_text("# Scope\n", encoding="utf-8")
            (root / "scripts/verify.sh").write_text("#!/bin/sh\n", encoding="utf-8")
            (root / "packaging/package_release.py").write_text("# packager\n", encoding="utf-8")
            (root / "tests/test_release.py").write_text("# test\n", encoding="utf-8")
            (root / "docs/README.md").write_text("# docs\n", encoding="utf-8")
            benchmark = root / "benchmarks/ros2_transport"
            (benchmark / "target").mkdir(parents=True)
            (benchmark / "build").mkdir()
            (benchmark / "README.md").write_text("# ROS 2 transport benchmark\n", encoding="utf-8")
            (benchmark / "benchmark.cpp").write_text("// benchmark source\n", encoding="utf-8")
            (benchmark / "run.sh").write_text("#!/bin/sh\n", encoding="utf-8")
            (benchmark / "target/benchmark-binary").write_text("generated binary\n", encoding="utf-8")
            (benchmark / "build/compile_commands.json").write_text("[]\n", encoding="utf-8")

            (root / "drivers").symlink_to(sibling, target_is_directory=True)
            (root / "navigation").symlink_to(sibling, target_is_directory=True)
            (root / "perception").symlink_to(sibling, target_is_directory=True)
            (root / "packaging/addons").symlink_to(sibling, target_is_directory=True)
            (root / "packaging/package_addons_source.py").symlink_to(
                sibling / "package_addons_source.py"
            )
            (root / "scripts/package_addons_source.sh").symlink_to(
                sibling / "package_addons_source.sh"
            )
            (root / "tests/test_consume.py").symlink_to(sibling / "test_consume.py")

            archive_path = Path(temporary) / "source.tar.gz"
            result = package_release.make_source_archive(root, archive_path, 0)
            self.assertNotIn("drivers", result["allowlist"])
            with tarfile.open(archive_path, "r:gz") as archive:
                names = archive.getnames()

            self.assertIn("rbt-0.8.0-source/README.md", names)
            self.assertIn("rbt-0.8.0-source/PROJECT_STATUS.md", names)
            self.assertIn("rbt-0.8.0-source/benchmarks/ros2_transport/benchmark.cpp", names)
            self.assertIn("rbt-0.8.0-source/benchmarks/ros2_transport/run.sh", names)
            self.assertNotIn("rbt-0.8.0-source/benchmarks/ros2_transport/target/benchmark-binary", names)
            self.assertNotIn("rbt-0.8.0-source/benchmarks/ros2_transport/build/compile_commands.json", names)
            for excluded in (
                "drivers",
                "navigation",
                "perception",
                "packaging/addons",
                "packaging/package_addons_source.py",
                "scripts/package_addons_source.sh",
                "tests/test_consume.py",
            ):
                self.assertFalse(
                    any(
                        name == f"rbt-0.8.0-source/{excluded}"
                        or name.startswith(f"rbt-0.8.0-source/{excluded}/")
                        for name in names
                    ),
                    f"robot workspace link was included in source archive: {excluded}",
                )


if __name__ == "__main__":
    unittest.main()
