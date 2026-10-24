#include "storage/wal/checksum_reader.h"

#include <cstring>

#include "common/checksum.h"
#include "storage/wal/malformed_wal.h"
#include "common/exception/storage.h"
#include "common/serializer/buffered_file.h"
#include "common/serializer/deserializer.h"
#include <bit>

namespace lbug::storage {
static constexpr uint64_t INITIAL_BUFFER_SIZE = common::LBUG_PAGE_SIZE;

ChecksumReader::ChecksumReader(common::FileInfo& fileInfo, MemoryManager& memoryManager,
    std::string_view checksumMismatchMessage)
    : deserializer(std::make_unique<common::BufferedFileReader>(fileInfo)),
      entryBuffer(memoryManager.allocateBuffer(false, INITIAL_BUFFER_SIZE)),
      checksumMismatchMessage(checksumMismatchMessage) {}

// Growing the entry buffer must carry the bytes already accumulated for this
// record. Dropping them persisted every record above 4 KiB with a stale head
// that its own checksum covered (https://github.com/nowledge-co/mem/issues/8009,
// upstream LadybugDB 0b4f086ad1).
static void resizeBufferIfNeeded(std::unique_ptr<MemoryBuffer>& entryBuffer, uint64_t currentSize,
    uint64_t requestedSize) {
    const auto currentBufferSize = entryBuffer->getBuffer().size_bytes();
    if (requestedSize > currentBufferSize) {
        auto* memoryManager = entryBuffer->getMemoryManager();
        auto newBuffer = memoryManager->allocateBuffer(false, std::bit_ceil(requestedSize));
        std::memcpy(newBuffer->getData(), entryBuffer->getData(), currentSize);
        entryBuffer = std::move(newBuffer);
    }
}

void ChecksumReader::read(uint8_t* data, uint64_t size) {
    deserializer.read(data, size);
    if (currentEntrySize.has_value()) {
        resizeBufferIfNeeded(entryBuffer, *currentEntrySize, *currentEntrySize + size);
        std::memcpy(entryBuffer->getData() + *currentEntrySize, data, size);
        *currentEntrySize += size;
    }
}

bool ChecksumReader::finished() {
    return deserializer.finished();
}

uint64_t ChecksumReader::getRemainingBytes() const {
    const auto remaining = deserializer.getReader()->getRemainingBytes();
    if (!currentEntrySize.has_value()) {
        return remaining;
    }
    return remaining < sizeof(uint64_t) ? 0 : remaining - sizeof(uint64_t);
}

void ChecksumReader::onObjectBegin() {
    currentEntrySize.emplace(0);
}

void ChecksumReader::onObjectEnd() {
    KU_ASSERT(currentEntrySize.has_value());
    const uint64_t computedChecksum = common::checksum(entryBuffer->getData(), *currentEntrySize);
    uint64_t storedChecksum{};
    deserializer.deserializeValue(storedChecksum);
    if (storedChecksum != computedChecksum) {
        throw WalChecksumMismatchException(std::string{checksumMismatchMessage});
    }

    currentEntrySize.reset();
}

uint64_t ChecksumReader::getReadOffset() const {
    return deserializer.getReader()->cast<common::BufferedFileReader>()->getReadOffset();
}

} // namespace lbug::storage
