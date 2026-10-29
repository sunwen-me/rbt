from __future__ import annotations

import base64
import csv
import hashlib
import importlib.util
import io
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[2] / "packaging/verify_release.py"
SPEC = importlib.util.spec_from_file_location("rbt_verify_release", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
verify_release = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify_release
SPEC.loader.exec_module(verify_release)


class SafeArchiveExtractionTests(unittest.TestCase):
    def test_discovers_sdk_prefix_above_bin(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            staging = Path(temporary)
            sdk = staging / "rbt-sdk-0.8.0-linux-x86_64"
            (sdk / "bin").mkdir(parents=True)
            (sdk / "bin/rbt").write_bytes(b"executable")
            self.assertEqual(verify_release._discover_sdk_root(staging), sdk)

    def make_archive(self, archive_path: Path, member: tarfile.TarInfo, payload: bytes = b"x") -> None:
        with tarfile.open(archive_path, "w:gz") as archive:
            if member.isfile():
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
            else:
                archive.addfile(member)

    def test_extracts_regular_files_and_contained_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive_path = root / "sdk.tar.gz"
            with tarfile.open(archive_path, "w:gz") as archive:
                binary = tarfile.TarInfo("rbt-sdk/bin/rbt")
                binary.mode = 0o755
                binary.size = 3
                archive.addfile(binary, io.BytesIO(b"rbt"))
                target = tarfile.TarInfo("rbt-sdk/lib/libsample.so.1")
                target.size = 4
                archive.addfile(target, io.BytesIO(b"elf!"))
                link = tarfile.TarInfo("rbt-sdk/lib/libsample.so")
                link.type = tarfile.SYMTYPE
                link.linkname = "libsample.so.1"
                archive.addfile(link)

            destination = root / "extracted"
            verify_release._safe_extract_tar(archive_path, destination)
            self.assertEqual((destination / "rbt-sdk/bin/rbt").read_bytes(), b"rbt")
            self.assertTrue((destination / "rbt-sdk/bin/rbt").stat().st_mode & 0o111)
            self.assertEqual((destination / "rbt-sdk/lib/libsample.so").read_bytes(), b"elf!")

    def test_rejects_traversal_absolute_paths_escaping_links_and_special_files(self) -> None:
        cases = [
            tarfile.TarInfo("../outside"),
            tarfile.TarInfo("/absolute/file"),
        ]
        escaping_link = tarfile.TarInfo("rbt-sdk/link")
        escaping_link.type = tarfile.SYMTYPE
        escaping_link.linkname = "../../outside"
        cases.append(escaping_link)
        special = tarfile.TarInfo("rbt-sdk/fifo")
        special.type = tarfile.FIFOTYPE
        cases.append(special)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, member in enumerate(cases):
                with self.subTest(member=member.name):
                    archive_path = root / f"unsafe-{index}.tar.gz"
                    destination = root / f"out-{index}"
                    self.make_archive(archive_path, member)
                    with self.assertRaises(verify_release.VerificationError):
                        verify_release._safe_extract_tar(archive_path, destination)
                    self.assertFalse((root / "outside").exists())


class RelocationAuditTests(unittest.TestCase):
    def test_accepts_only_origin_relative_sdk_rpaths(self) -> None:
        entries = verify_release._rpath_entries(
            " 0x000000001d (RUNPATH) Library runpath: [$ORIGIN/../lib:$ORIGIN]"
        )
        self.assertTrue(verify_release._rpaths_are_origin_relative(entries))
        absolute = verify_release._rpath_entries(
            " 0x000000001d (RPATH) Library rpath: [/home/build/prefix/lib]"
        )
        self.assertFalse(verify_release._rpaths_are_origin_relative(absolute))
        self.assertFalse(verify_release._rpaths_have_no_absolute_entries(absolute))

    def test_metadata_rejects_foreign_sdk_resolution_but_allows_registry_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary) / "relocated prefix with spaces/sdk root/share/rbt/rust"
            sdk.mkdir(parents=True)
            metadata = {
                "packages": [
                    {"name": "rbt", "manifest_path": str(sdk / "crates/rbt/Cargo.toml")},
                    {"name": "iceoryx2", "manifest_path": str(sdk / "third_party/iceoryx2/Cargo.toml")},
                    {"name": "serde", "manifest_path": "/home/sw/.cargo/registry/serde/Cargo.toml"},
                ]
            }
            detail = verify_release._assert_sdk_paths(metadata, sdk)
            self.assertIn("2 rbt and maintained native packages", detail)
            metadata["packages"][0]["manifest_path"] = "/home/sw/rbt/crates/rbt/Cargo.toml"
            with self.assertRaisesRegex(verify_release.VerificationError, "outside moved prefix"):
                verify_release._assert_sdk_paths(metadata, sdk)


class WheelIntegrityTests(unittest.TestCase):
    def create_wheel(self, path: Path, *, corrupt_payload: bool = False) -> None:
        dist_info = "rbt-0.8.0.dist-info"
        files = {
            f"{dist_info}/METADATA": b"Name: rbt\nVersion: 0.8.0\nLicense-File: LICENSE-MIT\nLicense-File: LICENSE-APACHE\n\n",
            f"{dist_info}/WHEEL": b"Wheel-Version: 1.0\nGenerator: test\nRoot-Is-Purelib: false\nTag: cp312-cp312-manylinux_2_28_x86_64\n",
            f"{dist_info}/licenses/LICENSE-MIT": b"MIT license text\n",
            f"{dist_info}/licenses/LICENSE-APACHE": b"Apache license text\n",
            "rbt/__init__.py": b"__version__ = '0.8.0'\n",
        }
        record = io.StringIO(newline="")
        writer = csv.writer(record, lineterminator="\n")
        for name, payload in files.items():
            digest = base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).decode("ascii").rstrip("=")
            writer.writerow((name, f"sha256={digest}", str(len(payload))))
        writer.writerow((f"{dist_info}/RECORD", "", ""))
        files[f"{dist_info}/RECORD"] = record.getvalue().encode("utf-8")
        if corrupt_payload:
            files["rbt/__init__.py"] = b"changed after RECORD\n"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as wheel:
            for name, payload in files.items():
                wheel.writestr(name, payload)

    def test_record_hashes_and_licenses_are_checked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            wheel = Path(temporary) / "rbt-0.8.0-cp312-cp312-manylinux_2_28_x86_64.whl"
            self.create_wheel(wheel)
            detail = verify_release._validate_wheel(wheel, "cp312")
            self.assertIn("RECORD hashes are valid", detail)

            corrupt = Path(temporary) / "corrupt.whl"
            self.create_wheel(corrupt, corrupt_payload=True)
            with self.assertRaisesRegex(verify_release.VerificationError, "RECORD hash or size mismatch"):
                verify_release._validate_wheel(corrupt, "cp312")


if __name__ == "__main__":
    unittest.main()
