#pragma once

#include <cstdint>
#include <optional>

#include "common/exception/runtime.h"
#include "common/exception/storage.h"

namespace lbug::storage {

// Describes validated current bytes; it is never a claim about lost acknowledgments.
enum class WalPrefixStopReason : uint8_t {
    InvalidRecordType = 1,
    ChecksumMismatch = 2,
    UnexpectedEof = 3,
    InvalidTransaction = 5,
    UndecodableRecord = 6,
};

struct VerifiedWalPrefix {
    uint64_t commitEnd = 0;
    uint64_t transactionCount = 0;
    uint64_t failedRecordStart = 0;
    uint64_t walSize = 0;
    WalPrefixStopReason reason = WalPrefixStopReason::InvalidRecordType;
    bool zeroSuffix = false;
    std::string prefixSha256;
};

class InvalidWalTransactionException final : public common::RuntimeException {
public:
    explicit InvalidWalTransactionException(const std::string& reason) : RuntimeException(reason) {}
};

// Source causes carry type authority independently of exception wording.
class InvalidWalRecordTypeException final : public common::RuntimeException {
public:
    InvalidWalRecordTypeException()
        : RuntimeException("Corrupted wal file. Read out invalid WAL record type.") {}
};

// A record with a valid type whose body violates a decoder invariant (for
// example an unsupported persisted physical type). Record checksums are only
// verified after the body is decoded, so damaged bytes surface here first.
class UndecodableWalRecordException final : public common::RuntimeException {
public:
    explicit UndecodableWalRecordException(const std::string& reason)
        : RuntimeException("Corrupted wal file. Could not decode WAL record: " + reason) {}
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
    explicit MalformedWalException(const std::string& reason,
        std::optional<VerifiedWalPrefix> prefix = {})
        : RuntimeException("LBUG_MALFORMED_WAL_INPUT: " + reason), prefix{prefix} {}

    const std::optional<VerifiedWalPrefix> prefix;
};

} // namespace lbug::storage
