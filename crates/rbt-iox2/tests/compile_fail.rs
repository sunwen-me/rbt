#[test]
fn received_payload_cannot_escape_its_sample_guard() {
    let test_cases = trybuild::TestCases::new();
    test_cases.compile_fail("tests/ui/received_payload_escape.rs");
}
