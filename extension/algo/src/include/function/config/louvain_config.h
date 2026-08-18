#pragma once

#include <cmath>
#include <string>

#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/gds/gds.h"

namespace lbug {
namespace function {

// Generalized-modularity resolution parameter. Values above 1 prefer smaller
// communities; values below 1 prefer larger communities. A default of 1 keeps
// the historical Newman-Girvan modularity objective byte-compatible.
struct Resolution {
    static constexpr const char* NAME = "resolution";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 1.0;

    static void validate(double resolution) {
        if (!std::isfinite(resolution) || resolution <= 0) {
            throw common::BinderException{"resolution must be finite and greater than 0."};
        }
    }
};

// The maximum number of phases in which the graph is clustered and then aggregated.
struct MaxPhases {
    static constexpr const char* NAME = "maxphases";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 20;

    static void validate(int64_t maxPhases) {
        if (maxPhases < 0) {
            throw common::BinderException{"maxphases must be a positive integer."};
        }
    }
};

struct LouvainConfig final : public GDSConfig {
    uint64_t maxIterations = 20;
    uint64_t maxPhases = MaxPhases::DEFAULT_VALUE;

    LouvainConfig() = default;
};

} // namespace function
} // namespace lbug
