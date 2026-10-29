from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "native_export", Path(__file__).resolve().parents[2] / "scripts/check_iceoryx2_rbt_export.py"
)
native_export = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(native_export)


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


if __name__ == "__main__":
    unittest.main()
