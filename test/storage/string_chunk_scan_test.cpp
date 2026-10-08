// Backported from LadybugDB/ladybug#659; adapted to the pinned 0.12 API.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/vector/value_vector.h"
#include "gtest/gtest.h"
#include "main/connection.h"
#include "main/database.h"
#include "processor/result/flat_tuple.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/column_chunk.h"
#include "storage/table/column_chunk_data.h"
#include "storage/table/string_chunk_data.h"
#include "storage/table/string_column.h"
#include <span>

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug {
namespace testing {
namespace {

using MaybeString = std::optional<std::string_view>;
using string_index_t = DictionaryChunk::string_index_t;

constexpr std::string_view NULL_SENTINEL = "<NULL>";

class StringChunkScanTest : public ::testing::Test {
public:
    void SetUp() override {
        const auto uniqueSuffix = std::to_string(std::random_device{}()) + "-" +
                                   std::to_string(std::chrono::steady_clock::now()
                                                      .time_since_epoch()
                                                      .count());
        const auto candidate = std::filesystem::temp_directory_path() /
                               ("lbug-string-chunk-scan-" + uniqueSuffix);
        ASSERT_TRUE(std::filesystem::create_directory(candidate));
        testDirectory = candidate;
        databasePath = (testDirectory / "graph.lbug").string();
        database = std::make_unique<main::Database>(databasePath, testConfig);
        conn = std::make_unique<main::Connection>(database.get());
    }

    void TearDown() override {
        conn.reset();
        database.reset();
        std::error_code error;
        if (!testDirectory.empty()) {
            std::filesystem::remove_all(testDirectory, error);
        }
    }

protected:
    std::unique_ptr<main::Database> database;
    std::unique_ptr<main::Connection> conn;
    std::string databasePath;
    std::filesystem::path testDirectory;
    const main::SystemConfig testConfig{64 * 1024 * 1024 /*bufferPoolSize*/, 2 /*maxNumThreads*/};
};

MaybeString value(std::string_view str) {
    return MaybeString{str};
}

MaybeString nullValue() {
    return std::nullopt;
}

std::string printableValue(const MaybeString& maybeValue) {
    if (!maybeValue.has_value()) {
        return std::string{NULL_SENTINEL};
    }
    return std::string{*maybeValue};
}

std::vector<std::string> printableValues(std::span<const MaybeString> values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& maybeValue : values) {
        result.push_back(printableValue(maybeValue));
    }
    return result;
}

std::vector<std::string> expectedRange(std::span<const MaybeString> values, offset_t startRow,
    offset_t numRows) {
    std::vector<std::string> result;
    result.reserve(numRows);
    for (auto i = 0u; i < numRows; i++) {
        result.push_back(printableValue(values[startRow + i]));
    }
    return result;
}

std::vector<std::string> expectedPrefixedRange(std::span<const MaybeString> prefix,
    std::span<const MaybeString> values, offset_t startRow, offset_t numRows) {
    auto result = printableValues(prefix);
    auto range = expectedRange(values, startRow, numRows);
    result.insert(result.end(), range.begin(), range.end());
    return result;
}

LogicalType makeStringLikeType(const StringColumn&) {
    return LogicalType::STRING();
}

void writeMaybeStringValue(StringChunkData& chunk, ValueVector& vector, offset_t row,
    const MaybeString& maybeValue) {
    vector.setNull(0, !maybeValue.has_value());
    if (maybeValue.has_value()) {
        StringVector::addString(&vector, 0, *maybeValue);
    }
    chunk.write(&vector, 0, row);
}

struct PersistedStringChunk {
    std::unique_ptr<ColumnChunkData> chunk;
    ChunkState state;
};

PersistedStringChunk buildPersistedStringChunk(MemoryManager& memoryManager,
    PageManager& pageManager, StringColumn& column, std::span<const MaybeString> values) {
    auto chunk = ColumnChunkFactory::createColumnChunkData(memoryManager,
        makeStringLikeType(column), true /*enableCompression*/,
        std::max<uint64_t>(values.size(), 1), ResidencyState::IN_MEMORY);
    auto& stringChunk = chunk->cast<StringChunkData>();
    ValueVector valueVector{makeStringLikeType(column), &memoryManager};
    for (auto row = 0u; row < values.size(); row++) {
        writeMaybeStringValue(stringChunk, valueVector, row, values[row]);
    }
    chunk->flush(pageManager);

    ChunkState state;
    state.column = &column;
    state.segmentStates.resize(1);
    chunk->initializeScanState(state.segmentStates[0], &column);
    return {std::move(chunk), std::move(state)};
}

struct MaterializedStringChunk {
    std::vector<std::string> values;
    std::vector<std::optional<string_index_t>> indexes;
    uint64_t dictionarySize;
};

MaterializedStringChunk materializeStringChunk(const StringChunkData& chunk, offset_t numRows) {
    MaterializedStringChunk result;
    result.values.reserve(numRows);
    result.indexes.reserve(numRows);
    result.dictionarySize = chunk.getDictionaryChunk().getOffsetChunk()->getNumValues();
    const auto* indexChunk = chunk.getIndexColumnChunk();
    for (auto row = 0u; row < numRows; row++) {
        if (chunk.isNull(row)) {
            result.values.push_back(std::string{NULL_SENTINEL});
            result.indexes.push_back(std::nullopt);
            continue;
        }
        result.values.push_back(chunk.getValue<std::string>(row));
        result.indexes.push_back(indexChunk->getValue<string_index_t>(row));
    }
    return result;
}

void expectIndexesInBounds(const MaterializedStringChunk& chunk) {
    for (const auto& maybeIndex : chunk.indexes) {
        if (maybeIndex.has_value()) {
            EXPECT_LT(*maybeIndex, chunk.dictionarySize);
        }
    }
}

std::vector<std::string> materializeValueVector(const ValueVector& vector, offset_t numRows) {
    std::vector<std::string> values;
    values.reserve(numRows);
    for (auto row = 0u; row < numRows; row++) {
        if (vector.isNull(row)) {
            values.push_back(std::string{NULL_SENTINEL});
            continue;
        }
        values.push_back(vector.getValue<ku_string_t>(row).getAsString());
    }
    return values;
}

MaterializedStringChunk scanToStringChunk(MemoryManager& memoryManager, StringColumn& column,
    const ChunkState& state, offset_t startRow, offset_t numRows,
    std::span<const MaybeString> prefix = {}) {
    auto outputCapacity = std::max<uint64_t>(prefix.size() + numRows, 1);
    auto outputChunk =
        ColumnChunkFactory::createColumnChunkData(memoryManager, makeStringLikeType(column),
            true /*enableCompression*/, outputCapacity, ResidencyState::IN_MEMORY);
    auto& outputStringChunk = outputChunk->cast<StringChunkData>();
    ValueVector valueVector{makeStringLikeType(column), &memoryManager};
    for (auto row = 0u; row < prefix.size(); row++) {
        writeMaybeStringValue(outputStringChunk, valueVector, row, prefix[row]);
    }

    if (prefix.empty()) {
        column.scan(state, outputChunk.get(), startRow, numRows);
    } else {
        static_cast<const Column&>(column).scanSegment(state.segmentStates[0], outputChunk.get(),
            startRow, numRows);
    }

    return materializeStringChunk(outputStringChunk, prefix.size() + numRows);
}

std::vector<std::string> scanToValueVector(MemoryManager& memoryManager, StringColumn& column,
    const ChunkState& state, offset_t startRow, offset_t numRows) {
    ValueVector outputVector{makeStringLikeType(column), &memoryManager};
    column.scan(state, startRow, numRows, &outputVector, 0 /*offsetInVector*/);
    return materializeValueVector(outputVector, numRows);
}

struct ScanCase {
    std::string name;
    std::vector<MaybeString> values;
    offset_t startRow;
    offset_t numRows;
    std::vector<MaybeString> prefix;
};

} // namespace

TEST_F(StringChunkScanTest, PartialAndFullStringChunkScansMatchValueVectorScans) {
    auto* memoryManager = database->getMemoryManager();
    auto* storageManager = database->getStorageManager();
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<ScanCase> cases = {
        {
            "duplicate first occurrence before partial range",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            1,
            3,
            {},
        },
        {
            "dictionary order differs from row encounter order",
            {value("ALPHA"), value("BETA"), value("ALPHA"), value("GAMMA"), value("BETA")},
            1,
            4,
            {},
        },
        {
            "unique values partial range",
            {value("ALPHA"), value("BETA"), value("GAMMA"), value("DELTA")},
            1,
            3,
            {},
        },
        {
            "all values duplicate",
            {value("ALPHA"), value("ALPHA"), value("ALPHA"), value("ALPHA")},
            1,
            3,
            {},
        },
        {
            "duplicate values separated by null",
            {value("ALPHA"), nullValue(), value("ALPHA"), value("BETA"), value("ALPHA")},
            0,
            5,
            {},
        },
        {
            "all-null partial range",
            {value("ROOT"), nullValue(), nullValue(), nullValue(), value("TAIL")},
            1,
            3,
            {},
        },
        {
            "empty strings and duplicates",
            {value(""), value("ALPHA"), value(""), nullValue(), value("BETA"), value("")},
            0,
            6,
            {},
        },
        {
            "full segment uses entire dictionary fast path",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            0,
            4,
            {},
        },
        {
            "partial scan appends after existing dictionary rows",
            {value("REL_A"), value("REL_B"), value("REL_B"), value("REL_A")},
            1,
            3,
            {value("PREFIX_ONE"), value("PREFIX_TWO")},
        },
        {
            "high duplicate pressure partial range",
            {value("ROOT"), value("HOT"), value("HOT"), value("HOT"), value("COLD"), value("HOT"),
                value("COLD"), value("HOT")},
            1,
            7,
            {},
        },
        {
            "low duplicate pressure partial range",
            {value("ROOT"), value("A"), value("B"), value("C"), value("D"), value("E"), value("F"),
                value("A")},
            1,
            7,
            {},
        },
    };

    for (const auto& scanCase : cases) {
        SCOPED_TRACE(scanCase.name);
        auto persisted =
            buildPersistedStringChunk(*memoryManager, pageManager, column, scanCase.values);

        const auto valueVectorValues = scanToValueVector(*memoryManager, column, persisted.state,
            scanCase.startRow, scanCase.numRows);
        const auto stringChunkValues = scanToStringChunk(*memoryManager, column, persisted.state,
            scanCase.startRow, scanCase.numRows, scanCase.prefix);

        EXPECT_EQ(valueVectorValues,
            expectedRange(scanCase.values, scanCase.startRow, scanCase.numRows));
        EXPECT_EQ(stringChunkValues.values, expectedPrefixedRange(scanCase.prefix, scanCase.values,
                                                scanCase.startRow, scanCase.numRows));
        expectIndexesInBounds(stringChunkValues);
    }
}

TEST_F(StringChunkScanTest, PartialStringChunkScansPreserveIndexInvariantsWhenAppending) {
    auto* memoryManager = database->getMemoryManager();
    auto* storageManager = database->getStorageManager();
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<MaybeString> values = {value("REL_A"), value("REL_B"), value("REL_B"),
        value("REL_A")};
    const std::vector<MaybeString> prefix = {value("PREFIX_ONE"), value("PREFIX_TWO")};
    auto persisted = buildPersistedStringChunk(*memoryManager, pageManager, column, values);

    const auto scanned = scanToStringChunk(*memoryManager, column, persisted.state, 1, 3, prefix);

    EXPECT_EQ(scanned.values, expectedPrefixedRange(prefix, values, 1, 3));
    expectIndexesInBounds(scanned);
    ASSERT_EQ(scanned.indexes.size(), 5u);
    ASSERT_TRUE(scanned.indexes[2].has_value());
    ASSERT_TRUE(scanned.indexes[3].has_value());
    ASSERT_TRUE(scanned.indexes[4].has_value());
    EXPECT_EQ(scanned.indexes[2], scanned.indexes[3]);
    EXPECT_NE(scanned.indexes[2], scanned.indexes[4]);
    EXPECT_GE(*scanned.indexes[2], static_cast<string_index_t>(prefix.size()));
    EXPECT_GE(*scanned.indexes[4], static_cast<string_index_t>(prefix.size()));
}

TEST_F(StringChunkScanTest, ValueVectorPartialScansRemainCorrectWhenDictionaryOffsetsCanBeSorted) {
    auto* memoryManager = database->getMemoryManager();
    auto* storageManager = database->getStorageManager();
    PageManager pageManager{storageManager->getDataFH()};
    StringColumn column{"scan_value", LogicalType::STRING(), storageManager->getDataFH(),
        memoryManager, &storageManager->getShadowFile(), true /*enableCompression*/};

    const std::vector<MaybeString> values = {value("REL_A"), value("REL_B"), value("REL_B"),
        value("REL_A")};
    auto persisted = buildPersistedStringChunk(*memoryManager, pageManager, column, values);

    const auto scanned = scanToValueVector(*memoryManager, column, persisted.state, 1, 3);

    EXPECT_EQ(scanned, expectedRange(values, 1, 3));
}

TEST_F(StringChunkScanTest, CorrectingOneAliasListPreservesOtherEntitiesAfterCheckpoint) {
    const auto run = [&](const std::string& query) {
        auto result = conn->query(query);
        EXPECT_TRUE(result->isSuccess()) << result->getErrorMessage();
        return result;
    };
    ASSERT_TRUE(run("CREATE NODE TABLE Entity(id INT64, aliases STRING[], updated_at INT64, "
                    "revision INT64, PRIMARY KEY(id))")->isSuccess());
    ASSERT_TRUE(run("CREATE (:Entity {id:0, aliases:['ALPHA'], updated_at:1, revision:3}), "
                    "(:Entity {id:1, aliases:['BETA'], updated_at:1, revision:3}), "
                    "(:Entity {id:2, aliases:['BETA'], updated_at:1, revision:3}), "
                    "(:Entity {id:3, aliases:['ALPHA'], updated_at:1, revision:3})")
                    ->isSuccess());
    ASSERT_TRUE(run("CHECKPOINT")->isSuccess());
    ASSERT_TRUE(run("MATCH (e:Entity {id:0}) SET e.aliases=[], e.updated_at=2, e.revision=4")
                    ->isSuccess());
    ASSERT_TRUE(run("CHECKPOINT")->isSuccess());
    const auto verify = [&] {
        auto result = run("MATCH (e:Entity) WHERE e.id > 0 RETURN e.id, e.aliases, "
                          "e.updated_at, e.revision ORDER BY e.id");
        ASSERT_EQ(result->getNumTuples(), 3);
        for (int64_t id = 1; id < 4; id++) {
            auto row = result->getNext();
            EXPECT_EQ(row->getValue(0)->getValue<int64_t>(), id);
            EXPECT_EQ(row->getValue(1)->toString(), id == 3 ? "[ALPHA]" : "[BETA]");
            EXPECT_EQ(row->getValue(2)->getValue<int64_t>(), 1);
            EXPECT_EQ(row->getValue(3)->getValue<int64_t>(), 3);
        }
    };
    verify();
    conn.reset();
    database.reset();
    database = std::make_unique<main::Database>(databasePath, testConfig);
    conn = std::make_unique<main::Connection>(database.get());
    verify();
}

} // namespace testing
} // namespace lbug
