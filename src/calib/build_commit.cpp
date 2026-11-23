// The commit the estimator was built from, written into every calibration file.
#include "calib/estimator.hpp"
#include "calib_git_commit.hpp"  // generated in the build tree by cmake/git_commit.cmake

namespace calib {

std::string build_commit() { return LOBSIM_GIT_COMMIT; }

}  // namespace calib
