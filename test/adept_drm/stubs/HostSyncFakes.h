#pragma once

#include <HalStorage.h>

#include <string>
#include <utility>
#include <vector>

namespace hostnet {

std::vector<std::pair<std::string, std::string>>& httpQueue();
std::vector<std::string>& postBodies();
void reset();

}  // namespace hostnet
