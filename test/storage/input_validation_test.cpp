#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

#include "common/exception/runtime.h"
#include "common/serializer/buffer_writer.h"
#include "common/serializer/deserializer.h"
#include "common/serializer/serializer.h"
#include "common/type_utils.h"
#include "common/vector/value_vector.h"
#include "gtest/gtest.h"
#include "main/client_context.h"
#include "main/connection.h"
#include "main/database.h"
#include "processor/result/flat_tuple.h"
#include "storage/disk_array.h"
#include "storage/disk_array_collection.h"
#include "storage/file_db_id_utils.h"
#include "storage/file_handle.h"
#include "storage/overflow_file.h"
#include "storage/page_manager.h"
#include "storage/storage_manager.h"
#include "storage/wal/wal_record.h"

using namespace lbug::common;
using namespace lbug::storage;

namespace lbug::testing {
namespace {

// A truncated test payload must fail before any out-of-bounds buffer access, even on the old code.
class BoundedReader final : public Reader {
public:
    explicit BoundedReader(const BufferWriter& writer)
        : data{writer.getBlobData()}, size{writer.getSize()} {}

    void read(uint8_t* output, uint64_t count) override {
        if (count > size - position) {
            throw std::out_of_range("Test reader exhausted before input validation");
        }
        std::memcpy(output, data + position, count);
        position += count;
    }

    bool finished() override { return position == size; }

private:
    const uint8_t* data;
    uint64_t size;
    uint64_t position = 0;
};

// Mirror the unchanged native header layout so corruption fixtures need no production test hook.
struct CollectionHeaderFixture {
    static constexpr auto CAPACITY =
        (LBUG_PAGE_SIZE - sizeof(page_idx_t) - sizeof(uint32_t)) / sizeof(DiskArrayHeader);
    std::array<DiskArrayHeader, CAPACITY> headers;
    page_idx_t next = INVALID_PAGE_IDX;
    uint32_t count = 0;
};
static_assert(sizeof(CollectionHeaderFixture) <= LBUG_PAGE_SIZE);

class InputValidationTest : public ::testing::Test {
public:
    void SetUp() override {
        const auto suffix =
            std::to_string(std::random_device{}()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        directory = std::filesystem::temp_directory_path() / ("lbug-input-validation-" + suffix);
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        config.autoCheckpoint = false;
        config.forceCheckpointOnClose = false;
        database = std::make_unique<main::Database>((directory / "graph.lbug").string(), config);
    }

    void TearDown() override {
        database.reset();
        std::error_code error;
        if (!directory.empty()) {
            std::filesystem::remove_all(directory, error);
        }
    }

protected:
    FileHandle& file() { return *database->getStorageManager()->getDataFH(); }

    page_idx_t writePage(const uint8_t* data) {
        const auto page = file().getPageManager()->allocatePage();
        file().writePageToFile(data, page);
        return page;
    }

    static std::string readBytes(const std::string& path) {
        std::ifstream stream{path, std::ios::binary};
        if (!stream.is_open()) {
            throw std::runtime_error("Cannot read test fixture");
        }
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    DiskArrayHeader oneElementArray() {
        std::array<uint8_t, LBUG_PAGE_SIZE> values{};
        const uint64_t value = 42;
        std::memcpy(values.data(), &value, sizeof(value));
        PIP pip;
        pip.pageIdxs[0] = writePage(values.data());
        DiskArrayHeader header;
        header.numElements = 1;
        header.firstPIPPageIdx = writePage(reinterpret_cast<const uint8_t*>(&pip));
        return header;
    }

    main::SystemConfig config{64 * 1024 * 1024, 2};
    std::unique_ptr<main::Database> database;
    std::filesystem::path directory;
};

TEST_F(InputValidationTest, DiskArrayReadRejectsLogicalOutOfBounds) {
    const auto header = oneElementArray();
    auto writeHeader = header;
    DiskArray<uint64_t> array{file(), header, writeHeader,
        &database->getStorageManager()->getShadowFile()};
    EXPECT_EQ(array.get(0, &transaction::DUMMY_TRANSACTION), 42);
    EXPECT_THROW(array.get(1, &transaction::DUMMY_TRANSACTION), RuntimeException);
}

TEST_F(InputValidationTest, DiskArrayUpdateRejectsLogicalOutOfBounds) {
    const auto header = oneElementArray();
    auto writeHeader = header;
    DiskArray<uint64_t> array{file(), header, writeHeader,
        &database->getStorageManager()->getShadowFile(), true};
    EXPECT_THROW(array.update(&transaction::DUMMY_CHECKPOINT_TRANSACTION, 1, 99), RuntimeException);
    EXPECT_EQ(array.get(0, &transaction::DUMMY_TRANSACTION), 42);
}

TEST_F(InputValidationTest, DiskArrayRejectsMissingPageIndices) {
    DiskArrayHeader header;
    header.numElements = 1;
    auto writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
}

TEST_F(InputValidationTest, DiskArrayRejectsOverflowingElementCount) {
    DiskArrayHeader header;
    header.numElements = std::numeric_limits<uint64_t>::max();
    auto writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
}

TEST_F(InputValidationTest, DiskArrayAcceptsEmptyArray) {
    DiskArrayHeader header;
    auto writeHeader = header;
    DiskArray<uint64_t> array{file(), header, writeHeader,
        &database->getStorageManager()->getShadowFile()};
    EXPECT_EQ(array.getNumElements(), 0);
}

TEST_F(InputValidationTest, CollectionRejectsInvalidIndexAndHeaderCount) {
    auto& shadow = database->getStorageManager()->getShadowFile();
    DiskArrayCollection empty{file(), shadow};
    EXPECT_THROW(empty.getDiskArray<uint64_t>(0), RuntimeException);
    EXPECT_EQ(empty.addDiskArray(), 0);
    EXPECT_EQ(empty.getDiskArray<uint64_t>(0)->getNumElements(), 0);
    EXPECT_THROW(empty.getDiskArray<uint64_t>(1), RuntimeException);
    CollectionHeaderFixture header;
    header.count = CollectionHeaderFixture::CAPACITY + 1;
    std::array<uint8_t, LBUG_PAGE_SIZE> data{};
    std::memcpy(data.data(), &header, sizeof(header));
    const auto page = writePage(data.data());
    EXPECT_THROW((DiskArrayCollection{file(), shadow, page}), RuntimeException);
}

TEST_F(InputValidationTest, CollectionRejectsCyclicReservedAndOutOfFileHeaders) {
    auto& shadow = database->getStorageManager()->getShadowFile();
    CollectionHeaderFixture header;
    const auto page = file().getPageManager()->allocatePage();
    header.next = page;
    std::array<uint8_t, LBUG_PAGE_SIZE> data{};
    std::memcpy(data.data(), &header, sizeof(header));
    file().writePageToFile(data.data(), page);
    EXPECT_THROW((DiskArrayCollection{file(), shadow, page}), RuntimeException);
    EXPECT_THROW((DiskArrayCollection{file(), shadow, page_idx_t{0}}), RuntimeException);
    EXPECT_THROW((DiskArrayCollection{file(), shadow, file().getNumPages()}), RuntimeException);
    EXPECT_THROW((DiskArrayCollection{file(), shadow, INVALID_PAGE_IDX}), RuntimeException);
}

TEST_F(InputValidationTest, DiskArrayRejectsInvalidArrayPageAndShortPIP) {
    PIP pip;
    pip.pageIdxs[0] = file().getNumPages() + 1;
    DiskArrayHeader header;
    header.numElements = 1;
    header.firstPIPPageIdx = writePage(reinterpret_cast<const uint8_t*>(&pip));
    auto writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
    header.numElements = (NUM_PAGE_IDXS_PER_PIP + 1) * (LBUG_PAGE_SIZE / sizeof(uint64_t));
    writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
}

TEST_F(InputValidationTest, DiskArrayRejectsCyclicAndOutOfFilePIPs) {
    PIP pip;
    const auto first = file().getPageManager()->allocatePage();
    const auto second = file().getPageManager()->allocatePage();
    pip.nextPipPageIdx = second;
    file().writePageToFile(reinterpret_cast<const uint8_t*>(&pip), first);
    pip.nextPipPageIdx = first;
    file().writePageToFile(reinterpret_cast<const uint8_t*>(&pip), second);
    DiskArrayHeader header;
    header.firstPIPPageIdx = first;
    auto writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
    pip.nextPipPageIdx = file().getNumPages();
    file().writePageToFile(reinterpret_cast<const uint8_t*>(&pip), second);
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
    header.firstPIPPageIdx = file().getNumPages();
    writeHeader = header;
    EXPECT_THROW((DiskArray<uint64_t>{file(), header, writeHeader,
                     &database->getStorageManager()->getShadowFile()}),
        RuntimeException);
}

TEST_F(InputValidationTest, OverflowRejectsOutOfFilePageAndPreservesValidLongString) {
    OverflowFile overflow{&file(), *database->getMemoryManager(),
        &database->getStorageManager()->getShadowFile(), INVALID_PAGE_IDX};
    auto* handle = overflow.addHandle();
    const std::string key(ku_string_t::SHORT_STR_LENGTH + 1, 'a');
    const auto validKey = handle->writeString(file().getPageManager(), key);
    handle->checkpoint();
    handle->checkpointInMemory();
    EXPECT_EQ(handle->readString(transaction::TransactionType::READ_ONLY, validKey), key);
    EXPECT_TRUE(handle->equals(transaction::TransactionType::READ_ONLY, key, validKey));
    auto invalidKey = validKey;
    for (const auto page : {file().getNumPages(), INVALID_PAGE_IDX - 1}) {
        TypeUtils::encodeOverflowPtr(invalidKey.overflowPtr, page, 0);
        EXPECT_THROW(handle->readString(transaction::TransactionType::READ_ONLY, invalidKey),
            RuntimeException);
        EXPECT_THROW(handle->equals(transaction::TransactionType::READ_ONLY, key, invalidKey),
            RuntimeException);
    }
}

TEST_F(InputValidationTest, VectorRejectsOversizedCountBeforeReadingPayload) {
    auto writer = std::make_shared<BufferWriter>();
    Serializer serializer{writer};
    serializer.writeDebuggingInfo("data_type");
    LogicalType::INT64().serialize(serializer);
    serializer.writeDebuggingInfo("num_values");
    serializer.write<sel_t>(DEFAULT_VECTOR_CAPACITY + 1);
    Deserializer deserializer{std::make_unique<BoundedReader>(*writer)};
    EXPECT_THROW(ValueVector::deSerialize(deserializer, database->getMemoryManager(),
                     std::make_shared<DataChunkState>()),
        RuntimeException);
    EXPECT_TRUE(deserializer.finished());
}

TEST_F(InputValidationTest, VectorAcceptsExactlyCapacityValues) {
    ValueVector source{LogicalType::INT64(), database->getMemoryManager()};
    source.setState(std::make_shared<DataChunkState>());
    source.state->initOriginalAndSelectedSize(DEFAULT_VECTOR_CAPACITY);
    for (auto i = 0u; i < DEFAULT_VECTOR_CAPACITY; ++i) {
        source.setNull(i, false);
        source.setValue<int64_t>(i, i);
    }
    auto writer = std::make_shared<BufferWriter>();
    Serializer serializer{writer};
    source.serialize(serializer);
    Deserializer deserializer{std::make_unique<BoundedReader>(*writer)};
    const auto result = ValueVector::deSerialize(deserializer, database->getMemoryManager(),
        std::make_shared<DataChunkState>());
    ASSERT_EQ(result->state->getSelSize(), DEFAULT_VECTOR_CAPACITY);
    EXPECT_EQ(result->getValue<int64_t>(DEFAULT_VECTOR_CAPACITY - 1), DEFAULT_VECTOR_CAPACITY - 1);
    EXPECT_TRUE(deserializer.finished());
}

TEST_F(InputValidationTest, InsertionRecordRejectsEmptyVectors) {
    main::ClientContext context{database.get()};
    for (const auto type : {TableType::NODE, TableType::REL}) {
        auto writer = std::make_shared<BufferWriter>();
        Serializer serializer{writer};
        TableInsertionRecord record{0, type, 1, std::vector<ValueVector*>{}};
        record.serialize(serializer);
        Deserializer deserializer{std::make_unique<BoundedReader>(*writer)};
        EXPECT_THROW(WALRecord::deserialize(deserializer, context), RuntimeException);
        EXPECT_TRUE(deserializer.finished());
    }
}

TEST_F(InputValidationTest, StrictOpenRejectsMalformedWALWithoutChangingSourceBytes) {
    database.reset();
    config.enableChecksums = false;
    const auto path = (directory / "graph.lbug").string();
    database = std::make_unique<main::Database>(path, config);
    {
        main::Connection connection{database.get()};
        ASSERT_TRUE(
            connection.query("CREATE NODE TABLE item(id INT64, PRIMARY KEY(id))")->isSuccess());
        ASSERT_TRUE(connection.query("CHECKPOINT")->isSuccess());
    }
    const auto databaseID = [&] {
        main::ClientContext context{database.get()};
        return database->getStorageManager()->getOrInitDatabaseID(context);
    }();
    database.reset();
    const auto original = readBytes(path);
    const auto walPath = StorageUtils::getWALFilePath(path);
    for (const auto type : {TableType::NODE, TableType::REL}) {
        auto writer = std::make_shared<BufferWriter>();
        Serializer serializer{writer};
        FileDBIDUtils::writeDatabaseID(serializer, databaseID);
        serializer.write(false);
        BeginTransactionRecord{}.serialize(serializer);
        TableInsertionRecord{0, type, 1, std::vector<ValueVector*>{}}.serialize(serializer);
        CommitRecord{}.serialize(serializer);
        {
            std::ofstream stream{walPath, std::ios::binary | std::ios::trunc};
            ASSERT_TRUE(stream.is_open());
            stream.write(reinterpret_cast<const char*>(writer->getBlobData()), writer->getSize());
            stream.flush();
            ASSERT_TRUE(stream.good());
        }
        const auto wal = readBytes(walPath);
        EXPECT_THROW((main::Database{path, config}), RuntimeException);
        EXPECT_EQ(readBytes(path), original);
        EXPECT_EQ(readBytes(walPath), wal);
    }
}

TEST_F(InputValidationTest, ValidWALReplaysAndCheckpointReopens) {
    const auto path = (directory / "graph.lbug").string();
    {
        main::Connection connection{database.get()};
        ASSERT_TRUE(
            connection.query("CREATE NODE TABLE item(id INT64, PRIMARY KEY(id))")->isSuccess());
        ASSERT_TRUE(connection.query("CHECKPOINT")->isSuccess());
        ASSERT_TRUE(connection.query("CREATE (:item {id: 42})")->isSuccess());
    }
    database.reset();
    database = std::make_unique<main::Database>(path, config);
    {
        main::Connection connection{database.get()};
        auto result = connection.query("MATCH (n:item) RETURN n.id");
        ASSERT_TRUE(result->isSuccess());
        ASSERT_TRUE(result->hasNext());
        EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 42);
        EXPECT_FALSE(result->hasNext());
        ASSERT_TRUE(connection.query("CHECKPOINT")->isSuccess());
    }
    database.reset();
    database = std::make_unique<main::Database>(path, config);
    main::Connection connection{database.get()};
    auto result = connection.query("MATCH (n:item) RETURN n.id");
    ASSERT_TRUE(result->isSuccess());
    ASSERT_TRUE(result->hasNext());
    EXPECT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 42);
    EXPECT_FALSE(result->hasNext());
}

} // namespace
} // namespace lbug::testing
