#pragma once

#include "common/exception/runtime.h"
#include "common/exception/storage.h"

namespace lbug::storage {

// Source causes carry type authority independently of exception wording.
class InvalidWalRecordTypeException final : public common::RuntimeException {
public:
    InvalidWalRecordTypeException()
        : RuntimeException("Invalid persisted WAL record type.") {}
};

class WalChecksumMismatchException final : public common::StorageException {
public:
    explicit WalChecksumMismatchException(const std::string& reason) : StorageException(reason) {}
};

// Only strict dry replay constructs this cause, after the WAL header has been
// checked against the existing database. A generic deserializer/query exception
// does not give a caller permission to isolate a WAL.
class MalformedWalException final : public common::RuntimeException {
public:
    explicit MalformedWalException(const std::string& reason)
        : RuntimeException("LBUG_MALFORMED_WAL_INPUT: " + reason) {}
};

} // namespace lbug::storage
