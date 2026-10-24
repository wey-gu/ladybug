#include "common/serializer/deserializer.h"

#include "common/serializer/buffered_file.h"
#include "common/string_format.h"

namespace lbug {
namespace common {

template<>
void Deserializer::deserializeValue(std::string& value) {
    uint64_t valueLength = 0;
    deserializeValue(valueLength);
    validateSerializedCount(valueLength, 1);
    value.resize(valueLength);
    reader->read(reinterpret_cast<uint8_t*>(value.data()), valueLength);
}

void Deserializer::validateSerializedCount(uint64_t count, uint64_t minBytesPerElement) const {
    KU_ASSERT(minBytesPerElement > 0);
    const auto remaining = reader->getRemainingBytes();
    if (count > remaining / minBytesPerElement) {
        throw UnexpectedEndOfFileException(stringFormat(
            "Corrupted serialized data: {} elements of at least {} bytes run past the {} bytes "
            "remaining.",
            count, minBytesPerElement, remaining));
    }
}

void Deserializer::validateDebuggingInfo(std::string& value, const std::string& expectedVal) {
#if defined(LBUG_DESER_DEBUG) && (defined(LBUG_RUNTIME_CHECKS) || !defined(NDEBUG))
    deserializeValue<std::string>(value);
    KU_ASSERT(value == expectedVal);
#endif
    // DO NOTHING
    KU_UNUSED(value);
    KU_UNUSED(expectedVal);
}

} // namespace common
} // namespace lbug
