//! Validated three-dimensional vectors, rotations, poses, and transforms.

use serde::{Deserialize, Deserializer, Serialize, de::Error as _};

use crate::{TfError, TfResult};

const QUATERNION_TOLERANCE: f64 = 1.0e-6;
const SLERP_LINEAR_THRESHOLD: f64 = 0.9995;

/// A finite three-dimensional vector.
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct Vec3 {
    x: f64,
    y: f64,
    z: f64,
}

impl Vec3 {
    /// Constructs a vector with finite coordinates.
    pub fn new(x: f64, y: f64, z: f64) -> TfResult<Self> {
        if !x.is_finite() || !y.is_finite() || !z.is_finite() {
            return Err(TfError::InvalidTransform);
        }
        Ok(Self { x, y, z })
    }

    /// Returns the zero vector.
    pub const fn zero() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        }
    }

    /// Returns the x coordinate.
    pub const fn x(self) -> f64 {
        self.x
    }

    /// Returns the y coordinate.
    pub const fn y(self) -> f64 {
        self.y
    }

    /// Returns the z coordinate.
    pub const fn z(self) -> f64 {
        self.z
    }

    /// Returns all coordinates in x, y, z order.
    pub const fn xyz(self) -> (f64, f64, f64) {
        (self.x, self.y, self.z)
    }

    /// Checks that every coordinate is finite.
    pub fn validate(&self) -> TfResult<()> {
        if self.x.is_finite() && self.y.is_finite() && self.z.is_finite() {
            Ok(())
        } else {
            Err(TfError::InvalidTransform)
        }
    }

    fn checked_add(self, rhs: Self) -> TfResult<Self> {
        Self::new(self.x + rhs.x, self.y + rhs.y, self.z + rhs.z)
    }

    fn checked_neg(self) -> TfResult<Self> {
        Self::new(-self.x, -self.y, -self.z)
    }

    fn interpolate(self, rhs: Self, amount: f64) -> TfResult<Self> {
        Self::new(
            interpolate_scalar(self.x, rhs.x, amount),
            interpolate_scalar(self.y, rhs.y, amount),
            interpolate_scalar(self.z, rhs.z, amount),
        )
    }
}

impl<'de> Deserialize<'de> for Vec3 {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct RawVec3 {
            x: f64,
            y: f64,
            z: f64,
        }

        let raw = RawVec3::deserialize(deserializer)?;
        Self::new(raw.x, raw.y, raw.z).map_err(D::Error::custom)
    }
}

/// A unit quaternion in x, y, z, w order.
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct Quaternion {
    x: f64,
    y: f64,
    z: f64,
    w: f64,
}

impl Quaternion {
    /// Constructs a quaternion, accepting and correcting small norm error.
    pub fn new(x: f64, y: f64, z: f64, w: f64) -> TfResult<Self> {
        if !x.is_finite() || !y.is_finite() || !z.is_finite() || !w.is_finite() {
            return Err(TfError::InvalidQuaternion);
        }
        let norm = x.hypot(y).hypot(z).hypot(w);
        if !norm.is_finite() || norm == 0.0 || (norm - 1.0).abs() > QUATERNION_TOLERANCE {
            return Err(TfError::InvalidQuaternion);
        }
        Ok(Self {
            x: x / norm,
            y: y / norm,
            z: z / norm,
            w: w / norm,
        })
    }

    fn normalize_result(x: f64, y: f64, z: f64, w: f64) -> TfResult<Self> {
        if !x.is_finite() || !y.is_finite() || !z.is_finite() || !w.is_finite() {
            return Err(TfError::InvalidQuaternion);
        }
        let norm = x.hypot(y).hypot(z).hypot(w);
        if !norm.is_finite() || norm == 0.0 {
            return Err(TfError::InvalidQuaternion);
        }
        Ok(Self {
            x: x / norm,
            y: y / norm,
            z: z / norm,
            w: w / norm,
        })
    }

    /// Returns the identity rotation.
    pub const fn identity() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 1.0,
        }
    }

    /// Returns the x component.
    pub const fn x(self) -> f64 {
        self.x
    }

    /// Returns the y component.
    pub const fn y(self) -> f64 {
        self.y
    }

    /// Returns the z component.
    pub const fn z(self) -> f64 {
        self.z
    }

    /// Returns the w component.
    pub const fn w(self) -> f64 {
        self.w
    }

    /// Returns all components in x, y, z, w order.
    pub const fn xyzw(self) -> (f64, f64, f64, f64) {
        (self.x, self.y, self.z, self.w)
    }

    fn conjugate(self) -> Self {
        Self {
            x: -self.x,
            y: -self.y,
            z: -self.z,
            w: self.w,
        }
    }

    fn checked_mul(self, rhs: Self) -> TfResult<Self> {
        Self::new(
            self.w * rhs.x + self.x * rhs.w + self.y * rhs.z - self.z * rhs.y,
            self.w * rhs.y - self.x * rhs.z + self.y * rhs.w + self.z * rhs.x,
            self.w * rhs.z + self.x * rhs.y - self.y * rhs.x + self.z * rhs.w,
            self.w * rhs.w - self.x * rhs.x - self.y * rhs.y - self.z * rhs.z,
        )
    }

    fn slerp(self, mut rhs: Self, amount: f64) -> TfResult<Self> {
        if amount == 0.0 {
            return Ok(self);
        }
        if amount == 1.0 {
            return Ok(rhs);
        }

        let mut dot = self.x * rhs.x + self.y * rhs.y + self.z * rhs.z + self.w * rhs.w;
        if dot < 0.0 {
            rhs = Self {
                x: -rhs.x,
                y: -rhs.y,
                z: -rhs.z,
                w: -rhs.w,
            };
            dot = -dot;
        }
        dot = dot.clamp(-1.0, 1.0);

        if dot > SLERP_LINEAR_THRESHOLD {
            return Self::normalize_result(
                interpolate_scalar(self.x, rhs.x, amount),
                interpolate_scalar(self.y, rhs.y, amount),
                interpolate_scalar(self.z, rhs.z, amount),
                interpolate_scalar(self.w, rhs.w, amount),
            );
        }

        let theta = dot.acos();
        let sin_theta = theta.sin();
        if !sin_theta.is_finite() || sin_theta == 0.0 {
            return Err(TfError::InvalidQuaternion);
        }
        let lhs_weight = ((1.0 - amount) * theta).sin() / sin_theta;
        let rhs_weight = (amount * theta).sin() / sin_theta;
        Self::normalize_result(
            self.x * lhs_weight + rhs.x * rhs_weight,
            self.y * lhs_weight + rhs.y * rhs_weight,
            self.z * lhs_weight + rhs.z * rhs_weight,
            self.w * lhs_weight + rhs.w * rhs_weight,
        )
    }

    fn rotate_vector(self, vector: Vec3) -> TfResult<Vec3> {
        let scale = vector.x.abs().max(vector.y.abs()).max(vector.z.abs());
        if scale == 0.0 {
            return Ok(Vec3::zero());
        }

        let x = vector.x / scale;
        let y = vector.y / scale;
        let z = vector.z / scale;
        let xx = self.x * self.x;
        let yy = self.y * self.y;
        let zz = self.z * self.z;
        let xy = self.x * self.y;
        let xz = self.x * self.z;
        let yz = self.y * self.z;
        let xw = self.x * self.w;
        let yw = self.y * self.w;
        let zw = self.z * self.w;

        let rx = ((1.0 - 2.0 * (yy + zz)) * x) + (2.0 * (xy - zw) * y) + (2.0 * (xz + yw) * z);
        let ry = (2.0 * (xy + zw) * x) + ((1.0 - 2.0 * (xx + zz)) * y) + (2.0 * (yz - xw) * z);
        let rz = (2.0 * (xz - yw) * x) + (2.0 * (yz + xw) * y) + ((1.0 - 2.0 * (xx + yy)) * z);

        Vec3::new(rx * scale, ry * scale, rz * scale)
    }
}

impl<'de> Deserialize<'de> for Quaternion {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct RawQuaternion {
            x: f64,
            y: f64,
            z: f64,
            w: f64,
        }

        let raw = RawQuaternion::deserialize(deserializer)?;
        Self::new(raw.x, raw.y, raw.z, raw.w).map_err(D::Error::custom)
    }
}

/// A rigid transform from a source frame into a target frame.
///
/// `self * rhs` composes transforms as `T_target<-middle * T_middle<-source`.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct Transform3 {
    translation: Vec3,
    rotation: Quaternion,
}

impl Transform3 {
    /// Constructs a transform from its translation and rotation.
    pub const fn new(translation: Vec3, rotation: Quaternion) -> Self {
        Self {
            translation,
            rotation,
        }
    }

    /// Returns the identity transform.
    pub const fn identity() -> Self {
        Self {
            translation: Vec3::zero(),
            rotation: Quaternion::identity(),
        }
    }

    /// Returns the transform's translation.
    pub const fn translation(self) -> Vec3 {
        self.translation
    }

    /// Returns the transform's rotation.
    pub const fn rotation(self) -> Quaternion {
        self.rotation
    }

    /// Returns the inverse transform.
    pub fn inverse(self) -> TfResult<Self> {
        let inverse_rotation = self.rotation.conjugate();
        let inverse_translation =
            inverse_rotation.rotate_vector(self.translation.checked_neg()?)?;
        Ok(Self::new(inverse_translation, inverse_rotation))
    }

    /// Composes this transform with a transform expressed in its source frame.
    pub fn compose(self, rhs: Self) -> TfResult<Self> {
        let translation = self.apply_point(rhs.translation)?;
        let rotation = self.rotation.checked_mul(rhs.rotation)?;
        Ok(Self::new(translation, rotation))
    }

    /// Applies this transform to a point, rotating then translating it.
    pub fn apply_point(self, point: Vec3) -> TfResult<Vec3> {
        self.rotation
            .rotate_vector(point)?
            .checked_add(self.translation)
    }

    /// Applies this transform's rotation to a vector, without translation.
    pub fn apply_vector(self, vector: Vec3) -> TfResult<Vec3> {
        self.rotation.rotate_vector(vector)
    }

    /// Applies this transform to a pose.
    pub fn apply_pose(self, pose: Pose3) -> TfResult<Pose3> {
        Ok(Pose3::new(
            self.apply_point(pose.position)?,
            self.rotation.checked_mul(pose.orientation)?,
        ))
    }

    /// Interpolates translation linearly and rotation with shortest-path SLERP.
    pub fn interpolate(self, rhs: Self, amount: f64) -> TfResult<Self> {
        validate_interpolation_amount(amount)?;
        Ok(Self::new(
            self.translation.interpolate(rhs.translation, amount)?,
            self.rotation.slerp(rhs.rotation, amount)?,
        ))
    }
}

/// A rigid pose containing a position and orientation.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct Pose3 {
    position: Vec3,
    orientation: Quaternion,
}

impl Pose3 {
    /// Constructs a pose from a position and orientation.
    pub const fn new(position: Vec3, orientation: Quaternion) -> Self {
        Self {
            position,
            orientation,
        }
    }

    /// Returns the pose's position.
    pub const fn position(self) -> Vec3 {
        self.position
    }

    /// Returns the pose's orientation.
    pub const fn orientation(self) -> Quaternion {
        self.orientation
    }
}

fn validate_interpolation_amount(amount: f64) -> TfResult<()> {
    if amount.is_finite() && (0.0..=1.0).contains(&amount) {
        Ok(())
    } else {
        Err(TfError::InvalidTransform)
    }
}

fn interpolate_scalar(lhs: f64, rhs: f64, amount: f64) -> f64 {
    if amount == 0.0 {
        lhs
    } else if amount == 1.0 {
        rhs
    } else {
        lhs.mul_add(1.0 - amount, rhs * amount)
    }
}
