# Build the pinned M3T implementation without enabling its camera SDKs,
# tests, examples, or documentation targets.
option(RBT_M3T_ENABLE_CUDA "Enable CUDA support for the M3T texture modality" OFF)

get_filename_component(
    _RBT_M3T_DEFAULT_UPSTREAM_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../target/m3t-upstream"
    ABSOLUTE
)

# Apply audited fixes and the texture support overlay for pinned upstream commit
# f02106186a1e756450226b3b793e309506817c58 to build-directory copies only.
# Requiring each expected block exactly once makes upstream drift fail at
# configure time instead of silently producing a partially patched build.
function(_rbt_m3t_replace_exactly_once contents_var expected replacement source_file)
    set(_rbt_m3t_contents "${${contents_var}}")
    set(_rbt_m3t_search "${_rbt_m3t_contents}")
    string(LENGTH "${expected}" _rbt_m3t_expected_length)
    set(_rbt_m3t_occurrences 0)

    while(TRUE)
        string(FIND "${_rbt_m3t_search}" "${expected}" _rbt_m3t_index)
        if(_rbt_m3t_index EQUAL -1)
            break()
        endif()
        math(EXPR _rbt_m3t_occurrences "${_rbt_m3t_occurrences} + 1")
        math(EXPR _rbt_m3t_next_index "${_rbt_m3t_index} + ${_rbt_m3t_expected_length}")
        string(SUBSTRING "${_rbt_m3t_search}" ${_rbt_m3t_next_index} -1 _rbt_m3t_search)
    endwhile()

    if(NOT _rbt_m3t_occurrences EQUAL 1)
        message(FATAL_ERROR
            "Expected exactly one audited M3T overlay block in ${source_file}; "
            "found ${_rbt_m3t_occurrences}. Check the pinned source before updating this overlay."
        )
    endif()

    string(REPLACE "${expected}" "${replacement}" _rbt_m3t_patched "${_rbt_m3t_contents}")
    set(${contents_var} "${_rbt_m3t_patched}" PARENT_SCOPE)
endfunction()

function(_rbt_m3t_create_patched_source upstream_source build_source)
    get_filename_component(_rbt_m3t_source_name "${upstream_source}" NAME)
    file(READ "${upstream_source}" _rbt_m3t_contents)

    if(_rbt_m3t_source_name STREQUAL "body.cpp")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#include <m3t/body.h>]=]
            [=[#include <m3t/body.h>

#include <stdexcept>]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[uchar Body::get_id(IDType id_type) const {
  if (id_type == IDType::BODY) return body_id_;
  if (id_type == IDType::REGION) return region_id_;
}]=]
            [=[uchar Body::get_id(IDType id_type) const {
  if (id_type == IDType::BODY) return body_id_;
  if (id_type == IDType::REGION) return region_id_;
  throw std::invalid_argument{"Unknown Body IDType"};
}]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "renderer.cpp")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[const std::vector<std::shared_ptr<Body>> &Renderer::referenced_body_ptrs()
    const {
  return {};
}]=]
            [=[const std::vector<std::shared_ptr<Body>> &Renderer::referenced_body_ptrs()
    const {
  static const std::vector<std::shared_ptr<Body>> empty_body_ptrs;
  return empty_body_ptrs;
}]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "renderer_geometry.cpp")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    if (!glfwInit()) {
      std::cerr << "Failed to initialize GLFW" << std::endl;
      return false;
    }]=]
            [=[#if defined(__linux__) && defined(GLFW_PLATFORM) && defined(GLFW_PLATFORM_X11)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (!glfwInit()) {
      std::cerr << "Failed to initialize GLFW" << std::endl;
      return false;
    }]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "texture_modality.h")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#ifdef USE_CUDA
#include <opencv2/cudafeatures2d.hpp>
#endif]=]
            [=[#ifdef USE_CUDA
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudafeatures2d.hpp>
#endif]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#include <opencv2/xfeatures2d.hpp>]=]
            [=[#ifdef RBT_M3T_HAS_OPENCV_XFEATURES2D
#include <opencv2/xfeatures2d.hpp>
#endif]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#ifdef RBT_M3T_HAS_OPENCV_XFEATURES2D
#include <opencv2/xfeatures2d.hpp>
#endif]=]
            [=[#ifdef RBT_M3T_HAS_OPENCV_XFEATURES2D
#include <opencv2/xfeatures2d.hpp>
#endif
#ifdef RBT_M3T_HAS_HEXAGON_ORB
#include <rbt_m3t/dsp_orb.hpp>
#endif]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    ORB = 4,
#ifdef USE_CUDA
    ORB_CUDA = 5
#endif]=]
            [=[    ORB = 4,
#ifdef USE_CUDA
    ORB_CUDA = 5,
#endif
    ORB_DSP = 6]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[   * @param ORB_CUDA uses the ORB descriptor and ORB detector with CUDA support.]=]
            [=[   * @param ORB_CUDA uses the ORB descriptor and ORB detector with CUDA support.
   * @param ORB_DSP uses the ORB descriptor and detector through the Hexagon
   * DSP backend.]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  void DetectAndComputeCorrKeypoints();]=]
            [=[  bool DetectAndComputeCorrKeypoints();]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  cv::Ptr<cv::DescriptorMatcher> descriptor_matcher_;
  std::deque<std::vector<Eigen::Vector3f>> points_keyframes_;]=]
            [=[  cv::Ptr<cv::DescriptorMatcher> descriptor_matcher_;
#ifdef RBT_M3T_HAS_HEXAGON_ORB
  std::unique_ptr<rbt_m3t::dsp::Orb> dsp_orb_;
#endif
  std::deque<std::vector<Eigen::Vector3f>> points_keyframes_;]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "texture_modality.cpp")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#include <m3t/texture_modality.h>]=]
            [=[#include <m3t/texture_modality.h>

#include <exception>]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[void TextureModality::SetUpFeatureDetectorAndMatcher() {
  // Set up feature detector]=]
            [=[void TextureModality::SetUpFeatureDetectorAndMatcher() {
#ifdef RBT_M3T_HAS_HEXAGON_ORB
  dsp_orb_.reset();
#endif
  // Set up feature detector]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  SetUpFeatureDetectorAndMatcher();]=]
            [=[  switch (descriptor_type_) {
    case DescriptorType::BRISK:
    case DescriptorType::ORB:
      break;
    case DescriptorType::SIFT:
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 4)
      break;
#elif defined(RBT_M3T_HAS_OPENCV_XFEATURES2D)
      break;
#else
      std::cerr << "SIFT texture descriptors require OpenCV 4.4+ or xfeatures2d"
                << std::endl;
      return false;
#endif
    case DescriptorType::DAISY:
    case DescriptorType::FREAK:
#ifdef RBT_M3T_HAS_OPENCV_XFEATURES2D
      break;
#else
      std::cerr << "DAISY and FREAK texture descriptors require OpenCV xfeatures2d"
                << std::endl;
      return false;
#endif
#ifdef USE_CUDA
    case DescriptorType::ORB_CUDA:
      if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
        std::cerr << "ORB_CUDA texture descriptor requires an available CUDA device"
                  << std::endl;
        return false;
      }
      break;
#endif
    case DescriptorType::ORB_DSP:
#ifdef RBT_M3T_HAS_HEXAGON_ORB
      break;
#else
      std::cerr << "ORB_DSP texture descriptor requires a build with "
                   "RBT_M3T_ENABLE_HEXAGON_ORB=ON"
                << std::endl;
      return false;
#endif
    default:
      std::cerr << "Unsupported texture descriptor type "
                << static_cast<int>(descriptor_type_) << std::endl;
      return false;
  }

  if (descriptor_type_ == DescriptorType::ORB_DSP) {
#ifdef RBT_M3T_HAS_HEXAGON_ORB
    try {
      SetUpFeatureDetectorAndMatcher();
    } catch (const std::exception &error) {
      std::cerr << "ORB_DSP texture descriptor setup failed: " << error.what()
                << std::endl;
      return false;
    }
#else
    return false;
#endif
  } else {
    SetUpFeatureDetectorAndMatcher();
  }]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    case DescriptorType::DAISY:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ = cv::xfeatures2d::DAISY::create(
          daisy_radius_, daisy_q_radius_, daisy_q_theta_, daisy_q_hist_);
      break;
    case DescriptorType::FREAK:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ = cv::xfeatures2d::FREAK::create(
          freak_orientation_normalized_, freak_scale_normalized_,
          freak_pattern_scale_, freak_n_octaves_);
      break;]=]
            [=[#ifdef RBT_M3T_HAS_OPENCV_XFEATURES2D
    case DescriptorType::DAISY:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ = cv::xfeatures2d::DAISY::create(
          daisy_radius_, daisy_q_radius_, daisy_q_theta_, daisy_q_hist_);
      break;
    case DescriptorType::FREAK:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ = cv::xfeatures2d::FREAK::create(
          freak_orientation_normalized_, freak_scale_normalized_,
          freak_pattern_scale_, freak_n_octaves_);
      break;
#endif]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    case DescriptorType::SIFT:
      // Look in both namespaces for SIFT
      using namespace cv;
      using namespace cv::xfeatures2d;
      feature_detector_ = SIFT::create(sift_n_features_, sift_n_octave_layers_,
                                       sift_contrast_threshold_,
                                       sift_edge_threshold_, sift_sigma_);
      feature_descriptor_ = SIFT::create(
          sift_n_features_, sift_n_octave_layers_, sift_contrast_threshold_,
          sift_edge_threshold_, sift_sigma_);
      break;]=]
            [=[    case DescriptorType::SIFT:
      // OpenCV 4.4 moved SIFT from xfeatures2d to features2d.
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 4)
      feature_detector_ = cv::SIFT::create(
          sift_n_features_, sift_n_octave_layers_, sift_contrast_threshold_,
          sift_edge_threshold_, sift_sigma_);
      feature_descriptor_ = cv::SIFT::create(
          sift_n_features_, sift_n_octave_layers_, sift_contrast_threshold_,
          sift_edge_threshold_, sift_sigma_);
      break;
#elif defined(RBT_M3T_HAS_OPENCV_XFEATURES2D)
      feature_detector_ = cv::xfeatures2d::SIFT::create(
          sift_n_features_, sift_n_octave_layers_, sift_contrast_threshold_,
          sift_edge_threshold_, sift_sigma_);
      feature_descriptor_ = cv::xfeatures2d::SIFT::create(
          sift_n_features_, sift_n_octave_layers_, sift_contrast_threshold_,
          sift_edge_threshold_, sift_sigma_);
      break;
#else
      break;
#endif]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#ifdef USE_CUDA
    case DescriptorType::ORB_CUDA:
      feature_detector_orb_cuda_ = cv::cuda::ORB::create(
          orb_n_features_, orb_scale_factor_, orb_n_levels_);
#endif
    case DescriptorType::ORB:
    default:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      break;]=]
            [=[#ifdef USE_CUDA
    case DescriptorType::ORB_CUDA:
      feature_detector_orb_cuda_ = cv::cuda::ORB::create(
          orb_n_features_, orb_scale_factor_, orb_n_levels_);
      break;
#endif
    case DescriptorType::ORB_DSP:
#ifdef RBT_M3T_HAS_HEXAGON_ORB
      dsp_orb_ = std::make_unique<rbt_m3t::dsp::Orb>(
          rbt_m3t::dsp::OrbConfig{orb_n_features_, orb_scale_factor_,
                                  orb_n_levels_});
#endif
      break;
    case DescriptorType::ORB:
      feature_detector_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      feature_descriptor_ =
          cv::ORB::create(orb_n_features_, orb_scale_factor_, orb_n_levels_);
      break;
    default:
      break;]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    // Set up feature matcher
#ifdef USE_CUDA
  if (descriptor_type_ == DescriptorType::ORB_CUDA)]=]
            [=[    // Set up feature matcher
  if (descriptor_type_ == DescriptorType::ORB_DSP) return;
#ifdef USE_CUDA
  if (descriptor_type_ == DescriptorType::ORB_CUDA)]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  PrecalculatePoseVariables();
  DetectAndComputeCorrKeypoints();
  ComputeKeyframeData();]=]
            [=[  PrecalculatePoseVariables();
  if (!DetectAndComputeCorrKeypoints()) return false;
  ComputeKeyframeData();]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[    DetectAndComputeCorrKeypoints();
    std::vector<std::vector<std::vector<cv::DMatch>>> knn_matches_keyframes;]=]
            [=[    if (!DetectAndComputeCorrKeypoints()) return false;
    std::vector<std::vector<std::vector<cv::DMatch>>> knn_matches_keyframes;]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#ifdef USE_CUDA
    if (descriptor_type_ == DescriptorType::ORB_CUDA) {
      for (const auto &descriptors_keyframe_cuda :
           descriptors_keyframes_cuda_) {
        std::vector<std::vector<cv::DMatch>> knn_matches;
        if (!descriptors_keyframe_cuda.empty() && !descriptors_cuda_.empty())
          descriptor_matcher_cuda_->knnMatch(descriptors_keyframe_cuda,
                                             descriptors_cuda_, knn_matches, 2);
        knn_matches_keyframes.push_back(std::move(knn_matches));
      }
    } else
#endif
    {
      for (const auto &descriptors_keyframe : descriptors_keyframes_) {
        std::vector<std::vector<cv::DMatch>> knn_matches;
        if (!descriptors_keyframe.empty() && !descriptors_.empty())
          descriptor_matcher_->knnMatch(descriptors_keyframe, descriptors_,
                                        knn_matches, 2);
        knn_matches_keyframes.push_back(std::move(knn_matches));
      }
    }]=]
            [=[    if (descriptor_type_ == DescriptorType::ORB_DSP) {
#ifdef RBT_M3T_HAS_HEXAGON_ORB
      try {
        for (const auto &descriptors_keyframe : descriptors_keyframes_) {
          std::vector<std::vector<cv::DMatch>> knn_matches;
          if (!descriptors_keyframe.empty() && !descriptors_.empty())
            knn_matches =
                dsp_orb_->match_2nn(descriptors_keyframe, descriptors_);
          knn_matches_keyframes.push_back(std::move(knn_matches));
        }
      } catch (const std::exception &error) {
        data_points_.clear();
        std::cerr << "ORB_DSP texture matcher failed: " << error.what()
                  << std::endl;
        return false;
      }
#else
      std::cerr << "ORB_DSP texture descriptor requires a build with "
                   "RBT_M3T_ENABLE_HEXAGON_ORB=ON"
                << std::endl;
      return false;
#endif
    } else {
#ifdef USE_CUDA
      if (descriptor_type_ == DescriptorType::ORB_CUDA) {
        for (const auto &descriptors_keyframe_cuda :
             descriptors_keyframes_cuda_) {
          std::vector<std::vector<cv::DMatch>> knn_matches;
          if (!descriptors_keyframe_cuda.empty() && !descriptors_cuda_.empty())
            descriptor_matcher_cuda_->knnMatch(descriptors_keyframe_cuda,
                                               descriptors_cuda_, knn_matches,
                                               2);
          knn_matches_keyframes.push_back(std::move(knn_matches));
        }
      } else
#endif
      {
        for (const auto &descriptors_keyframe : descriptors_keyframes_) {
          std::vector<std::vector<cv::DMatch>> knn_matches;
          if (!descriptors_keyframe.empty() && !descriptors_.empty())
            descriptor_matcher_->knnMatch(descriptors_keyframe, descriptors_,
                                          knn_matches, 2);
          knn_matches_keyframes.push_back(std::move(knn_matches));
        }
      }
    }]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[void TextureModality::DetectAndComputeCorrKeypoints() {
  keypoints_.clear();

  // Compute focused image
  cv::Mat focused_image;
  cv::Rect region_of_interest;
  float scale;
  if (!CalculateScaleAndRegionOfInterest(&region_of_interest, &scale)) return;
  cv::cvtColor(color_camera_ptr_->image(), focused_image, cv::COLOR_BGR2GRAY);
  cv::resize(focused_image(region_of_interest), focused_image, cv::Size(),
             scale, scale);

  // Detect features and compute descriptors
#ifdef USE_CUDA
  if (descriptor_type_ == DescriptorType::ORB_CUDA) {
    cv::cuda::GpuMat focused_image_cuda{focused_image};
    feature_detector_orb_cuda_->detectAndCompute(
        focused_image_cuda, cv::noArray(), keypoints_, descriptors_cuda_);
  } else
#endif
  {
    feature_detector_->detect(focused_image, keypoints_);
    feature_descriptor_->compute(focused_image, keypoints_, descriptors_);
  }

  // Add focus offset to keypoints
  for (auto &corr_keypoint : keypoints_) {
    corr_keypoint.pt.x = region_of_interest.x + corr_keypoint.pt.x / scale;
    corr_keypoint.pt.y = region_of_interest.y + corr_keypoint.pt.y / scale;
  }
}]=]
            [=[bool TextureModality::DetectAndComputeCorrKeypoints() {
  auto clear_features = [this]() {
    keypoints_.clear();
    descriptors_.release();
#ifdef USE_CUDA
    descriptors_cuda_.release();
#endif
  };
  clear_features();

  // Compute focused image
  cv::Mat focused_image;
  cv::Rect region_of_interest;
  float scale;
  if (!CalculateScaleAndRegionOfInterest(&region_of_interest, &scale))
    return true;
  cv::cvtColor(color_camera_ptr_->image(), focused_image, cv::COLOR_BGR2GRAY);
  cv::resize(focused_image(region_of_interest), focused_image, cv::Size(),
             scale, scale);

  // Detect features and compute descriptors
  if (descriptor_type_ == DescriptorType::ORB_DSP) {
#ifdef RBT_M3T_HAS_HEXAGON_ORB
    try {
      dsp_orb_->extract(focused_image, keypoints_, descriptors_);
    } catch (const std::exception &error) {
      clear_features();
      std::cerr << "ORB_DSP texture extraction failed: " << error.what()
                << std::endl;
      return false;
    }
#else
    clear_features();
    std::cerr << "ORB_DSP texture descriptor requires a build with "
                 "RBT_M3T_ENABLE_HEXAGON_ORB=ON"
              << std::endl;
    return false;
#endif
  } else {
#ifdef USE_CUDA
    if (descriptor_type_ == DescriptorType::ORB_CUDA) {
      cv::cuda::GpuMat focused_image_cuda{focused_image};
      feature_detector_orb_cuda_->detectAndCompute(
          focused_image_cuda, cv::noArray(), keypoints_, descriptors_cuda_);
    } else
#endif
    {
      feature_detector_->detect(focused_image, keypoints_);
      feature_descriptor_->compute(focused_image, keypoints_, descriptors_);
    }
  }

  // Add focus offset to keypoints
  for (auto &corr_keypoint : keypoints_) {
    corr_keypoint.pt.x = region_of_interest.x + corr_keypoint.pt.x / scale;
    corr_keypoint.pt.y = region_of_interest.y + corr_keypoint.pt.y / scale;
  }
  return true;
}]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "optimizer.cpp")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[#include <m3t/optimizer.h>]=]
            [=[#include <m3t/optimizer.h>

#include <cmath>
#include <limits>]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[bool Optimizer::SetUp() {
  set_up_ = false;]=]
            [=[bool Optimizer::SetUp() {
  set_up_ = false;
  joint_angle_priors_.clear();]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  AddResidualsAndConstraintJacobians(&b, &a);
  a.diagonal().topRows(degrees_of_freedom_) += tikhonov_vector_;]=]
            [=[  AddResidualsAndConstraintJacobians(&b, &a);
  for (const auto &prior : joint_angle_priors_) {
    const double standard_deviation = prior.standard_deviation;
    const float precision = static_cast<float>(
        1.0 / (standard_deviation * standard_deviation));
    const float weighted_residual = static_cast<float>(
        (static_cast<double>(prior.target_angle) - prior.current_angle) /
        (standard_deviation * standard_deviation));
    b[prior.jacobian_index] += weighted_residual;
    a(prior.jacobian_index, prior.jacobian_index) += precision;
  }
  a.diagonal().topRows(degrees_of_freedom_) += tikhonov_vector_;]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[void Optimizer::ClearSoftConstraints() {
  set_up_ = false;
  soft_constraint_ptrs_.clear();
}]=]
            [=[void Optimizer::ClearSoftConstraints() {
  set_up_ = false;
  soft_constraint_ptrs_.clear();
}

bool Optimizer::SetJointAnglePriors(
    const std::vector<JointAnglePrior> &joint_angle_priors) {
  if (!set_up_) {
    std::cerr << "Set up optimizer " << name_ << " before assigning joint angle priors"
              << std::endl;
    return false;
  }
  if (joint_angle_priors.size() >
      static_cast<std::size_t>(degrees_of_freedom_)) {
    return false;
  }

  std::vector<bool> assigned(degrees_of_freedom_, false);
  const double maximum_float = std::numeric_limits<float>::max();
  for (const auto &prior : joint_angle_priors) {
    if (prior.jacobian_index < 0 ||
        prior.jacobian_index >= degrees_of_freedom_ ||
        assigned[prior.jacobian_index]) {
      return false;
    }
    if (!std::isfinite(prior.current_angle) ||
        !std::isfinite(prior.target_angle) ||
        !std::isfinite(prior.standard_deviation) ||
        prior.standard_deviation <= 0.0f) {
      return false;
    }

    const double standard_deviation = prior.standard_deviation;
    const double variance = standard_deviation * standard_deviation;
    const double precision = 1.0 / variance;
    const double weighted_residual =
        (static_cast<double>(prior.target_angle) - prior.current_angle) /
        variance;
    const double smallest_positive_float =
        std::numeric_limits<float>::denorm_min() > 0.0f
            ? std::numeric_limits<float>::denorm_min()
            : std::numeric_limits<float>::min();
    if (!std::isfinite(precision) || precision <= 0.0 ||
        precision < smallest_positive_float || precision > maximum_float ||
        !std::isfinite(weighted_residual) ||
        (weighted_residual != 0.0 &&
         std::abs(weighted_residual) < smallest_positive_float) ||
        std::abs(weighted_residual) > maximum_float) {
      return false;
    }
    const float float_precision = static_cast<float>(precision);
    const float float_weighted_residual =
        static_cast<float>(weighted_residual);
    if (!std::isfinite(float_precision) || float_precision <= 0.0f ||
        !std::isfinite(float_weighted_residual) ||
        (weighted_residual != 0.0 && float_weighted_residual == 0.0f)) {
      return false;
    }
    assigned[prior.jacobian_index] = true;
  }

  joint_angle_priors_ = joint_angle_priors;
  return true;
}

void Optimizer::ClearJointAnglePriors() { joint_angle_priors_.clear(); }]=]
            "${upstream_source}"
        )
    elseif(_rbt_m3t_source_name STREQUAL "optimizer.h")
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[namespace m3t {]=]
            [=[namespace m3t {

struct JointAnglePrior {
  int jacobian_index;
  float current_angle;
  float target_angle;
  float standard_deviation;
};]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  void ClearSoftConstraints();]=]
            [=[  void ClearSoftConstraints();
  bool SetJointAnglePriors(
      const std::vector<JointAnglePrior> &joint_angle_priors);
  void ClearJointAnglePriors();]=]
            "${upstream_source}"
        )
        _rbt_m3t_replace_exactly_once(
            _rbt_m3t_contents
            [=[  std::vector<std::shared_ptr<SoftConstraint>> soft_constraint_ptrs_{};
  float tikhonov_parameter_rotation_ = 1000.0f;]=]
            [=[  std::vector<std::shared_ptr<SoftConstraint>> soft_constraint_ptrs_{};
  std::vector<JointAnglePrior> joint_angle_priors_{};
  float tikhonov_parameter_rotation_ = 1000.0f;]=]
            "${upstream_source}"
        )
    else()
        message(FATAL_ERROR "No audited M3T overlay exists for ${upstream_source}")
    endif()

    get_filename_component(_rbt_m3t_build_source_dir "${build_source}" DIRECTORY)
    file(MAKE_DIRECTORY "${_rbt_m3t_build_source_dir}")
    if(EXISTS "${build_source}")
        file(READ "${build_source}" _rbt_m3t_existing_contents)
        if(_rbt_m3t_existing_contents STREQUAL _rbt_m3t_contents)
            return()
        endif()
    endif()
    file(WRITE "${build_source}" "${_rbt_m3t_contents}")
endfunction()

function(rbt_add_m3t_upstream)
    if(TARGET rbt_m3t_upstream)
        message(FATAL_ERROR "rbt_m3t_upstream is already defined")
    endif()

    if(DEFINED RBT_M3T_UPSTREAM_ROOT AND NOT RBT_M3T_UPSTREAM_ROOT STREQUAL "")
        set(_rbt_m3t_upstream_root "${RBT_M3T_UPSTREAM_ROOT}")
    else()
        set(_rbt_m3t_upstream_root "${_RBT_M3T_DEFAULT_UPSTREAM_ROOT}")
    endif()
    set(_rbt_m3t_source_dir "${_rbt_m3t_upstream_root}/M3T")
    if(NOT EXISTS "${_rbt_m3t_source_dir}/include/m3t/tracker.h")
        message(FATAL_ERROR
            "M3T source is missing at ${_rbt_m3t_source_dir}; run "
            "perception/m3t/scripts/bootstrap_native.sh first"
        )
    endif()

    find_package(OpenMP REQUIRED COMPONENTS CXX)
    find_package(Eigen3 3.3.2 REQUIRED NO_MODULE)
    find_package(GLEW REQUIRED)
    set(OpenGL_GL_PREFERENCE LEGACY)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 3.1.2 REQUIRED CONFIG)
    set(_rbt_m3t_opencv_components
        core imgproc imgcodecs calib3d highgui features2d)
    if(RBT_M3T_ENABLE_CUDA)
        list(APPEND _rbt_m3t_opencv_components cudafeatures2d)
    endif()
    find_package(OpenCV 4.3.0 REQUIRED COMPONENTS ${_rbt_m3t_opencv_components}
        OPTIONAL_COMPONENTS xfeatures2d)

    if(TARGET glfw)
        set(_rbt_m3t_glfw_target glfw)
    elseif(TARGET glfw3::glfw)
        set(_rbt_m3t_glfw_target glfw3::glfw)
    else()
        message(FATAL_ERROR "glfw3 was found, but it did not define a GLFW target")
    endif()

    set(_rbt_m3t_sources
        common.cpp
        body.cpp
        renderer_geometry.cpp
        renderer.cpp
        normal_renderer.cpp
        silhouette_renderer.cpp
        basic_depth_renderer.cpp
        model.cpp
        region_model.cpp
        depth_model.cpp
        camera.cpp
        loader_camera.cpp
        viewer.cpp
        image_viewer.cpp
        normal_viewer.cpp
        color_histograms.cpp
        modality.cpp
        region_modality.cpp
        depth_modality.cpp
        texture_modality.cpp
        link.cpp
        constraint.cpp
        soft_constraint.cpp
        optimizer.cpp
        detector.cpp
        static_detector.cpp
        manual_detector.cpp
        refiner.cpp
        publisher.cpp
        subscriber.cpp
        tracker.cpp
    )
    set(_rbt_m3t_source_files)
    set(_rbt_m3t_overlay_root "${CMAKE_CURRENT_BINARY_DIR}/m3t-upstream-overlay/M3T")
    set(_rbt_m3t_overlay_dir "${_rbt_m3t_overlay_root}/src")
    foreach(_rbt_m3t_source IN LISTS _rbt_m3t_sources)
        set(_rbt_m3t_source_file "${_rbt_m3t_source_dir}/src/${_rbt_m3t_source}")
        if(NOT EXISTS "${_rbt_m3t_source_file}")
            message(FATAL_ERROR "Required upstream M3T source is missing: ${_rbt_m3t_source_file}")
        endif()
        if(_rbt_m3t_source STREQUAL "body.cpp"
                OR _rbt_m3t_source STREQUAL "renderer.cpp"
                OR _rbt_m3t_source STREQUAL "renderer_geometry.cpp"
                OR _rbt_m3t_source STREQUAL "texture_modality.cpp"
                OR _rbt_m3t_source STREQUAL "optimizer.cpp")
            set(_rbt_m3t_overlay_source "${_rbt_m3t_overlay_dir}/${_rbt_m3t_source}")
            _rbt_m3t_create_patched_source("${_rbt_m3t_source_file}" "${_rbt_m3t_overlay_source}")
            list(APPEND _rbt_m3t_source_files "${_rbt_m3t_overlay_source}")
        else()
            list(APPEND _rbt_m3t_source_files "${_rbt_m3t_source_file}")
        endif()
    endforeach()

    set(_rbt_m3t_texture_header "${_rbt_m3t_source_dir}/include/m3t/texture_modality.h")
    set(_rbt_m3t_overlay_texture_header
        "${_rbt_m3t_overlay_root}/include/m3t/texture_modality.h")
    if(NOT EXISTS "${_rbt_m3t_texture_header}")
        message(FATAL_ERROR
            "Required upstream M3T header is missing: ${_rbt_m3t_texture_header}")
    endif()
    _rbt_m3t_create_patched_source(
        "${_rbt_m3t_texture_header}" "${_rbt_m3t_overlay_texture_header}")

    set(_rbt_m3t_optimizer_header "${_rbt_m3t_source_dir}/include/m3t/optimizer.h")
    set(_rbt_m3t_overlay_optimizer_header
        "${_rbt_m3t_overlay_root}/include/m3t/optimizer.h")
    if(NOT EXISTS "${_rbt_m3t_optimizer_header}")
        message(FATAL_ERROR
            "Required upstream M3T header is missing: ${_rbt_m3t_optimizer_header}")
    endif()
    _rbt_m3t_create_patched_source(
        "${_rbt_m3t_optimizer_header}" "${_rbt_m3t_overlay_optimizer_header}")

    add_library(rbt_m3t_upstream STATIC ${_rbt_m3t_source_files})
    target_compile_features(rbt_m3t_upstream PUBLIC cxx_std_17)
    target_include_directories(rbt_m3t_upstream SYSTEM PUBLIC
        "${_rbt_m3t_overlay_root}/include"
        "${_rbt_m3t_source_dir}/include"
        "${_rbt_m3t_source_dir}/third_party"
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(rbt_m3t_upstream PUBLIC
        Eigen3::Eigen
        ${OpenCV_LIBS}
        ${_rbt_m3t_glfw_target}
        GLEW::GLEW
        OpenGL::GL
        OpenMP::OpenMP_CXX
    )

    if(OpenCV_xfeatures2d_FOUND)
        target_compile_definitions(rbt_m3t_upstream PUBLIC
            RBT_M3T_HAS_OPENCV_XFEATURES2D=1)
    endif()
    if(RBT_M3T_ENABLE_CUDA)
        target_compile_definitions(rbt_m3t_upstream PUBLIC USE_CUDA=1)
    endif()
    if(RBT_M3T_ENABLE_HEXAGON_ORB)
        if(NOT TARGET rbt_m3t_dsp_backend)
            message(FATAL_ERROR
                "RBT_M3T_ENABLE_HEXAGON_ORB requires the "
                "rbt_m3t_dsp_backend target"
            )
        endif()
        target_link_libraries(rbt_m3t_upstream PUBLIC rbt_m3t_dsp_backend)
        target_compile_definitions(rbt_m3t_upstream PUBLIC
            RBT_M3T_HAS_HEXAGON_ORB=1)
    endif()
endfunction()
