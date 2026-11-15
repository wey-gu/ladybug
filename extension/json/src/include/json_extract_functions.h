#pragma once

#include "function/function.h"

namespace lbug {
namespace json_extension {

struct JsonExtractFunction {
    static constexpr const char* name = "json_extract";

    static function::function_set getFunctionSet();
};

struct JsonGetStringFunction {
    // Last exact top-level key, decoded STRING or NULL for absent/non-string
    // values. Malformed documents remain errors; json_extract is unchanged.
    static constexpr const char* name = "json_get_string";

    static function::function_set getFunctionSet();
};

} // namespace json_extension
} // namespace lbug
