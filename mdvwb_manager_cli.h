#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mdvwb {

int RunManagerCommand(
    const std::vector<std::string>& arguments,
    std::ostream& output,
    std::ostream& errors);

}  // namespace mdvwb
