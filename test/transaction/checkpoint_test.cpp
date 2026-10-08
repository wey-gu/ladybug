#include <fstream>
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "api_test/private_api_test.h"
#include "common/exception/runtime.h"
#include "storage/checkpointer.h"
#include "storage/storage_manager.h"
#include "storage/wal/wal.h"
#include "transaction/transaction_manager.h"

using namespace lbug::common;
using namespace lbug::testing;
using namespace lbug::transaction;
using namespace lbug::storage;

namespace lbug {
namespace testing {

class FlakyCheckpointer {
public:
    explicit FlakyCheckpointer(TransactionManager::init_checkpointer_func_t initFunc)
        : initFunc(std::move(initFunc)) {}

    void setCheckpointer(main::ClientContext& context) const {
        TransactionManager::Get(context)->initCheckpointerFunc = initFunc;
    }

private:
    TransactionManager::init_checkpointer_func_t initFunc;
};

class FlakyCheckpointerTest : public PrivateApiTest {
public:
    std::string getInputDir() override { return "empty"; }

    void runFlakyCheckpoint(const FlakyCheckpointer& flakyCheckpointer) {
        conn->query("CALL force_checkpoint_on_close=false;");
        conn->query("CALL auto_checkpoint=false");
        conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);");
        for (auto i = 0; i < 5000; i++) {
            conn->query(stringFormat("CREATE (a:test {id: {}, name: 'name_{}'});", i, i));
        }
        auto context = getClientContext(*conn);
        flakyCheckpointer.setCheckpointer(*context);
        auto res = conn->query("CHECKPOINT;");
        ASSERT_FALSE(res->isSuccess());
    }

    void runTest(const FlakyCheckpointer& flakyCheckpointer) {
        runFlakyCheckpoint(flakyCheckpointer);
        createDBAndConn();
        auto res = conn->query("MATCH (a:test) RETURN COUNT(a);");
        ASSERT_TRUE(res->isSuccess());
        ASSERT_EQ(res->getNext()->getValue(0)->getValue<int64_t>(), 5000);
    }
};

class FlakyCheckpointerFailsOnCheckpointStorage final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnCheckpointStorage(main::ClientContext& clientContext)
        : Checkpointer(clientContext) {}

    bool checkpointStorage() override { throw RuntimeException("checkpoint failed."); }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointStorageFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnCheckpointStorage>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnSerialization final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnSerialization(main::ClientContext& context)
        : Checkpointer(context) {}

    void serializeCatalogAndMetadata(DatabaseHeader&, bool) override {
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointSerializeFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnSerialization>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnWritingHeader final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnWritingHeader(main::ClientContext& context)
        : Checkpointer(context) {}

    void writeDatabaseHeader(const DatabaseHeader&) override {
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointWriteHeaderFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnWritingHeader>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnFlushingShadow final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnFlushingShadow(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages() override {
        // Simulate a failure during flushing the shadow pages.
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointFlushingShadowFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnFlushingShadow>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnLoggingCheckpoint final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnLoggingCheckpoint(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages() override {
        const auto storageManager = StorageManager::Get(clientContext);
        auto& shadowFile = storageManager->getShadowFile();
        // Flush the shadow file.
        shadowFile.flushAll(clientContext);
        // Simulate a failure during logging the checkpoint.
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointLoggingCheckpointFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnLoggingCheckpoint>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

class FlakyCheckpointerFailsOnApplyingShadow final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnApplyingShadow(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages() override {
        const auto storageManager = StorageManager::Get(clientContext);
        auto& shadowFile = storageManager->getShadowFile();
        // Flush the shadow file.
        shadowFile.flushAll(clientContext);
        auto wal = WAL::Get(clientContext);
        // Log the checkpoint to the WAL and flush WAL. This indicates that all shadow pages and
        // files (snapshots of catalog and metadata) have been written to disk. The part that is not
        // done is to replace them with the original pages or catalog and metadata files. If the
        // system crashes before this point, the WAL can still be used to recover the system to a
        // state where the checkpoint can be redone.
        wal->logAndFlushCheckpoint(&clientContext);
        // Simulate a failure during applying shadow pages.
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointApplyingShadowFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

TEST_F(FlakyCheckpointerTest, ShadowReplayKeepsDatabaseLockUntilClose) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnApplyingShadow>(context);
    };
    runFlakyCheckpoint(FlakyCheckpointer(initFlakyCheckpointer));
    createDBAndConn(); // Replays the checkpoint WAL and its shadow pages.
    auto result = conn->query("MATCH (a:test) RETURN COUNT(a);");
    ASSERT_TRUE(result->isSuccess());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 5000);

    const auto independentWriterCanLock = [&]() {
#if defined(_WIN32)
        // Windows file locks belong to a handle, so a second handle is an
        // independent contender even inside this process.
        const auto path = std::filesystem::u8path(databasePath);
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        EXPECT_NE(handle, INVALID_HANDLE_VALUE);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        OVERLAPPED overlapped = {};
        const auto locked = LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlapped);
        const auto error = GetLastError();
        EXPECT_TRUE(locked || error == ERROR_LOCK_VIOLATION);
        if (locked) {
            EXPECT_TRUE(UnlockFileEx(handle, 0, 1, 0, &overlapped));
        }
        CloseHandle(handle);
        return locked != 0;
#else
        // A forked child only makes syscalls; it must not run engine code after fork.
        const auto pid = fork();
        EXPECT_GE(pid, 0);
        if (pid == 0) {
            const auto fd = open(databasePath.c_str(), O_RDWR);
            if (fd < 0) {
                _exit(2);
            }
            struct flock lock {};
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            const auto rc = fcntl(fd, F_SETLK, &lock);
            const auto error = errno;
            close(fd);
            _exit(rc == 0 ? 1 : (error == EACCES || error == EAGAIN) ? 0 : 2);
        }
        int status = 0;
        EXPECT_EQ(waitpid(pid, &status, 0), pid);
        EXPECT_TRUE(WIFEXITED(status));
        return WEXITSTATUS(status) == 1;
#endif
    };
    EXPECT_FALSE(independentWriterCanLock()) << "shadow replay released the live database lock";
    conn.reset();
    database.reset();
    EXPECT_TRUE(independentWriterCanLock()) << "closing the database did not release its lock";
}

class FlakyCheckpointerFailsOnClearingFiles final : public Checkpointer {
public:
    explicit FlakyCheckpointerFailsOnClearingFiles(main::ClientContext& context)
        : Checkpointer(context) {}

    void logCheckpointAndApplyShadowPages() override {
        const auto storageManager = StorageManager::Get(clientContext);
        auto& shadowFile = storageManager->getShadowFile();
        // Flush the shadow file.
        shadowFile.flushAll(clientContext);
        auto wal = WAL::Get(clientContext);
        // Log the checkpoint to the WAL and flush WAL. This indicates that all shadow pages and
        // files (snapshots of catalog and metadata) have been written to disk. The part that is not
        // done is to replace them with the original pages or catalog and metadata files. If the
        // system crashes before this point, the WAL can still be used to recover the system to a
        // state where the checkpoint can be redone.
        wal->logAndFlushCheckpoint(&clientContext);
        shadowFile.applyShadowPages(clientContext);
        // Simulate a failure during clearing the WAL and shadow files.
        throw RuntimeException("checkpoint failed.");
    }
};

TEST_F(FlakyCheckpointerTest, RecoverFromCheckpointClearingFilesFailure) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runTest(flakyCheckpointer);
}

// Simulates a situation where a database attempts to replay a shadow file from an older database
// with the same path
TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchExistingDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // Temporarily rename the shadow file and wal file
    auto shadowFilePath = StorageUtils::getShadowFilePath(databasePath);
    auto walFilePath = StorageUtils::getWALFilePath(databasePath);
    auto tmpShadowFilePath = shadowFilePath + "1";
    auto tmpWALFilePath = walFilePath + "1";
    ASSERT_TRUE(std::filesystem::exists(shadowFilePath));
    ASSERT_TRUE(std::filesystem::exists(walFilePath));
    std::filesystem::rename(shadowFilePath, tmpShadowFilePath);
    std::filesystem::rename(walFilePath, tmpWALFilePath);

    // Recreate a new DB with the same path as before
    createDBAndConn();
    conn->query("CREATE NODE TABLE test(id INT64 PRIMARY KEY, name STRING);");

    // Close the DB
    conn.reset();
    database.reset();

    // Rename the files to the original names
    std::filesystem::rename(tmpShadowFilePath, shadowFilePath);
    std::filesystem::rename(tmpWALFilePath, walFilePath);

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchNewDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), RuntimeException);
}

TEST_F(FlakyCheckpointerTest, ShadowFileDatabaseIDMismatchCorruptedDB) {
    if (inMemMode || systemConfig->checkpointThreshold == 0) {
        GTEST_SKIP();
    }
    auto initFlakyCheckpointer = [](main::ClientContext& context) {
        return std::make_unique<FlakyCheckpointerFailsOnClearingFiles>(context);
    };
    FlakyCheckpointer flakyCheckpointer(initFlakyCheckpointer);
    runFlakyCheckpoint(flakyCheckpointer);

    std::filesystem::remove(databasePath);

    // Create a new DB file and write garbage bytes to it
    std::ofstream ofs(databasePath);
    ofs << "1a1a1a1a1a1a1a1a1a1a";
    ofs.close();

    // The shadow file replay should now fail
    EXPECT_THROW(createDBAndConn(), InternalException);
}

} // namespace testing
} // namespace lbug
