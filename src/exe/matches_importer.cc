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

#include <QApplication>

#include "base/feature_matching.h"
#include "util/logging.h"
#include "util/option_manager.h"

using namespace colmap;

int main(int argc, char** argv) {
  InitializeGlog(argv);

#ifdef CUDA_ENABLED
  bool no_opengl = true;
#else
  bool no_opengl = false;
#endif

  std::string match_list_path;
  std::string match_type = "pairs";

  OptionManager options;
  options.AddDatabaseOptions();
  options.AddMatchOptions();
  options.AddRequiredOption("match_list_path", &match_list_path);
  options.AddDefaultOption("match_type", match_type, &match_type,
                           "{'pairs', 'raw', 'inliers'}");
  options.AddDefaultOption("no_opengl", no_opengl, &no_opengl);

  if (!options.Parse(argc, argv)) {
    return EXIT_FAILURE;
  }

  if (options.ParseHelp(argc, argv)) {
    return EXIT_SUCCESS;
  }

  std::unique_ptr<QApplication> app;
  SiftMatchOptions match_options = options.match_options->Options();
  if (match_options.use_gpu) {
    if (no_opengl) {
      if (match_options.gpu_index < 0) {
        match_options.gpu_index = 0;
      }
    } else {
      app.reset(new QApplication(argc, argv));
    }
  }

  std::unique_ptr<Thread> feature_matcher;
  if (match_type == "pairs") {
    ImagePairsFeatureMatcher::Options matcher_options;
    matcher_options.match_list_path = match_list_path;
    feature_matcher.reset(new ImagePairsFeatureMatcher(
        matcher_options, match_options, *options.database_path));
  } else if (match_type == "raw" || match_type == "inliers") {
    FeaturePairsFeatureMatcher::Options matcher_options;
    matcher_options.match_list_path = match_list_path;
    matcher_options.verify_matches = match_type == "raw";
    feature_matcher.reset(new FeaturePairsFeatureMatcher(
        matcher_options, match_options, *options.database_path));
  } else {
    std::cerr << "ERROR: Invalid `match_type`";
    return EXIT_FAILURE;
  }

  if (!match_options.use_gpu || no_opengl) {
    feature_matcher->Start();
    feature_matcher->Wait();
  } else {
    RunThreadWithOpenGLContext(app.get(), feature_matcher.get());
  }

  return EXIT_SUCCESS;
}
