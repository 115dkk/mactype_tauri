#pragma once

#include "process_observation.h"
#include "probe_common.h"
#include "render_probe.h"

#include <string>

namespace mactype::service_probe::internal {

std::string BuildSnapshotJson(const ProbeOptions& options,
                              const ObservedModules& observed,
                              const FontSubstitutionObservation& font_substitution,
                              const std::string& observed_at);

}  // namespace mactype::service_probe::internal
