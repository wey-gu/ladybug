#include "common/file_system/local_file_system.h"
#include "graph_test/base_graph_test.h"
#include "graph_test/private_graph_test.h"

namespace lbug {
namespace testing {

class ImportExportDBTest : public DBTest {
public:
    std::string getInputDir() override {
        return TestHelper::appendLbugRootPath("dataset/tinysnb/");
    }
};

TEST_F(ImportExportDBTest, Test) {
    auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto dbPath = common::LocalFileSystem::joinPath(tempDir, "export.tmp");
    auto result = conn->query("EXPORT DATABASE '" + dbPath + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result.reset();
    createNewDB();
    result = conn->query("IMPORT DATABASE '" + dbPath + "'");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("MATCH (a:person)-[e:knows]->(b:person) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 14);
}

#ifdef _WIN32
TEST_F(ImportExportDBTest, VerbatimUnicodeLongPathRoundTrip) {
    const auto tempDir = TestHelper::getTempDir(getTestGroupAndName());
    auto native = std::filesystem::canonical(common::FileSystem::pathFromUtf8(tempDir)).native();
    if (native.rfind(L"\\\\?\\", 0) != 0) {
        native = native.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + native.substr(2) :
                                                 L"\\\\?\\" + native;
    }
    const std::string unicode = "\xE6\xBA\x90\xE7\x9B\xAE\xE5\xBD\x95";
    const auto parent = common::FileSystem::pathToUtf8(std::filesystem::path(native)) + "\\" +
                        unicode + "\\" + std::string(200, 'a');
    std::filesystem::create_directories(common::FileSystem::pathFromUtf8(parent));
    const auto dbPath = common::FileSystem::joinPath(parent, "export.tmp");
    ASSERT_GT(dbPath.size(), 260);
    std::string literal = "'";
    for (const auto c : dbPath) {
        if (c == '\\' || c == '\'') {
            literal += '\\';
        }
        literal += c;
    }
    literal += '\'';
    auto result = conn->query("EXPORT DATABASE " + literal);
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result.reset();
    createNewDB();
    result = conn->query("IMPORT DATABASE " + literal);
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    result = conn->query("MATCH (a:person)-[e:knows]->(b:person) RETURN COUNT(*)");
    ASSERT_TRUE(result->isSuccess()) << result->getErrorMessage();
    ASSERT_TRUE(result->hasNext());
    ASSERT_EQ(result->getNext()->getValue(0)->getValue<int64_t>(), 14);
}
#endif

} // namespace testing
} // namespace lbug
