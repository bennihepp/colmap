// COLMAP - Structure-from-Motion and Multi-View Stereo.
// Copyright (C) 2016  Johannes L. Schoenberger <jsch at inf.ethz.ch>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "mvs/patch_match.h"

#include <cmath>
#include <vector>
#include <future>
#include <boost/algorithm/string/split.hpp>
#include <boost/lexical_cast.hpp>

#include "mvs/patch_match_cuda.h"
#include "util/misc.h"

#define PrintOption(option) std::cout << #option ": " << option << std::endl

namespace colmap {
namespace mvs {
namespace {

// Read patch match problems from workspace.
void ReadPatchMatchProblems(const PatchMatch::Options& options,
                            const std::string& workspace_path,
                            const std::string& workspace_format,
                            const int max_image_size, Model* model,
                            std::vector<PatchMatch::Problem>* problems) {
  std::cout << "Reading model..." << std::endl;
  model->Read(workspace_path, workspace_format);

  std::cout << "Reading configuration..." << std::endl;

  std::ifstream file(JoinPaths(workspace_path, "stereo/patch-match.cfg"));
  CHECK(file.is_open());

  std::set<int> used_image_ids;
  std::vector<std::map<int, int>> shared_points;

  std::string line;
  std::string ref_image_name;
  while (std::getline(file, line)) {
    StringTrim(&line);

    if (line.empty() || line[0] == '#') {
      continue;
    }

    if (ref_image_name.empty()) {
      ref_image_name = line;
      continue;
    }

    PatchMatch::Problem problem;

    problem.images = &model->images;
    problem.depth_maps = &model->depth_maps;
    problem.normal_maps = &model->normal_maps;

    problem.ref_image_id = model->GetImageId(ref_image_name);
    used_image_ids.insert(problem.ref_image_id);

    const std::vector<std::string> src_image_names =
        CSVToVector<std::string>(line);

    if (src_image_names.size() == 1 && src_image_names[0] == "__all__") {
      // Use all images as source images.
      problem.src_image_ids.clear();
      problem.src_image_ids.reserve(model->images.size() - 1);
      for (size_t image_id = 0; image_id < model->images.size(); ++image_id) {
        if (static_cast<int>(image_id) != problem.ref_image_id) {
          problem.src_image_ids.push_back(image_id);
          used_image_ids.insert(image_id);
        }
      }
    } else if (src_image_names.size() == 2 &&
               src_image_names[0] == "__auto__") {
      // Use maximum number of overlapping images as source images. Overlapping
      // will be sorted based on the number of shared points to the reference
      // image and the top ranked images are selected.

      if (shared_points.empty()) {
        shared_points = model->ComputeSharedPoints();
      }

      const size_t max_num_src_images =
          boost::lexical_cast<int>(src_image_names[1]);

      const auto& overlapping_images = shared_points.at(problem.ref_image_id);

      if (max_num_src_images >= overlapping_images.size()) {
        problem.src_image_ids.reserve(overlapping_images.size());
        for (const auto& image : overlapping_images) {
          problem.src_image_ids.push_back(image.first);
          used_image_ids.insert(image.first);
        }
      } else {
        std::vector<std::pair<int, int>> src_images;
        src_images.reserve(overlapping_images.size());
        for (const auto& image : overlapping_images) {
          src_images.emplace_back(image.first, image.second);
        }

        std::partial_sort(
            src_images.begin(), src_images.begin() + max_num_src_images,
            src_images.end(), [](const std::pair<int, int> image1,
                                 const std::pair<int, int> image2) {
              return image1.second > image2.second;
            });

        problem.src_image_ids.reserve(max_num_src_images);
        for (size_t i = 0; i < max_num_src_images; ++i) {
          problem.src_image_ids.push_back(src_images[i].first);
          used_image_ids.insert(src_images[i].first);
        }
      }
    } else {
      problem.src_image_ids.reserve(src_image_names.size());
      for (const auto& src_image_name : src_image_names) {
        problem.src_image_ids.push_back(model->GetImageId(src_image_name));
      }
    }

    CHECK(!problem.src_image_ids.empty()) << "Need at least one source image";

    problems->push_back(problem);

    ref_image_name.clear();
  }

  std::cout << "Reading inputs..." << std::endl;
  for (const auto image_id : used_image_ids) {
    auto& image = model->images.at(image_id);
    const bool kReadImageAsRGB = false;
    image.Read(kReadImageAsRGB);
    if (options.geom_consistency) {
      const std::string file_name =
          model->GetImageName(image_id) + ".photometric.bin";
      auto& depth_map = model->depth_maps.at(image_id);
      depth_map.Read(JoinPaths(workspace_path, "stereo/depth_maps", file_name));
      auto& normal_map = model->normal_maps.at(image_id);
      normal_map.Read(
          JoinPaths(workspace_path, "stereo/normal_maps", file_name));
    }
  }

  std::cout << "Resampling model..." << std::endl;
  ThreadPool resample_thread_pool;
  for (size_t image_id = 0; image_id < model->images.size(); ++image_id) {
    resample_thread_pool.AddTask([&, image_id]() {
      if (max_image_size > 0) {
        model->images.at(image_id).Downsize(max_image_size, max_image_size);
        model->depth_maps.at(image_id).Downsize(max_image_size, max_image_size);
        model->normal_maps.at(image_id).Downsize(max_image_size,
                                                 max_image_size);
      }
    });
  }
  resample_thread_pool.Wait();
}

}  // namespace

PatchMatch::PatchMatch(const Options& options, const Problem& problem)
    : options_(options), problem_(problem) {}

PatchMatch::~PatchMatch() {}

void PatchMatch::Options::Print() const {
  PrintHeading2("PatchMatch::Options");
  PrintOption(depth_min);
  PrintOption(depth_max);
  PrintOption(window_radius);
  PrintOption(sigma_spatial);
  PrintOption(sigma_color);
  PrintOption(num_samples);
  PrintOption(ncc_sigma);
  PrintOption(min_triangulation_angle);
  PrintOption(incident_angle_sigma);
  PrintOption(num_iterations);
  PrintOption(geom_consistency);
  PrintOption(geom_consistency_regularizer);
  PrintOption(geom_consistency_max_cost);
  PrintOption(filter);
  PrintOption(filter_min_ncc);
  PrintOption(filter_min_triangulation_angle);
  PrintOption(filter_min_num_consistent);
  PrintOption(filter_geom_consistency_max_cost);
}

void PatchMatch::Problem::Print() const {
  PrintHeading2("PatchMatch::Problem");

  PrintOption(ref_image_id);

  std::cout << "src_image_ids: ";
  if (!src_image_ids.empty()) {
    for (size_t i = 0; i < src_image_ids.size() - 1; ++i) {
      std::cout << src_image_ids[i] << " ";
    }
    std::cout << src_image_ids.back() << std::endl;
  } else {
    std::cout << std::endl;
  }
}

void PatchMatch::Check() const {
  CHECK_NOTNULL(problem_.images);
  if (options_.geom_consistency) {
    CHECK_NOTNULL(problem_.depth_maps);
    CHECK_NOTNULL(problem_.normal_maps);
    CHECK_EQ(problem_.depth_maps->size(), problem_.images->size());
    CHECK_EQ(problem_.normal_maps->size(), problem_.images->size());
  }

  CHECK_GT(problem_.src_image_ids.size(), 0);

  // Check that there are no duplicate images and that the reference image
  // is not defined as a source image.
  std::set<int> unique_image_ids(problem_.src_image_ids.begin(),
                                 problem_.src_image_ids.end());
  unique_image_ids.insert(problem_.ref_image_id);
  CHECK_EQ(problem_.src_image_ids.size() + 1, unique_image_ids.size());

  // Check that input data is well-formed.
  for (const int image_id : unique_image_ids) {
    CHECK_GE(image_id, 0) << image_id;
    CHECK_LT(image_id, problem_.images->size()) << image_id;

    const Image& image = problem_.images->at(image_id);
    CHECK_GT(image.GetWidth(), 0) << image_id;
    CHECK_GT(image.GetHeight(), 0) << image_id;
    CHECK_EQ(image.GetChannels(), 1) << image_id;

    // Make sure, the calibration matrix only contains fx, fy, cx, cy.
    CHECK_LT(std::abs(image.GetK()[1] - 0.0f), 1e-6f) << image_id;
    CHECK_LT(std::abs(image.GetK()[3] - 0.0f), 1e-6f) << image_id;
    CHECK_LT(std::abs(image.GetK()[6] - 0.0f), 1e-6f) << image_id;
    CHECK_LT(std::abs(image.GetK()[7] - 0.0f), 1e-6f) << image_id;
    CHECK_LT(std::abs(image.GetK()[8] - 1.0f), 1e-6f) << image_id;

    if (options_.geom_consistency) {
      CHECK_LT(image_id, problem_.depth_maps->size()) << image_id;
      const DepthMap& depth_map = problem_.depth_maps->at(image_id);
      CHECK_EQ(image.GetWidth(), depth_map.GetWidth()) << image_id;
      CHECK_EQ(image.GetHeight(), depth_map.GetHeight()) << image_id;
    }
  }

  if (options_.geom_consistency) {
    const Image& ref_image = problem_.images->at(problem_.ref_image_id);
    const NormalMap& ref_normal_map =
        problem_.normal_maps->at(problem_.ref_image_id);
    CHECK_EQ(ref_image.GetWidth(), ref_normal_map.GetWidth());
    CHECK_EQ(ref_image.GetHeight(), ref_normal_map.GetHeight());
  }

  CHECK_LT(options_.depth_min, options_.depth_max);
  CHECK_GT(options_.depth_min, 0.0f);
  CHECK_LE(options_.window_radius, kMaxWindowRadius);
  CHECK_GT(options_.sigma_spatial, 0.0f);
  CHECK_GT(options_.sigma_color, 0.0f);
  CHECK_GT(options_.window_radius, 0);
  CHECK_GT(options_.num_samples, 0);
  CHECK_GT(options_.ncc_sigma, 0.0f);
  CHECK_GE(options_.min_triangulation_angle, 0.0f);
  CHECK_LT(options_.min_triangulation_angle, 180.0f);
  CHECK_GT(options_.incident_angle_sigma, 0.0f);
  CHECK_GT(options_.num_iterations, 0);
  CHECK_GE(options_.geom_consistency_regularizer, 0.0f);
  CHECK_GE(options_.geom_consistency_max_cost, 0.0f);
  CHECK_GE(options_.filter_min_ncc, -1.0f);
  CHECK_LE(options_.filter_min_ncc, 1.0f);
  CHECK_GE(options_.filter_min_triangulation_angle, 0.0f);
  CHECK_LE(options_.filter_min_triangulation_angle, 180.0f);
  CHECK_GE(options_.filter_min_num_consistent, 0);
  CHECK_GE(options_.filter_geom_consistency_max_cost, 0.0f);
}

void PatchMatch::Run() {
  PrintHeading2("PatchMatch::Run");

  Check();

  patch_match_cuda_.reset(new PatchMatchCuda(options_, problem_));
  patch_match_cuda_->Run();
}

DepthMap PatchMatch::GetDepthMap() const {
  return patch_match_cuda_->GetDepthMap();
}

NormalMap PatchMatch::GetNormalMap() const {
  return patch_match_cuda_->GetNormalMap();
}

Mat<float> PatchMatch::GetSelProbMap() const {
  return patch_match_cuda_->GetSelProbMap();
}

std::vector<int> PatchMatch::GetConsistentImageIds() const {
  return patch_match_cuda_->GetConsistentImageIds();
}

PatchMatchController::PatchMatchController(const PatchMatch::Options& options,
                                           const std::string& workspace_path,
                                           const std::string& workspace_format,
                                           const int max_image_size)
    : options_(options),
      workspace_path_(workspace_path),
      workspace_format_(workspace_format),
      max_image_size_(max_image_size) {}

void PatchMatchController::Run() {
  Model model;
  std::vector<PatchMatch::Problem> problems;
  ReadPatchMatchProblems(options_, workspace_path_, workspace_format_,
                         max_image_size_, &model, &problems);

  const auto depth_ranges = model.ComputeDepthRanges();

  const std::string output_suffix =
      options_.geom_consistency ? "geometric" : "photometric";

  std::vector<int> gpu_indices;
  if (options_.multi_gpu_indices.empty()) {
    gpu_indices.push_back(options_.gpu_index);
  }
  else {
    std::vector<std::string> strs;
    boost::split(strs, options_.multi_gpu_indices, boost::is_any_of(" ,"));
    for (const std::string& str : strs) {
      gpu_indices.push_back(boost::lexical_cast<int>(str));
      std::cout << "Using GPU " << gpu_indices.back() << std::endl;
    }
  }

  std::vector<std::future<bool>> results;
  ThreadPool thread_pool(gpu_indices.size());
  for (size_t i = 0; i < problems.size(); ++i) {
    const auto loop_lambda = [&](const size_t index) {
      const auto& problem = problems[index];

      const std::string image_name = model.GetImageName(problem.ref_image_id);
      const std::string file_name =
          StringPrintf("%s.%s.bin", image_name.c_str(), output_suffix.c_str());
      const std::string depth_map_path =
          JoinPaths(workspace_path_, "stereo/depth_maps", file_name);
      const std::string normal_map_path =
          JoinPaths(workspace_path_, "stereo/normal_maps", file_name);
      const std::string consistency_graph_path =
          JoinPaths(workspace_path_, "stereo/consistency_graphs", file_name);

      if (boost::filesystem::exists(depth_map_path) &&
          boost::filesystem::exists(normal_map_path) &&
          boost::filesystem::exists(consistency_graph_path)) {
        return true;
      }

      PrintHeading1(
          StringPrintf("Processing view %d / %d", index + 1, problems.size()));

      problem.Print();

      PatchMatch::Options patch_match_options = options_;
      patch_match_options.depth_min = depth_ranges.at(problem.ref_image_id).first;
      patch_match_options.depth_max =
          depth_ranges.at(problem.ref_image_id).second;
      patch_match_options.Print();

      int current_thread_index = thread_pool.GetCurrentIndex();
      int current_gpu_index = gpu_indices[current_thread_index];
      PatchMatch::Options current_options = patch_match_options;
      current_options.gpu_index = current_gpu_index;
      PatchMatch patch_match(current_options, problem);
      patch_match.Run();

      std::cout << std::endl << "Writing output: " << file_name << std::endl;

      patch_match.GetDepthMap().Write(depth_map_path);
      patch_match.GetNormalMap().Write(normal_map_path);
      WriteBinaryBlob(consistency_graph_path,
                      patch_match.GetConsistentImageIds());
      return true;
    };
    std::future<bool> result = thread_pool.AddTask(loop_lambda, i);
    results.push_back(std::move(result));
  }

  std::size_t results_done = 0;
  while (results_done < problems.size()) {
    if (IsStopped()) {
      break;
    }
    thread_pool.Wait(std::chrono::seconds(1));
    for (auto it = results.begin(); it != results.end();) {
      if (it->wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
        ++results_done;
        it = results.erase(it);
      }
      else {
        ++it;
      }
    }
  }

//  for (size_t i = 0; i < problems.size(); ++i) {
//    if (IsStopped()) {
//      break;
//    }
//
//    const auto& problem = problems[i];
//
//    const std::string image_name = model.GetImageName(problem.ref_image_id);
//    const std::string file_name =
//        StringPrintf("%s.%s.bin", image_name.c_str(), output_suffix.c_str());
//    const std::string depth_map_path =
//        JoinPaths(workspace_path_, "stereo/depth_maps", file_name);
//    const std::string normal_map_path =
//        JoinPaths(workspace_path_, "stereo/normal_maps", file_name);
//    const std::string consistency_graph_path =
//        JoinPaths(workspace_path_, "stereo/consistency_graphs", file_name);
//
//    if (boost::filesystem::exists(depth_map_path) &&
//        boost::filesystem::exists(normal_map_path) &&
//        boost::filesystem::exists(consistency_graph_path)) {
//      continue;
//    }
//
//    PrintHeading1(
//        StringPrintf("Processing view %d / %d", i + 1, problems.size()));
//
//    problem.Print();
//
//    PatchMatch::Options patch_match_options = options_;
//    patch_match_options.depth_min = depth_ranges.at(problem.ref_image_id).first;
//    patch_match_options.depth_max =
//        depth_ranges.at(problem.ref_image_id).second;
//    patch_match_options.Print();
//
//    PatchMatch patch_match(patch_match_options, problem);
//    patch_match.Run();
//
//    std::cout << std::endl << "Writing output: " << file_name << std::endl;
//
//    patch_match.GetDepthMap().Write(depth_map_path);
//    patch_match.GetNormalMap().Write(normal_map_path);
//    WriteBinaryBlob(consistency_graph_path,
//                    patch_match.GetConsistentImageIds());
//  }

  GetTimer().PrintMinutes();
}

}  // namespace mvs
}  // namespace colmap
