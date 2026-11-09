#pragma once
#include "app/options.hpp"
#include <functional>
namespace image_pipeline {
void run_pipeline(const Options& options, const std::function<bool()>& interrupted);
}
