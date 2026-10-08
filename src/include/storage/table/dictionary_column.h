#pragma once

#include <utility>
#include <vector>

#include "dictionary_chunk.h"
#include "storage/table/column.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/string_chunk_data.h"

namespace lbug {
namespace storage {

class StringColumn;

class DictionaryColumn {
public:
    DictionaryColumn(const std::string& name, FileHandle* dataFH, MemoryManager* mm,
        ShadowFile* shadowFile, bool enableCompression);

    void scan(const SegmentState& state, DictionaryChunk& dictChunk) const;

    DictionaryChunk::string_index_t append(const DictionaryChunk& dictChunk, SegmentState& state,
        std::string_view val) const;

    bool canCommitInPlace(const SegmentState& state, uint64_t numNewStrings,
        uint64_t totalStringLengthToAdd) const;

    Column* getDataColumn() const { return dataColumn.get(); }
    Column* getOffsetColumn() const { return offsetColumn.get(); }

private:
    friend class StringColumn;

    // Each pair maps a source dictionary index to an output position in the result vector.
    void scan(const SegmentState& offsetState, const SegmentState& dataState,
        std::vector<std::pair<DictionaryChunk::string_index_t, uint64_t>>& offsetsToScan,
        common::ValueVector* result, const ColumnChunkMetadata& indexMeta) const;

    // Append unique source entries and return their actual destination dictionary indexes.
    // Materialization may reorder entries for locality. Only the caller knows result row positions.
    std::vector<std::pair<DictionaryChunk::string_index_t, DictionaryChunk::string_index_t>>
    materializeToStringChunkDictionary(const SegmentState& offsetState,
        const SegmentState& dataState, std::vector<DictionaryChunk::string_index_t>& indexesToScan,
        StringChunkData& result, const ColumnChunkMetadata& indexMeta) const;

    void scanOffsets(const SegmentState& state, DictionaryChunk::string_offset_t* offsets,
        uint64_t index, uint64_t numValues, uint64_t dataSize) const;
    void scanValue(const SegmentState& dataState, uint64_t startOffset, uint64_t endOffset,
        common::ValueVector* resultVector, uint64_t offsetInVector) const;
    DictionaryChunk::string_index_t appendScannedValueToDictionary(const SegmentState& dataState,
        uint64_t startOffset, uint64_t length, StringChunkData& result) const;

    static bool canDataCommitInPlace(const SegmentState& dataState,
        uint64_t totalStringLengthToAdd);
    bool canOffsetCommitInPlace(const SegmentState& offsetState, const SegmentState& dataState,
        uint64_t numNewStrings, uint64_t totalStringLengthToAdd) const;

private:
    // The offset column stores the offsets for each index, and the data column stores the data in
    // order. Values are never removed from the dictionary during in-place updates, only appended to
    // the end.
    std::unique_ptr<Column> dataColumn;
    std::unique_ptr<Column> offsetColumn;
};

} // namespace storage
} // namespace lbug
