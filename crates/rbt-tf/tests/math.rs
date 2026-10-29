use rbt_tf::{FrameId, Pose3, Quaternion, TfError, Transform3, Vec3};

fn vec(x: f64, y: f64, z: f64) -> Vec3 {
    Vec3::new(x, y, z).expect("test vector should be finite")
}

fn quat_z(degrees: f64) -> Quaternion {
    let half_angle = degrees.to_radians() / 2.0;
    Quaternion::new(0.0, 0.0, half_angle.sin(), half_angle.cos())
        .expect("test quaternion should be unit length")
}

fn close_vec(actual: Vec3, expected: Vec3) {
    let (ax, ay, az) = actual.xyz();
    let (ex, ey, ez) = expected.xyz();
    assert!((ax - ex).abs() < 1.0e-9, "x: {ax} != {ex}");
    assert!((ay - ey).abs() < 1.0e-9, "y: {ay} != {ey}");
    assert!((az - ez).abs() < 1.0e-9, "z: {az} != {ez}");
}

#[test]
fn composition_uses_target_from_source_direction() {
    let target_from_middle = Transform3::new(vec(1.0, 0.0, 0.0), quat_z(90.0));
    let middle_from_source = Transform3::new(vec(1.0, 0.0, 0.0), Quaternion::identity());

    let target_from_source = target_from_middle
        .compose(middle_from_source)
        .expect("composition should be finite");
    close_vec(target_from_source.translation(), vec(1.0, 1.0, 0.0));
    close_vec(
        target_from_source
            .apply_point(Vec3::zero())
            .expect("origin should transform"),
        vec(1.0, 1.0, 0.0),
    );
}

#[test]
fn transforms_compose_and_invert() {
    let first = Transform3::new(vec(2.0, -3.0, 1.0), quat_z(37.0));
    let second = Transform3::new(vec(-1.0, 4.0, 2.0), quat_z(-12.0));
    let composed = first.compose(second).expect("composition should succeed");
    let identity = composed
        .compose(composed.inverse().expect("inverse should succeed"))
        .expect("inverse composition should succeed");

    close_vec(identity.translation(), Vec3::zero());
    close_vec(
        identity.apply_vector(vec(1.0, 2.0, 3.0)).unwrap(),
        vec(1.0, 2.0, 3.0),
    );
}

#[test]
fn point_vector_and_pose_application_have_distinct_translation_behavior() {
    let transform = Transform3::new(vec(3.0, 4.0, 5.0), quat_z(90.0));

    close_vec(
        transform.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(0.0, 1.0, 0.0),
    );
    close_vec(
        transform.apply_point(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(3.0, 5.0, 5.0),
    );

    let pose = Pose3::new(Vec3::zero(), Quaternion::identity());
    let result = transform.apply_pose(pose).unwrap();
    close_vec(result.position(), vec(3.0, 4.0, 5.0));
    close_vec(
        Transform3::new(Vec3::zero(), result.orientation())
            .apply_vector(vec(1.0, 0.0, 0.0))
            .unwrap(),
        vec(0.0, 1.0, 0.0),
    );
}

#[test]
fn slerp_uses_shortest_path_and_handles_boundary_cases() {
    let halfway = Transform3::new(Vec3::zero(), Quaternion::identity())
        .interpolate(Transform3::new(Vec3::zero(), quat_z(90.0)), 0.5)
        .unwrap();
    close_vec(
        halfway.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(2.0_f64.sqrt() / 2.0, 2.0_f64.sqrt() / 2.0, 0.0),
    );

    let negated_identity = Quaternion::new(0.0, 0.0, 0.0, -1.0).unwrap();
    let same_rotation = Transform3::new(Vec3::zero(), Quaternion::identity())
        .interpolate(Transform3::new(Vec3::zero(), negated_identity), 0.5)
        .unwrap();
    close_vec(
        same_rotation.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(1.0, 0.0, 0.0),
    );

    let negative_dot = Transform3::new(Vec3::zero(), Quaternion::identity())
        .interpolate(Transform3::new(Vec3::zero(), quat_z(270.0)), 0.5)
        .unwrap();
    close_vec(
        negative_dot.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(2.0_f64.sqrt() / 2.0, -2.0_f64.sqrt() / 2.0, 0.0),
    );

    let near_identical = Transform3::new(Vec3::zero(), Quaternion::identity())
        .interpolate(
            Transform3::new(Vec3::zero(), quat_z(1.0e-7_f64.to_degrees())),
            0.5,
        )
        .unwrap();
    assert!(near_identical.rotation().w().is_finite());

    for dot in [0.9996_f64, 0.9995] {
        let angle = 2.0 * dot.acos();
        let near_identical = Transform3::new(Vec3::zero(), Quaternion::identity())
            .interpolate(
                Transform3::new(Vec3::zero(), quat_z(angle.to_degrees())),
                0.5,
            )
            .unwrap();
        close_vec(
            near_identical.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
            vec((angle / 2.0).cos(), (angle / 2.0).sin(), 0.0),
        );
    }

    let near_half_turn = Transform3::new(Vec3::zero(), quat_z(180.0 - 1.0e-6))
        .interpolate(Transform3::new(Vec3::zero(), quat_z(180.0 + 1.0e-6)), 0.5)
        .unwrap();
    close_vec(
        near_half_turn.apply_vector(vec(1.0, 0.0, 0.0)).unwrap(),
        vec(-1.0, 0.0, 0.0),
    );
}

#[test]
fn invalid_numeric_values_and_interpolation_amounts_are_rejected() {
    assert_eq!(
        Vec3::new(f64::NAN, 0.0, 0.0),
        Err(TfError::InvalidTransform)
    );
    assert_eq!(
        Vec3::new(0.0, f64::INFINITY, 0.0),
        Err(TfError::InvalidTransform)
    );
    assert_eq!(
        Quaternion::new(0.0, 0.0, 0.0, 0.0),
        Err(TfError::InvalidQuaternion)
    );
    assert_eq!(
        Quaternion::new(0.0, 0.0, 0.0, 2.0),
        Err(TfError::InvalidQuaternion)
    );
    assert_eq!(
        Quaternion::new(f64::NAN, 0.0, 0.0, 1.0),
        Err(TfError::InvalidQuaternion)
    );

    let identity = Transform3::identity();
    for amount in [f64::NAN, f64::INFINITY, -0.1, 1.1] {
        assert_eq!(
            identity.interpolate(identity, amount),
            Err(TfError::InvalidTransform)
        );
    }
}

#[test]
fn large_finite_values_remain_checked() {
    let largest = f64::MAX;
    let translated = Transform3::new(vec(largest, 0.0, 0.0), Quaternion::identity());
    close_vec(
        translated.apply_point(Vec3::zero()).unwrap(),
        vec(largest, 0.0, 0.0),
    );
    assert_eq!(
        translated.compose(translated),
        Err(TfError::InvalidTransform)
    );

    let canceled = translated
        .compose(translated.inverse().expect("negative max remains finite"))
        .expect("opposite translations should cancel");
    close_vec(canceled.translation(), Vec3::zero());

    let opposite = Transform3::new(vec(-largest, 0.0, 0.0), Quaternion::identity());
    let midpoint = translated.interpolate(opposite, 0.5).unwrap();
    close_vec(midpoint.translation(), Vec3::zero());
}

#[test]
fn frame_id_enforces_canonical_wire_encoding_and_serde() {
    let frame = FrameId::new("robot/base_link").unwrap();
    assert_eq!(frame.as_str(), "robot/base_link");
    assert_eq!(frame.to_string(), "robot/base_link");
    assert_eq!(FrameId::from_bytes(frame.to_bytes()).unwrap(), frame);
    assert_eq!(
        serde_json::to_string(&frame).unwrap(),
        "\"robot/base_link\""
    );
    assert_eq!(
        serde_json::from_str::<FrameId>("\"robot/base_link\"").unwrap(),
        frame
    );

    for invalid in [
        "",
        "/base",
        "base/",
        "base//link",
        ".",
        "..",
        "base/.",
        "base/..",
        "-base",
        "base link",
        "base/é",
    ] {
        assert_eq!(
            FrameId::new(invalid),
            Err(TfError::InvalidFrame),
            "{invalid:?}"
        );
    }

    let mut invalid_padding = frame.to_bytes();
    invalid_padding[63] = b'x';
    assert_eq!(
        FrameId::from_bytes(invalid_padding),
        Err(TfError::InvalidFrame)
    );
    let invalid_frame_error = serde_json::from_str::<FrameId>("\"base//link\"").unwrap_err();
    assert!(
        invalid_frame_error
            .to_string()
            .starts_with("frame identifier is invalid")
    );

    let full = "a".repeat(63);
    assert_eq!(FrameId::new(&full).unwrap().as_str(), full);
    assert_eq!(
        FrameId::new(&format!("{full}a")),
        Err(TfError::InvalidFrame)
    );
}

#[test]
fn serde_deserialization_cannot_bypass_numeric_invariants() {
    assert!(serde_json::from_str::<Vec3>(r#"{"x":0.0,"y":1e999,"z":0.0}"#).is_err());
    assert!(serde_json::from_str::<Quaternion>(r#"{"x":0.0,"y":0.0,"z":0.0,"w":2.0}"#).is_err());
    assert!(serde_json::from_str::<Transform3>(
        r#"{"translation":{"x":0.0,"y":0.0,"z":0.0},"rotation":{"x":0.0,"y":0.0,"z":0.0,"w":0.0}}"#
    )
    .is_err());

    let pose = Pose3::new(vec(1.0, 2.0, 3.0), quat_z(45.0));
    let encoded = serde_json::to_string(&pose).unwrap();
    let decoded = serde_json::from_str::<Pose3>(&encoded).unwrap();
    close_vec(decoded.position(), pose.position());
    let (x, y, z, w) = decoded.orientation().xyzw();
    let (expected_x, expected_y, expected_z, expected_w) = pose.orientation().xyzw();
    let orientation_dot = x * expected_x + y * expected_y + z * expected_z + w * expected_w;
    assert!((orientation_dot.abs() - 1.0).abs() < 1.0e-12);
}
