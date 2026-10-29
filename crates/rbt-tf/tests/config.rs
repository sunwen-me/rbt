use rbt_tf::{TfError, config::StaticExtrinsicsConfig};
use rbt_time::HostClock;

const ONE_EDGE: &str = r#"
[[transforms.static]]
parent = "base_link"
child = "camera_link"
translation_m = [0.25, 0.0, 0.42]
rotation_xyzw = [0.0, 0.0, 0.0, 1.0]
"#;

#[test]
fn parses_complete_static_table_and_preserves_set_metadata() {
    let config = StaticExtrinsicsConfig::from_toml(ONE_EDGE, 8).unwrap();
    assert_eq!(config.edges().len(), 1);
    assert_eq!(config.edges()[0].parent.as_str(), "base_link");
    assert_eq!(config.edges()[0].child.as_str(), "camera_link");

    let effective_from = HostClock::new().unwrap().monotonic_now().unwrap();
    let set = config.into_static_set(7, effective_from, None);
    assert_eq!(set.revision, 7);
    assert_eq!(set.effective_from, effective_from);
    assert_eq!(set.effective_uncertainty_ns, None);
    assert_eq!(set.transforms.len(), 1);
}

#[test]
fn rejects_unrecognized_fields_at_every_level() {
    let unknown_root = format!("extra = true\n{ONE_EDGE}");
    let unknown_transform_table = format!("{ONE_EDGE}\n[transforms]\nother = true\n");
    let unknown_edge = ONE_EDGE.replace("parent =", "extra = 1\nparent =");

    assert_invalid(&unknown_root);
    assert_invalid(&unknown_transform_table);
    assert_invalid(&unknown_edge);
}

#[test]
fn rejects_invalid_names_self_edges_duplicate_children_and_cycles() {
    let invalid_name = ONE_EDGE.replace("parent = \"base_link\"", "parent = \"\"");
    let self_edge = ONE_EDGE.replace("child = \"camera_link\"", "child = \"base_link\"");
    let duplicate_child = format!(
        "{ONE_EDGE}\n[[transforms.static]]\nparent = \"other_base\"\nchild = \"camera_link\"\ntranslation_m = [0.0, 0.0, 0.0]\nrotation_xyzw = [0.0, 0.0, 0.0, 1.0]\n"
    );
    let cycle = r#"[[transforms.static]]
parent = "frame_b"
child = "frame_a"
translation_m = [0.0, 0.0, 0.0]
rotation_xyzw = [0.0, 0.0, 0.0, 1.0]

[[transforms.static]]
parent = "frame_a"
child = "frame_b"
translation_m = [0.0, 0.0, 0.0]
rotation_xyzw = [0.0, 0.0, 0.0, 1.0]
"#;

    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&invalid_name, 8),
        Err(TfError::InvalidFrame)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&self_edge, 8),
        Err(TfError::CycleDetected)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&duplicate_child, 8),
        Err(TfError::ConflictingParent)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(cycle, 8),
        Err(TfError::CycleDetected)
    ));
}

#[test]
fn rejects_wrong_array_lengths_and_invalid_numbers() {
    let wrong_translation_length = ONE_EDGE.replace(
        "translation_m = [0.25, 0.0, 0.42]",
        "translation_m = [0.25, 0.0]",
    );
    let wrong_rotation_length = ONE_EDGE.replace(
        "rotation_xyzw = [0.0, 0.0, 0.0, 1.0]",
        "rotation_xyzw = [0.0, 0.0, 1.0]",
    );
    let nonfinite_translation = ONE_EDGE.replace(
        "translation_m = [0.25, 0.0, 0.42]",
        "translation_m = [nan, 0.0, 0.42]",
    );
    let zero_quaternion = ONE_EDGE.replace(
        "rotation_xyzw = [0.0, 0.0, 0.0, 1.0]",
        "rotation_xyzw = [0.0, 0.0, 0.0, 0.0]",
    );
    let nonunit_quaternion = ONE_EDGE.replace(
        "rotation_xyzw = [0.0, 0.0, 0.0, 1.0]",
        "rotation_xyzw = [0.0, 0.0, 0.0, 2.0]",
    );

    assert_invalid(&wrong_translation_length);
    assert_invalid(&wrong_rotation_length);
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&nonfinite_translation, 8),
        Err(TfError::InvalidTransform)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&zero_quaternion, 8),
        Err(TfError::InvalidQuaternion)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&nonunit_quaternion, 8),
        Err(TfError::InvalidQuaternion)
    ));
}

#[test]
fn rejects_oversized_input_and_transform_count_during_parse() {
    let two_edges = format!("{ONE_EDGE}\n{ONE_EDGE}");
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&two_edges, 1),
        Err(TfError::InvalidConfig)
    ));
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(ONE_EDGE, 4097),
        Err(TfError::InvalidConfig)
    ));

    let oversized = " ".repeat(1024 * 1024 + 1);
    assert!(matches!(
        StaticExtrinsicsConfig::from_toml(&oversized, 8),
        Err(TfError::SnapshotTooLarge)
    ));
}

fn assert_invalid(source: &str) {
    assert!(StaticExtrinsicsConfig::from_toml(source, 8).is_err());
}
