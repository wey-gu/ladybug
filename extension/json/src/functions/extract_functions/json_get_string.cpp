#include <string_view>

#include "function/scalar_function.h"
#include "json_extract_functions.h"
#include "json_utils.h"

namespace lbug {
namespace json_extension {

using namespace function;
using namespace common;

static void execFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    result.resetAuxiliaryBuffer();
    const auto& documents = *parameters[0];
    const auto& keys = *parameters[1];
    const auto& documentSelection = *parameterSelVectors[0];
    const auto& keySelection = *parameterSelVectors[1];
    for (auto selectedPos = 0u; selectedPos < resultSelVector->getSelSize(); ++selectedPos) {
        auto resultPos = (*resultSelVector)[selectedPos];
        auto documentPos = documentSelection[documents.state->isFlat() ? 0 : selectedPos];
        auto keyPos = keySelection[keys.state->isFlat() ? 0 : selectedPos];
        result.setNull(resultPos, true);
        if (documents.isNull(documentPos) || keys.isNull(keyPos)) {
            continue;
        }

        auto document = stringToJson(documents.getValue<ku_string_t>(documentPos).getAsString());
        auto root = yyjson_doc_get_root(document.ptr);
        if (!yyjson_is_obj(root)) {
            continue;
        }
        auto requestedKey = keys.getValue<ku_string_t>(keyPos).getAsString();
        yyjson_val* selected = nullptr;
        size_t index = 0, size = 0;
        yyjson_val *key = nullptr, *value = nullptr;
        yyjson_obj_foreach(root, index, size, key, value) {
            if (std::string_view(yyjson_get_str(key), yyjson_get_len(key)) == requestedKey) {
                // Last-key selection is independent of the selected value's type.
                selected = value;
            }
        }
        if (!yyjson_is_str(selected)) {
            continue;
        }
        result.setNull(resultPos, false);
        StringVector::addString(&result, resultPos, yyjson_get_str(selected),
            yyjson_get_len(selected));
    }
}

function_set JsonGetStringFunction::getFunctionSet() {
    function_set result;
    result.push_back(std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING, LogicalTypeID::STRING},
        LogicalTypeID::STRING, execFunc));
    return result;
}

} // namespace json_extension
} // namespace lbug
