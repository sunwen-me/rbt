#include <m3t/link.h>
#include <m3t/optimizer.h>
#include <m3t/soft_constraint.h>

#include <Eigen/Geometry>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string &description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

const std::array<bool, 6> kZRotationOnly{false, false, true,
                                         false, false, false};
const std::array<bool, 6> kFixed{false, false, false,
                                 false, false, false};

float rotation_z(const m3t::Transform3fA &pose) {
  return std::atan2(pose.rotation()(1, 0), pose.rotation()(0, 0));
}

m3t::Transform3fA rotation_pose_z(float angle) {
  auto pose = m3t::Transform3fA::Identity();
  pose.linear() = Eigen::AngleAxisf{angle, Eigen::Vector3f::UnitZ()}
                      .toRotationMatrix();
  return pose;
}

struct OptimizerFixture {
  OptimizerFixture(bool free_base, float child_angle = 0.0f,
                   bool add_soft_constraint = false)
      : root{std::make_shared<m3t::Link>("base", nullptr,
                                         m3t::Transform3fA::Identity(),
                                         m3t::Transform3fA::Identity(),
                                         m3t::Transform3fA::Identity(),
                                         free_base ? kZRotationOnly : kFixed)},
        parent{std::make_shared<m3t::Link>(
            "parent", nullptr, m3t::Transform3fA::Identity(),
            m3t::Transform3fA::Identity(), m3t::Transform3fA::Identity(),
            kZRotationOnly)},
        child{std::make_shared<m3t::Link>(
            "child", nullptr, m3t::Transform3fA::Identity(),
            rotation_pose_z(child_angle),
            m3t::Transform3fA::Identity(), kZRotationOnly)},
        optimizer{"joint prior fixture", root, 1.0f, 1.0f} {
    check(root->AddChildLink(parent), "fixture adds parent link");
    check(parent->AddChildLink(child), "fixture adds child link");
    check(root->SetUp(), "fixture sets up base link");
    check(parent->SetUp(), "fixture sets up parent link");
    check(child->SetUp(), "fixture sets up child link");

    if (add_soft_constraint) {
      soft_constraint = std::make_shared<m3t::SoftConstraint>(
          "joint relation", parent, child, m3t::Transform3fA::Identity(),
          m3t::Transform3fA::Identity(), kZRotationOnly, 0.0f, 0.0f, 0.5f,
          0.01f);
      check(soft_constraint->SetUp(), "fixture sets up soft constraint");
      check(optimizer.AddSoftConstraint(soft_constraint),
            "fixture adds soft constraint");
    }
    check(optimizer.SetUp(), "fixture sets up optimizer without OpenGL");
  }

  int child_index() const { return child->first_jacobian_index(); }

  float base_angle() const { return rotation_z(root->link2world_pose()); }

  float parent_angle() const { return rotation_z(parent->joint2parent_pose()); }

  float child_angle() const { return rotation_z(child->joint2parent_pose()); }

  std::shared_ptr<m3t::Link> root;
  std::shared_ptr<m3t::Link> parent;
  std::shared_ptr<m3t::Link> child;
  std::shared_ptr<m3t::SoftConstraint> soft_constraint;
  m3t::Optimizer optimizer;
};

void prior_moves_only_the_indexed_revolute_joint() {
  OptimizerFixture fixed_base{false};
  check(fixed_base.child_index() == 1,
        "fixed base and parent produce the expected child Jacobian index");
  check(fixed_base.optimizer.SetJointAnglePriors(
            {{fixed_base.child_index(), 0.0f, 0.8f, 0.1f}}),
        "accepts a valid scalar prior");
  check(fixed_base.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes fixed-base fixture");
  check(fixed_base.child_angle() > 0.75f,
        "prior moves the indexed child joint toward its target");
  check(std::abs(fixed_base.parent_angle()) < 1.0e-6f,
        "prior does not move the ancestor joint");
  check(std::abs(fixed_base.base_angle()) < 1.0e-6f,
        "prior does not move a fixed base");

  OptimizerFixture free_base{true};
  check(free_base.child_index() == 2,
        "free base is included before the two revolute child coordinates");
  check(free_base.optimizer.SetJointAnglePriors(
            {{free_base.child_index(), 0.0f, 0.8f, 0.1f}}),
        "accepts a child prior with a free base");
  check(free_base.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes free-base fixture");
  check(free_base.child_angle() > 0.75f,
        "free-base fixture moves the requested child joint");
  check(std::abs(free_base.parent_angle()) < 1.0e-6f,
        "free-base fixture leaves the other revolute joint unchanged");
  check(std::abs(free_base.base_angle()) < 1.0e-6f,
        "free-base fixture leaves the base unchanged");
}

float child_delta_for_standard_deviation(float standard_deviation) {
  OptimizerFixture fixture{false};
  const bool accepted = fixture.optimizer.SetJointAnglePriors(
      {{fixture.child_index(), 0.0f, 1.0f, standard_deviation}});
  check(accepted, "accepts prior used to compare standard deviations");
  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes prior strength fixture");
  return fixture.child_angle();
}

void standard_deviation_changes_factor_strength() {
  const float strong_prior = child_delta_for_standard_deviation(0.1f);
  const float weak_prior = child_delta_for_standard_deviation(1.0f);
  check(strong_prior > weak_prior,
        "smaller standard deviation produces a stronger joint update");
  check(weak_prior > 0.4f,
        "larger standard deviation still contributes a positive update");
}

void zero_residual_prior_adds_hessian_information() {
  OptimizerFixture baseline{false, 0.6f, true};
  check(baseline.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes contradictory soft-constraint baseline");
  const float baseline_change = std::abs(baseline.child_angle() - 0.6f);

  OptimizerFixture with_prior{false, 0.6f, true};
  check(with_prior.optimizer.SetJointAnglePriors(
            {{with_prior.child_index(), 0.6f, 0.6f, 0.1f}}),
        "accepts zero-residual prior");
  check(with_prior.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes with zero-residual prior and contradictory soft constraint");
  const float prior_change = std::abs(with_prior.child_angle() - 0.6f);
  check(baseline_change > 1.0e-3f,
        "soft constraint provides a measurable baseline update");
  check(prior_change < baseline_change,
        "zero-residual prior Hessian resists the contradictory update");
}

void rejects_invalid_priors_without_replacing_existing_values() {
  OptimizerFixture fixture{false};
  const int index = fixture.child_index();
  check(fixture.optimizer.SetJointAnglePriors({{index, 0.0f, 0.25f, 0.1f}}),
        "sets initial prior for atomic validation check");

  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, 0.0f, 0.9f, 0.1f}, {index, 0.0f, 1.0f, 0.1f}}),
        "rejects duplicate Jacobian indices");
  check(!fixture.optimizer.SetJointAnglePriors({{-1, 0.0f, 0.1f, 0.1f}}),
        "rejects negative Jacobian indices");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{2, 0.0f, 0.1f, 0.1f}}),
        "rejects indices outside the fixed-base degrees of freedom");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, std::numeric_limits<float>::quiet_NaN(), 0.1f, 0.1f}}),
        "rejects non-finite current angle");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, 0.0f, std::numeric_limits<float>::infinity(), 0.1f}}),
        "rejects non-finite target angle");
  check(!fixture.optimizer.SetJointAnglePriors({{index, 0.0f, 0.1f, 0.0f}}),
        "rejects zero standard deviation");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, 0.0f, 0.1f,
              std::numeric_limits<float>::infinity()}}),
        "rejects non-finite standard deviation");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, 0.0f, 0.1f,
              std::numeric_limits<float>::denorm_min()}}),
        "rejects unrepresentable positive precision");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, -std::numeric_limits<float>::max(),
              std::numeric_limits<float>::max(), 1.0f}}),
        "rejects unrepresentable weighted residual");
  check(!fixture.optimizer.SetJointAnglePriors(
            {{index, 0.0f, 0.1f, 0.1f}, {0, 0.0f, 0.1f, 0.1f},
             {1, 0.0f, 0.1f, 0.1f}}),
        "rejects more priors than degrees of freedom");

  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes after rejected prior updates");
  check(std::abs(fixture.child_angle() - 0.25f) < 0.02f,
        "rejected updates leave the prior collection unchanged");
}

void priors_require_optimizer_setup() {
  auto root = std::make_shared<m3t::Link>("unconfigured base");
  m3t::Optimizer optimizer{"unconfigured optimizer", root};
  check(!optimizer.SetJointAnglePriors({}),
        "rejects prior assignment before optimizer setup");
}

void setup_again_clears_priors_when_the_tree_shrinks() {
  OptimizerFixture fixture{false};
  const int removed_child_index = fixture.child_index();
  check(fixture.optimizer.SetJointAnglePriors(
            {{removed_child_index, 0.0f, 0.4f, 0.1f}}),
        "sets a prior for the child that will be removed");

  fixture.parent->ClearChildLinks();
  check(fixture.parent->SetUp(), "sets up parent after removing its child");
  check(fixture.optimizer.SetUp(),
        "sets up optimizer again with fewer degrees of freedom");
  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes after setup clears priors from the old tree");
  check(std::abs(fixture.parent_angle()) < 1.0e-6f,
        "re-setup leaves the remaining joint at its no-prior baseline");
}

void clearing_priors_restores_the_no_prior_update() {
  OptimizerFixture fixture{false};
  check(fixture.optimizer.SetJointAnglePriors(
            {{fixture.child_index(), 0.0f, 0.5f, 0.1f}}),
        "sets prior before clear check");
  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes with prior before clear check");
  check(fixture.child_angle() > 0.4f, "prior changed the child angle");

  fixture.parent->ResetJointPoses();
  fixture.child->ResetJointPoses();
  check(fixture.optimizer.CalculateConsistentPoses(),
        "restores fixture's initial joint poses");
  fixture.optimizer.ClearJointAnglePriors();
  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes after clearing priors");
  check(std::abs(fixture.child_angle()) < 1.0e-6f,
        "clearing priors restores the no-prior baseline");

  check(fixture.optimizer.SetJointAnglePriors(
            {{fixture.child_index(), 0.0f, 0.5f, 0.1f}}),
        "sets prior before empty-vector clear check");
  fixture.parent->ResetJointPoses();
  fixture.child->ResetJointPoses();
  check(fixture.optimizer.CalculateConsistentPoses(),
        "restores initial poses before empty-vector clear check");
  check(fixture.optimizer.SetJointAnglePriors({}),
        "accepts an empty prior collection");
  check(fixture.optimizer.CalculateOptimization(0, 0, 0),
        "optimizes after setting an empty prior collection");
  check(std::abs(fixture.child_angle()) < 1.0e-6f,
        "empty prior collection clears the factor");
}

}  // namespace

int main() {
  prior_moves_only_the_indexed_revolute_joint();
  standard_deviation_changes_factor_strength();
  zero_residual_prior_adds_hessian_information();
  rejects_invalid_priors_without_replacing_existing_values();
  priors_require_optimizer_setup();
  setup_again_clears_priors_when_the_tree_shrinks();
  clearing_priors_restores_the_no_prior_update();
  return failures == 0 ? 0 : 1;
}
