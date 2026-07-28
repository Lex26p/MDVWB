#pragma once

#include "mdv_buses_config.h"
#include "mdvwb_service_sync.h"

namespace mdvwb {

// Strictly reads existing /etc/default/mdvwb and canonical mdvwb-N files and
// builds the first buses.json without executing shell contents. Incomplete,
// malformed or ambiguous legacy sources are rejected instead of being skipped.
BusesConfig MigrateLegacyDefaults(
    const ServiceSyncPaths& paths,
    CommandRunner& commandRunner);

}  // namespace mdvwb
