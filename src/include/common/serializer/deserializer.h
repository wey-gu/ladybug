#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/assert.h"
#include "common/serializer/reader.h"

namespace lbug {
namespace common {

class LBUG_API Deserializer {
public:
    explicit Deserializer(std::unique_ptr<Reader> reader) : reader(std::move(reader)) {}

    bool finished() const { return reader->finished(); }

    template<typename T>
        requires std::is_trivially_destructible_v<T> || std::is_same_v<std::string, T>
    void deserializeValue(T& value) {
        reader->read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
    }

    void read(uint8_t* data, uint64_t size) const { reader->read(data, size); }

    Reader* getReader() const { return reader.get(); }

    void validateDebuggingInfo(std::string& value, const std::string& expectedVal);

    // Persisted lengths and counts are untrusted input. Reject `count` elements
    // of at least `minBytesPerElement` serialized bytes each when they cannot fit
    // in the bytes the reader can still supply. Call before sizing an allocation
    // from a persisted value. The rejection is the UnexpectedEndOfFileException
    // that reading those bytes would raise, so a damaged huge length is
    // classified exactly like a torn record instead of escaping as
    // std::bad_alloc or std::length_error.
    //
    // Allocation bound: a leaf byte buffer (string, index aux buffer) is sized
    // exactly from a validated length, so it is at most the remaining bytes.
    // Containers below never reserve from a persisted count; they grow only as
    // elements are actually decoded, so their storage is at most twice
    // sizeof(T) per decoded element, and each decoded element consumed at least
    // minSerializedSize<T>() bytes.
    void validateSerializedCount(uint64_t count, uint64_t minBytesPerElement) const;

    // Smallest number of bytes one serialized element of T occupies. A type
    // whose serialize side guarantees more declares MIN_SERIALIZED_SIZE.
    template<typename T>
    static constexpr uint64_t minSerializedSize() {
        if constexpr (requires { T::MIN_SERIALIZED_SIZE; }) {
            return T::MIN_SERIALIZED_SIZE;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return sizeof(uint64_t);
        } else if constexpr (requires(Deserializer& deser) { T::deserialize(deser); }) {
            return 1;
        } else {
            return sizeof(T);
        }
    }

    template<typename T>
    void deserializeOptionalValue(std::unique_ptr<T>& value) {
        bool isNull = false;
        deserializeValue(isNull);
        if (!isNull) {
            value = T::deserialize(*this);
        }
    }

    template<typename T1, typename T2>
    void deserializeMap(std::map<T1, T2>& values) {
        uint64_t mapSize = 0;
        deserializeValue<uint64_t>(mapSize);
        validateSerializedCount(mapSize, minSerializedSize<T1>() + minSerializedSize<T2>());
        for (auto i = 0u; i < mapSize; i++) {
            T1 key;
            deserializeValue<T1>(key);
            auto val = T2::deserialize(*this);
            values.emplace(key, std::move(val));
        }
    }

    template<typename T1, typename T2>
    void deserializeUnorderedMap(std::unordered_map<T1, T2>& values) {
        uint64_t mapSize = 0;
        deserializeValue<uint64_t>(mapSize);
        validateSerializedCount(mapSize, minSerializedSize<T1>() + minSerializedSize<T2>());
        for (auto i = 0u; i < mapSize; i++) {
            T1 key;
            deserializeValue<T1>(key);
            T2 val;
            deserializeValue(val);
            values.emplace(key, std::move(val));
        }
    }

    template<typename T1, typename T2>
    void deserializeUnorderedMapOfPtrs(std::unordered_map<T1, std::unique_ptr<T2>>& values) {
        uint64_t mapSize = 0;
        deserializeValue<uint64_t>(mapSize);
        validateSerializedCount(mapSize, minSerializedSize<T1>() + minSerializedSize<T2>());
        for (auto i = 0u; i < mapSize; i++) {
            T1 key;
            deserializeValue<T1>(key);
            auto val = T2::deserialize(*this);
            values.emplace(key, std::move(val));
        }
    }

    template<typename T>
    void deserializeVector(std::vector<T>& values) {
        uint64_t vectorSize = 0;
        deserializeValue(vectorSize);
        validateSerializedCount(vectorSize, minSerializedSize<T>());
        values.clear();
        for (auto i = 0u; i < vectorSize; i++) {
            if constexpr (requires(Deserializer& deser) { T::deserialize(deser); }) {
                values.push_back(T::deserialize(*this));
            } else {
                deserializeValue(values.emplace_back());
            }
        }
    }

    template<typename T, uint64_t ARRAY_SIZE>
    void deserializeArray(std::array<T, ARRAY_SIZE>& values) {
        KU_ASSERT(values.size() == ARRAY_SIZE);
        for (auto& value : values) {
            if constexpr (requires(Deserializer& deser) { T::deserialize(deser); }) {
                value = T::deserialize(*this);
            } else {
                deserializeValue(value);
            }
        }
    }

    template<typename T>
    void deserializeVectorOfPtrs(std::vector<std::unique_ptr<T>>& values) {
        uint64_t vectorSize = 0;
        deserializeValue(vectorSize);
        validateSerializedCount(vectorSize, minSerializedSize<T>());
        values.clear();
        for (auto i = 0u; i < vectorSize; i++) {
            values.push_back(T::deserialize(*this));
        }
    }

    template<typename T>
    void deserializeVectorOfPtrs(std::vector<std::unique_ptr<T>>& values,
        std::function<std::unique_ptr<T>(Deserializer&)> deserializeFunc) {
        uint64_t vectorSize = 0;
        deserializeValue(vectorSize);
        validateSerializedCount(vectorSize, minSerializedSize<T>());
        values.clear();
        for (auto i = 0u; i < vectorSize; i++) {
            values.push_back(deserializeFunc(*this));
        }
    }

    template<typename T>
    void deserializeUnorderedSet(std::unordered_set<T>& values) {
        uint64_t setSize = 0;
        deserializeValue(setSize);
        validateSerializedCount(setSize, minSerializedSize<T>());
        for (auto i = 0u; i < setSize; i++) {
            T value;
            deserializeValue<T>(value);
            values.insert(value);
        }
    }

private:
    std::unique_ptr<Reader> reader;
};

template<>
void Deserializer::deserializeValue(std::string& value);

} // namespace common
} // namespace lbug
