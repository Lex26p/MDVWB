#pragma once

#include "mdv_buses_config.h"
#include "mdvwb_service_sync.h"

namespace mdvwb {

// Reads existing /etc/default/mdvwb and /etc/default/mdvwb-N files and builds
// the first buses.json without executing shell contents.
BusesConfig MigrateLegacyDefaults(
    const ServiceSyncPaths& paths,
    CommandRunner& commandRunner);

} // namespace mdvwb
