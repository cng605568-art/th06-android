#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/Log.hpp"

namespace touhou {

// Records game features the data asked for that this port does not implement yet, so
// gaps are visible (log + HUD) instead of silently ignored.
class UnportedFeatures {
  public:
    void hit(const char* feature) {
        auto [it, inserted] = counts_.try_emplace(feature, 0);
        if (inserted) log::warn("Not ported yet: {}", feature);
        ++it->second;
    }
    const std::map<std::string, std::uint64_t>& counts() const { return counts_; }

  private:
    std::map<std::string, std::uint64_t> counts_;
};

}  // namespace touhou
