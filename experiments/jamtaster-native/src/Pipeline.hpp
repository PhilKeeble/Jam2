#pragma once

#include "PipelineTypes.hpp"

#include <functional>
#include <string>

namespace jamtaster::native {

using PipelineProgress = std::function<void(int, const std::string&)>;
PipelineResult runPipeline(const PipelineOptions& options, PipelineProgress progress = {});

} // namespace jamtaster::native
