import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import audit_unsafe  # noqa: E402


class UnsafeAuditTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        manifest_dir = self.root / "tests/tf"
        manifest_dir.mkdir(parents=True)
        shutil.copy2(
            REPO_ROOT / "tests/tf/unsafe-baseline.json",
            manifest_dir / "unsafe-baseline.json",
        )

        manifest = json.loads(
            (manifest_dir / "unsafe-baseline.json").read_text(encoding="utf-8")
        )
        for relative_path in manifest["files"]:
            baseline = REPO_ROOT / relative_path
            source = self.root / relative_path
            snapshot = self.root / "target/tf-baseline/files" / relative_path
            source.parent.mkdir(parents=True, exist_ok=True)
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(baseline, source)
            shutil.copy2(baseline, snapshot)

        tf_path = self.root / "generated/rust/tf.rs"
        tf_path.parent.mkdir(parents=True, exist_ok=True)
        tf_path.write_text("\n".join(self.tf_markers()) + "\n", encoding="utf-8")

    def tearDown(self):
        self.temp_dir.cleanup()

    @staticmethod
    def tf_markers():
        lines = []
        for name in audit_unsafe.TF_TYPE_NAMES:
            lines.append(f"unsafe impl crate::ShmSafe for {name} {{}}")
            lines.append(
                "unsafe impl iceoryx2::prelude::ZeroCopySend for "
                f'{name} {{ unsafe fn type_name() -> &\'static str {{ "rbt.tf/{name}@1" }} }}'
            )
        return lines

    def test_accepts_pinned_legacy_markers_and_exact_tf_pairs(self):
        report = audit_unsafe.audit(self.root)

        self.assertEqual(report.legacy_count, 49)
        self.assertEqual(report.tf_generated_count, 24)
        self.assertEqual(report.total_count, 73)
        self.assertEqual(len(report.locations), 73)

    def test_rejects_new_handwritten_unsafe_location(self):
        source = self.root / "crates/rbt-extra/src/lib.rs"
        source.parent.mkdir(parents=True)
        source.write_text(
            "pub fn invalid() { unsafe { core::hint::unreachable_unchecked() } }\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(audit_unsafe.AuditError, "unexpected unsafe locations"):
            audit_unsafe.audit(self.root)

    def test_detects_unsafe_keywords_after_character_literals_and_in_attributes(self):
        source = self.root / "crates/rbt-extra/src/lib.rs"
        source.parent.mkdir(parents=True)
        source.write_text(
            "fn benign<'a>(value: &'a str) {\n"
            "    let quote = '\"';\n"
            "    let byte_quote = b'\"';\n"
            "    let _ = value;\n"
            "}\n"
            "unsafe /* split across a comment */\n"
            "fn dangerous() {}\n"
            "#[unsafe(no_mangle)]\n"
            "pub extern \"C\" fn attributed() {}\n",
            encoding="utf-8",
        )

        self.assertEqual(
            [line for line, _ in audit_unsafe._markers(source, "crates/rbt-extra/src/lib.rs")],
            [6, 8],
        )
        with self.assertRaisesRegex(audit_unsafe.AuditError, "unexpected unsafe locations"):
            audit_unsafe.audit(self.root)

    def test_ignores_unsafe_text_in_strings_comments_and_lifetimes(self):
        source = self.root / "crates/rbt-extra/src/lib.rs"
        source.parent.mkdir(parents=True)
        source.write_text(
            "struct RawIdentifier { r#unsafe: bool }\n"
            "fn example<'a>(value: &'a str) {\n"
            "    let quote = '\"';\n"
            "    let apostrophe = '\\'';\n"
            "    let unicode = 'λ';\n"
            "    let byte_quote = b'\"';\n"
            "    let escaped = \"unsafe \\\" still in a string\";\n"
            "    let raw = r##\"unsafe \"##;\n"
            "    let byte_raw = br#\"unsafe\"#;\n"
            "    let c_string = c\"unsafe\";\n"
            "    /* unsafe /* nested unsafe */ still a comment */\n"
            "    // unsafe\n"
            "    'label: loop { break 'label; }\n"
            "    let _ = value;\n"
            "}\n",
            encoding="utf-8",
        )

        self.assertEqual(audit_unsafe._markers(source, "crates/rbt-extra/src/lib.rs"), [])
        self.assertEqual(audit_unsafe.audit(self.root).total_count, 73)

    def test_clean_checkout_needs_only_the_committed_marker_manifest(self):
        shutil.rmtree(self.root / "target")
        self.assertEqual(audit_unsafe.audit(self.root).total_count, 73)

    def test_rejects_tf_marker_type_name_tampering(self):
        tf_path = self.root / "generated/rust/tf.rs"
        source = tf_path.read_text(encoding="utf-8")
        tf_path.write_text(
            source.replace('"rbt.tf/TfFrameId@1"', '"rbt.other/TfFrameId@1"', 1),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(audit_unsafe.AuditError, "unapproved unsafe marker"):
            audit_unsafe.audit(self.root)

    def test_rejects_extra_tf_unsafe_block(self):
        tf_path = self.root / "generated/rust/tf.rs"
        with tf_path.open("a", encoding="utf-8") as tf_source:
            tf_source.write("unsafe { core::hint::unreachable_unchecked() }\n")

        with self.assertRaisesRegex(audit_unsafe.AuditError, "unapproved unsafe marker"):
            audit_unsafe.audit(self.root)

    def test_preserves_original_scope_exclusions(self):
        test_source = self.root / "crates/rbt-extra/tests/unsafe.rs"
        test_source.parent.mkdir(parents=True)
        test_source.write_text("unsafe { core::hint::unreachable_unchecked() }\n", encoding="utf-8")

        generator_source = self.root / "crates/rbt-idl/src/lib.rs"
        generator_source.parent.mkdir(parents=True)
        generator_source.write_text(
            'const TEMPLATE: &str = "unsafe { ... }";\n', encoding="utf-8"
        )

        types_source = self.root / "crates/rbt-types/src/lib.rs"
        with types_source.open("a", encoding="utf-8") as types:
            types.write("\n#[cfg(test)]\nunsafe impl ShmSafe for TestOnly {}\n")

        report = audit_unsafe.audit(self.root)
        self.assertEqual(report.total_count, 73)

    def test_rejects_legacy_marker_text_changes(self):
        control = self.root / "generated/rust/control.rs"
        source = control.read_text(encoding="utf-8")
        control.write_text(
            source.replace(
                "unsafe impl crate::ShmSafe for ControlCommand {}",
                "unsafe impl crate::ShmSafe for ControlCommand { unsafe {} }",
                1,
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(audit_unsafe.AuditError, "legacy unsafe markers changed"):
            audit_unsafe.audit(self.root)


if __name__ == "__main__":
    unittest.main()
