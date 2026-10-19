#include "common/file_system/file_system.h"

#include "common/exception/io.h"
#include "common/string_utils.h"
#if defined(_WIN32)
#include "common/windows_utils.h"
#endif

namespace lbug {
namespace common {

void FileSystem::overwriteFile(const std::string& /*from*/, const std::string& /*to*/) {
    KU_UNREACHABLE;
}

void FileSystem::copyFile(const std::string& /*from*/, const std::string& /*to*/) {
    KU_UNREACHABLE;
}

void FileSystem::createDir(const std::string& /*dir*/) const {
    KU_UNREACHABLE;
}

void FileSystem::removeFileIfExists(const std::string&, const main::ClientContext* /*context*/) {
    KU_UNREACHABLE;
}

bool FileSystem::fileOrPathExists(const std::string& /*path*/, main::ClientContext* /*context*/) {
    KU_UNREACHABLE;
}

std::string FileSystem::expandPath(main::ClientContext* /*context*/,
    const std::string& path) const {
    return path;
}

std::filesystem::path FileSystem::pathFromUtf8(const std::string& path) {
    if (path.find('\0') != std::string::npos) {
        throw IOException("Filesystem path contains an embedded NUL");
    }
#if defined(_WIN32)
    // Internal/VFS joins use '/', but Win32 does not translate that separator
    // inside the verbatim namespace (\\?\). Convert only at the native path
    // boundary, preserving the namespace and Unicode/long-path identity.
    return std::filesystem::path{WindowsUtils::utf8ToUnicode(path.c_str())}.make_preferred();
#else
    return std::filesystem::path{path};
#endif
}

std::string FileSystem::pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    if (path.native().find(L'\0') != std::wstring::npos) {
        throw IOException("Filesystem path contains an embedded NUL");
    }
    return WindowsUtils::unicodeToUTF8(path.c_str());
#else
    return path.string();
#endif
}

std::string FileSystem::joinPath(const std::string& base, const std::string& part) {
    return base + "/" + part;
}

std::string FileSystem::getFileExtension(const std::filesystem::path& path) {
    auto extension = path.extension();
    if (isCompressedFile(path)) {
        extension = path.stem().extension();
    }
    return pathToUtf8(extension);
}

bool FileSystem::isCompressedFile(const std::filesystem::path& path) {
    return isGZIPCompressed(path);
}

std::string FileSystem::getFileName(const std::filesystem::path& path) {
    return pathToUtf8(path.filename());
}

void FileSystem::writeFile(FileInfo& /*fileInfo*/, const uint8_t* /*buffer*/, uint64_t /*numBytes*/,
    uint64_t /*offset*/) const {
    KU_UNREACHABLE;
}

void FileSystem::truncate(FileInfo& /*fileInfo*/, uint64_t /*size*/) const {
    KU_UNREACHABLE;
}

void FileSystem::reset(FileInfo& fileInfo) {
    fileInfo.seek(0, SEEK_SET);
}

bool FileSystem::isGZIPCompressed(const std::filesystem::path& path) {
    auto extensionLowerCase = StringUtils::getLower(pathToUtf8(path.extension()));
    return extensionLowerCase == ".gz" || extensionLowerCase == ".gzip";
}

} // namespace common
} // namespace lbug
