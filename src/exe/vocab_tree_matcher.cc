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

  OptionManager options;
  options.AddDatabaseOptions();
  options.AddMatchOptions();
  options.AddVocabTreeMatchOptions();
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

  VocabTreeFeatureMatcher feature_matcher(
      options.vocab_tree_match_options->Options(), match_options,
      *options.database_path);

  if (!match_options.use_gpu || no_opengl) {
    feature_matcher.Start();
    feature_matcher.Wait();
  } else {
    RunThreadWithOpenGLContext(app.get(), &feature_matcher);
  }

  return EXIT_SUCCESS;
}
