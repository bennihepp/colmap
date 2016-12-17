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

#include "mvs/fusion.h"

#include <Eigen/Geometry>

#include "util/logging.h"
#include "util/misc.h"

namespace colmap {
namespace mvs {
namespace internal {

float Median(std::vector<float>* elems) {
  CHECK(!elems->empty());
  const size_t mid_idx = elems->size() / 2;
  std::nth_element(elems->begin(), elems->begin() + mid_idx, elems->end());
  if (elems->size() % 2 == 0) {
    const float mid_element1 = (*elems)[mid_idx];
    const float mid_element2 =
        *std::max_element(elems->begin(), elems->begin() + mid_idx);
    return (mid_element1 + mid_element2) / 2.0f;
  } else {
    return (*elems)[mid_idx];
  }
}

}  // namespace internal

void StereoFusion::Options::Print() const {
#define PrintOption(option) std::cout << #option ": " << option << std::endl
  PrintHeading2("StereoFusion::Options");
  PrintOption(min_num_pixels);
  PrintOption(max_num_pixels);
  PrintOption(max_traversal_depth);
  PrintOption(max_reproj_error);
  PrintOption(max_depth_error);
  PrintOption(max_normal_error);
#undef PrintOption
}

void StereoFusion::Options::Check() const {
  CHECK_GE(min_num_pixels, 0);
  CHECK_LE(min_num_pixels, max_num_pixels);
  CHECK_GT(max_traversal_depth, 0);
  CHECK_GE(max_reproj_error, 0);
  CHECK_GE(max_depth_error, 0);
  CHECK_GE(max_normal_error, 0);
}

StereoFusion::StereoFusion(const Options& options,
                           const std::string& workspace_path,
                           const std::string& workspace_format,
                           const std::string& input_type)
    : options_(options),
      workspace_path_(workspace_path),
      workspace_format_(workspace_format),
      input_type_(input_type),
      max_squared_reproj_error_(options_.max_reproj_error *
                                options_.max_reproj_error),
      min_cos_normal_error_(std::cos(DegToRad(options_.max_normal_error))) {}

const std::vector<FusedPoint>& StereoFusion::GetFusedPoints() const {
  return fused_points_;
}

void StereoFusion::Run() {
  fused_points_.clear();

  options_.Print();
  std::cout << std::endl;

  Read();
  if (IsStopped()) {
    GetTimer().PrintMinutes();
    return;
  }

  Prepare();
  if (IsStopped()) {
    GetTimer().PrintMinutes();
    return;
  }

  const size_t min_num_pixels = static_cast<size_t>(options_.min_num_pixels);

  for (size_t image_id = 0; image_id < image_data_.size(); ++image_id) {
    if (IsStopped()) {
      break;
    }

    Timer timer;
    timer.Start();

    std::cout << StringPrintf("Fusing image [%d/%d]", image_id + 1,
                              image_data_.size())
              << std::flush;

    const auto& image_data = image_data_[image_id];

    const DepthMap& depth_map = model_.depth_maps[image_id];
    const NormalMap& normal_map = model_.normal_maps[image_id];
    DepthMap fused_depth_map;
    NormalMap fused_normal_map;
    if (options_.output_fused_depth_maps) {
      // Write filtered depth and normal maps to disk
      fused_depth_map = DepthMap(depth_map.GetWidth(), depth_map.GetHeight(), depth_map.GetDepthMin(), depth_map.GetDepthMax());
      fused_normal_map = NormalMap(normal_map.GetWidth(), normal_map.GetHeight());
      fused_depth_map.Fill(0);
      fused_normal_map.Fill(0);
      // Set all pixels that have already been fused from another image
      for (size_t row = 0; row < image_data.image->GetHeight(); ++row) {
        for (size_t col = 0; col < image_data.image->GetWidth(); ++col) {
          if (image_data.visited_mask.Get(row, col, 0)) {
            fused_depth_map.Set(row, col, 0, depth_map.Get(row, col));
            for (size_t channel = 0; channel < normal_map.GetDepth(); ++channel) {
              fused_normal_map.Set(row, col, channel, normal_map.Get(row, col, channel));
            }
          }
        }
      }
    }

    for (size_t row = 0; row < image_data.image->GetHeight(); ++row) {
      for (size_t col = 0; col < image_data.image->GetWidth(); ++col) {
        if (image_data.visited_mask.Get(row, col, 0)) {
          continue;
        }

        fused_points_x_.clear();
        fused_points_y_.clear();
        fused_points_z_.clear();
        fused_normal_sum_.setZero();
        fused_color_sum_ = BitmapColor<uint32_t>(0, 0, 0);

        FusePoint(image_id, row, col, 0);

        const size_t num_pixels = fused_points_x_.size();
        if (num_pixels >= min_num_pixels) {
          FusedPoint fused_point;

          fused_point.x = internal::Median(&fused_points_x_);
          fused_point.y = internal::Median(&fused_points_y_);
          fused_point.z = internal::Median(&fused_points_z_);

          const Eigen::Vector3d mean_normal = fused_normal_sum_.normalized();
          fused_point.nx = static_cast<float>(mean_normal(0));
          fused_point.ny = static_cast<float>(mean_normal(1));
          fused_point.nz = static_cast<float>(mean_normal(2));

          fused_point.r = TruncateCast<double, uint8_t>(
              std::round(static_cast<double>(fused_color_sum_.r) / num_pixels));
          fused_point.g = TruncateCast<double, uint8_t>(
              std::round(static_cast<double>(fused_color_sum_.g) / num_pixels));
          fused_point.b = TruncateCast<double, uint8_t>(
              std::round(static_cast<double>(fused_color_sum_.b) / num_pixels));

          fused_points_.push_back(fused_point);

          if (options_.output_fused_depth_maps) {
            fused_depth_map.Set(row, col, 0, depth_map.Get(row, col));
            for (size_t channel = 0; channel < normal_map.GetDepth(); ++channel) {
              fused_normal_map.Set(row, col, channel, normal_map.Get(row, col, channel));
            }
          }
        }
      }
    }

    std::cout << StringPrintf(" in %.3fs", timer.ElapsedSeconds()) << std::endl;

    if (options_.output_fused_depth_maps) {
      // Write filtered depth and normal maps to disk
      const std::string image_name = model_.GetImageName(image_id);
      const std::string file_name =
      StringPrintf("%s.%s.fused.bin", image_name.c_str(), input_type_.c_str());
      fused_depth_map.Write(JoinPaths(workspace_path_, "stereo/depth_maps", file_name));
      fused_normal_map.Write(JoinPaths(workspace_path_, "stereo/normal_maps", file_name));
    }
  }

  fused_points_.shrink_to_fit();

  std::cout << "Number of fused points: " << fused_points_.size() << std::endl;
  GetTimer().PrintMinutes();
}

void StereoFusion::Read() {
  std::cout << "Reading model..." << std::endl;
  model_.Read(workspace_path_, workspace_format_);
  used_image_mask_.resize(model_.images.size(), false);

  std::cout << "Reading configuration..." << std::endl;

  std::ifstream file(JoinPaths(workspace_path_, "stereo/fusion.cfg"));
  CHECK(file.is_open());

  std::string line;
  while (std::getline(file, line)) {
    if (IsStopped()) {
      return;
    }

    StringTrim(&line);

    if (line.empty() || line[0] == '#') {
      continue;
    }

    Timer timer;
    timer.Start();

    const std::string image_name = line;
    const int image_id = model_.GetImageId(image_name);

    std::cout << StringPrintf("Loading image %s", image_name.c_str())
              << std::flush;

    used_image_mask_.at(image_id) = true;

    auto& image = model_.images.at(image_id);
    auto& depth_map = model_.depth_maps.at(image_id);
    auto& normal_map = model_.normal_maps.at(image_id);

    const std::string file_name =
        StringPrintf("%s.%s.bin", image_name.c_str(), input_type_.c_str());
    depth_map.Read(JoinPaths(workspace_path_, "stereo/depth_maps", file_name));
    normal_map.Read(
        JoinPaths(workspace_path_, "stereo/normal_maps", file_name));
    ReadBinaryBlob<int>(
        JoinPaths(workspace_path_, "stereo/consistency_graphs", file_name),
        &model_.consistency_graph.at(image_id));

    const bool kReadImageAsRGB = true;
    image.Read(kReadImageAsRGB);

    CHECK_EQ(depth_map.GetWidth(), normal_map.GetWidth());
    CHECK_EQ(depth_map.GetHeight(), normal_map.GetHeight());

    if (depth_map.GetWidth() != image.GetWidth() ||
        depth_map.GetHeight() != image.GetHeight()) {
      image.Rescale(
          depth_map.GetWidth() / static_cast<float>(image.GetWidth()),
          depth_map.GetHeight() / static_cast<float>(image.GetHeight()));
      CHECK_EQ(image.GetWidth(), depth_map.GetWidth());
      CHECK_EQ(image.GetHeight(), depth_map.GetHeight());
    }

    std::cout << StringPrintf(" in %.3fs", timer.ElapsedSeconds()) << std::endl;
  }
}

void StereoFusion::Prepare() {
  CHECK_EQ(model_.images.size(), used_image_mask_.size());
  CHECK_EQ(model_.images.size(), model_.depth_maps.size());
  CHECK_EQ(model_.images.size(), model_.normal_maps.size());
  CHECK_EQ(model_.images.size(), model_.consistency_graph.size());

  for (size_t i = 0; i < model_.images.size(); ++i) {
    CHECK_EQ(model_.images[i].GetWidth(), model_.depth_maps[i].GetWidth());
    CHECK_EQ(model_.images[i].GetHeight(), model_.depth_maps[i].GetHeight());
    CHECK_EQ(model_.images[i].GetWidth(), model_.normal_maps[i].GetWidth());
    CHECK_EQ(model_.images[i].GetHeight(), model_.normal_maps[i].GetHeight());
  }

  std::cout << "Preparing fusion..." << std::endl;
  consistency_graph_ =
      ConsistencyGraph(model_.images, &model_.consistency_graph);

  image_data_.resize(model_.images.size());
  for (size_t image_id = 0; image_id < model_.images.size(); ++image_id) {
    if (IsStopped()) {
      return;
    }

    if (!used_image_mask_[image_id]) {
      continue;
    }

    auto& image_data = image_data_[image_id];

    image_data.used = true;

    image_data.image = &model_.images[image_id];
    image_data.depth_map = &model_.depth_maps[image_id];
    image_data.normal_map = &model_.normal_maps[image_id];

    image_data.visited_mask = Mat<char>(image_data.image->GetWidth(),
                                        image_data.image->GetHeight(), 1);
    image_data.visited_mask.Fill(false);

    image_data.P =
        Eigen::Map<const Eigen::Matrix<float, 3, 4, Eigen::RowMajor>>(
            image_data.image->GetP());
    image_data.inv_P =
        Eigen::Map<const Eigen::Matrix<float, 3, 4, Eigen::RowMajor>>(
            image_data.image->GetInvP());
    image_data.inv_R =
        Eigen::Map<const Eigen::Matrix<float, 3, 3, Eigen::RowMajor>>(
            image_data.image->GetR())
            .transpose();
  }
}

void StereoFusion::FusePoint(const int image_id, const int row, const int col,
                             const size_t traversal_depth) {
  auto& image_data = image_data_.at(image_id);

  if (!image_data.used) {
    return;
  }

  if (col < 0 || row < 0 ||
      col >= static_cast<int>(image_data.depth_map->GetWidth()) ||
      row >= static_cast<int>(image_data.depth_map->GetHeight())) {
    return;
  }

  const float depth = image_data.depth_map->Get(row, col);

  // Pixels with negative depth are filtered.
  if (depth <= 0.0f) {
    return;
  }

  // Check if pixel already fused.
  if (image_data.visited_mask.Get(row, col, 0)) {
    return;
  }

  // If the traversal depth is greater than zero, the initial reference pixel
  // has already been added and we need to check for consistency.
  if (traversal_depth > 0) {
    // Project reference point into current view.
    const Eigen::Vector3f proj = image_data.P * fused_ref_point_;

    // Depth error of reference depth with current depth.
    const float depth_error = std::abs((proj(2) - depth) / depth);
    if (depth_error > options_.max_depth_error) {
      return;
    }

    // Reprojection error reference point in the current view.
    const float col_diff = proj(0) / proj(2) - col;
    const float row_diff = proj(1) / proj(2) - row;
    const float squared_reproj_error =
        col_diff * col_diff + row_diff * row_diff;
    if (squared_reproj_error > max_squared_reproj_error_) {
      return;
    }
  }

  // Determine normal direction in global reference frame.
  const Eigen::Vector3f normal =
      image_data.inv_R *
      Eigen::Vector3f(image_data.normal_map->Get(row, col, 0),
                      image_data.normal_map->Get(row, col, 1),
                      image_data.normal_map->Get(row, col, 2));

  // Check for consistent normal direction with reference normal.
  if (traversal_depth > 0) {
    const float cos_normal_error = fused_ref_normal_.dot(normal);
    if (cos_normal_error < min_cos_normal_error_) {
      return;
    }
  }

  // Determine 3D location of current depth value.
  const Eigen::Vector3f xyz =
      image_data.inv_P * Eigen::Vector4f(col * depth, row * depth, depth, 1.0f);

  // Read the color of the pixel.
  BitmapColor<uint8_t> color;
  image_data.image->GetBitmap().GetPixel(col, row, &color);

  // Set the current pixel as visited.
  image_data.visited_mask.Set(row, col, 0, true);

  // Accumulate statistics for fused point.
  fused_points_x_.push_back(xyz(0));
  fused_points_y_.push_back(xyz(1));
  fused_points_z_.push_back(xyz(2));
  fused_normal_sum_ += normal.cast<double>();
  fused_color_sum_.r += color.r;
  fused_color_sum_.g += color.g;
  fused_color_sum_.b += color.b;

  // Remember the first pixel as the reference.
  if (traversal_depth == 0) {
    fused_ref_point_ = Eigen::Vector4f(xyz(0), xyz(1), xyz(2), 1.0f);
    fused_ref_normal_ = normal;
  }

  const int next_traversal_depth = traversal_depth + 1;

  // Do not traverse the graph infinitely in one branch and limit the maximum
  // number of pixels fused in one point to avoid stack overflow.
  if (next_traversal_depth >= options_.max_traversal_depth ||
      fused_points_x_.size() >= static_cast<size_t>(options_.max_num_pixels)) {
    return;
  }

  // Traverse the consistency graph by projecting point into other views.
  int num_consistent = 0;
  const int* consistent_image_ids = nullptr;
  consistency_graph_.GetConsistentImageIds(image_id, row, col, &num_consistent,
                                           &consistent_image_ids);
  for (int i = 0; i < num_consistent; ++i) {
    const int next_image_id = consistent_image_ids[i];
    const auto& next_image_data = image_data_.at(next_image_id);
    const Eigen::Vector3f next_proj = next_image_data.P * xyz.homogeneous();
    const int next_col =
        static_cast<int>(std::round(next_proj(0) / next_proj(2)));
    const int next_row =
        static_cast<int>(std::round(next_proj(1) / next_proj(2)));
    FusePoint(next_image_id, next_row, next_col, next_traversal_depth);
  }
}

StereoFusion::ConsistencyGraph::ConsistencyGraph()
    : consistency_graph_(nullptr) {}

StereoFusion::ConsistencyGraph::ConsistencyGraph(
    const std::vector<Image>& images,
    const std::vector<std::vector<int>>* consistency_graph)
    : consistency_graph_(consistency_graph) {
  CHECK_EQ(images.size(), consistency_graph->size());
  const int kNoConsistentImageIds = -1;
  image_maps_.resize(images.size());
  for (size_t image_id = 0; image_id < images.size(); ++image_id) {
    const auto& image = images[image_id];
    const auto& consistency_list = (*consistency_graph)[image_id];
    auto& image_map = image_maps_[image_id];
    image_map.resize(image.GetHeight(), image.GetWidth());
    image_map.setConstant(kNoConsistentImageIds);
    for (size_t i = 0; i < consistency_list.size();) {
      const int col = consistency_list[i++];
      const int row = consistency_list[i++];
      image_map(row, col) = i;
      const int num_consistent = consistency_list[i++];
      i += num_consistent;
    }
  }
}

void StereoFusion::ConsistencyGraph::GetConsistentImageIds(
    const int image_id, const int row, const int col, int* num_consistent,
    const int** consistent_image_ids) const {
  const int kNoConsistentImageIds = -1;
  const int index = image_maps_.at(image_id)(row, col);
  if (index == kNoConsistentImageIds) {
    *num_consistent = 0;
    *consistent_image_ids = nullptr;
  } else {
    const auto& consistency_list = consistency_graph_->at(image_id);
    *num_consistent = consistency_list.at(index);
    *consistent_image_ids = &consistency_list[index + 1];
  }
}

void WritePlyText(const std::string& path,
                  const std::vector<FusedPoint>& points) {
  std::ofstream file(path);
  CHECK(file.is_open()) << path;

  file << "ply" << std::endl;
  file << "format ascii 1.0" << std::endl;
  file << "element vertex " << points.size() << std::endl;
  file << "property float x" << std::endl;
  file << "property float y" << std::endl;
  file << "property float z" << std::endl;
  file << "property float nx" << std::endl;
  file << "property float ny" << std::endl;
  file << "property float nz" << std::endl;
  file << "property uchar red" << std::endl;
  file << "property uchar green" << std::endl;
  file << "property uchar blue" << std::endl;
  file << "end_header" << std::endl;

  for (const auto& point : points) {
    file << point.x << " " << point.y << " " << point.z << " " << point.nx
         << " " << point.ny << " " << point.nz << " "
         << static_cast<int>(point.r) << " " << static_cast<int>(point.g) << " "
         << static_cast<int>(point.b) << std::endl;
  }

  file.close();
}

void WritePlyBinary(const std::string& path,
                    const std::vector<FusedPoint>& points) {
  std::fstream text_file(path, std::ios_base::out);
  CHECK(text_file.is_open()) << path;

  text_file << "ply" << std::endl;
  if (IsBigEndian()) {
    text_file << "format binary_big_endian 1.0" << std::endl;
  } else {
    text_file << "format binary_little_endian 1.0" << std::endl;
  }
  text_file << "element vertex " << points.size() << std::endl;
  text_file << "property float x" << std::endl;
  text_file << "property float y" << std::endl;
  text_file << "property float z" << std::endl;
  text_file << "property float nx" << std::endl;
  text_file << "property float ny" << std::endl;
  text_file << "property float nz" << std::endl;
  text_file << "property uchar red" << std::endl;
  text_file << "property uchar green" << std::endl;
  text_file << "property uchar blue" << std::endl;
  text_file << "end_header" << std::endl;
  text_file.close();

  std::fstream binary_file(
      path, std::ios_base::out | std::ios_base::binary | std::ios_base::app);
  CHECK(binary_file.is_open()) << path;

  float xyz_normal_buffer[6];
  uint8_t rgb_buffer[3];
  for (const auto& point : points) {
    xyz_normal_buffer[0] = point.x;
    xyz_normal_buffer[1] = point.y;
    xyz_normal_buffer[2] = point.z;
    xyz_normal_buffer[3] = point.nx;
    xyz_normal_buffer[4] = point.ny;
    xyz_normal_buffer[5] = point.nz;
    binary_file.write(reinterpret_cast<const char*>(xyz_normal_buffer),
                      6 * sizeof(float));
    rgb_buffer[0] = point.r;
    rgb_buffer[1] = point.g;
    rgb_buffer[2] = point.b;
    binary_file.write(reinterpret_cast<const char*>(rgb_buffer),
                      3 * sizeof(uint8_t));
  }
  binary_file.close();
}

}  // namespace mvs
}  // namespace colmap
