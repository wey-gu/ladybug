#include "storage/disk_array_collection.h"

#include <unordered_set>

#include "common/system_config.h"
#include "common/types/types.h"
#include "storage/file_handle.h"
#include "storage/shadow_utils.h"

using namespace lbug::common;

namespace lbug {
namespace storage {

DiskArrayCollection::DiskArrayCollection(FileHandle& fileHandle, ShadowFile& shadowFile,
    bool bypassShadowing)
    : fileHandle(fileHandle), shadowFile{shadowFile}, bypassShadowing{bypassShadowing},
      numHeaders{0} {
    headersForReadTrx.push_back(std::make_unique<HeaderPage>());
    headersForWriteTrx.push_back(std::make_unique<HeaderPage>());
    headerPagesOnDisk = 0;
}

DiskArrayCollection::DiskArrayCollection(FileHandle& fileHandle, ShadowFile& shadowFile,
    page_idx_t firstHeaderPage, bool bypassShadowing)
    : fileHandle(fileHandle), shadowFile{shadowFile}, bypassShadowing{bypassShadowing},
      numHeaders{0} {
    // Read headers from disk
    page_idx_t headerPageIdx = firstHeaderPage;
    std::unordered_set<page_idx_t> visitedHeaderPages;
    do {
        if (headerPageIdx == 0 || headerPageIdx >= fileHandle.getNumPages()) {
            throw RuntimeException("Disk array header page is reserved or outside the file.");
        }
        if (!visitedHeaderPages.insert(headerPageIdx).second) {
            throw RuntimeException("Disk array header page chain contains a cycle.");
        }
        HeaderPage headerPage;
        // Optimistic reads may retry; only the local snapshot may change in the callback.
        fileHandle.optimisticReadPage(headerPageIdx,
            [&](auto* frame) { headerPage = *reinterpret_cast<const HeaderPage*>(frame); });
        if (headerPage.numHeaders > HeaderPage::NUM_HEADERS_PER_PAGE) {
            throw RuntimeException("Disk array header count exceeds the page capacity.");
        }
        headersForReadTrx.push_back(std::make_unique<HeaderPage>(headerPage));
        headersForWriteTrx.push_back(std::make_unique<HeaderPage>(headerPage));
        headerPageIdx = headerPage.nextHeaderPage;
        numHeaders += headerPage.numHeaders;
    } while (headerPageIdx != INVALID_PAGE_IDX);
    headerPagesOnDisk = headersForReadTrx.size();
}

void DiskArrayCollection::checkpoint(page_idx_t firstHeaderPage, PageAllocator& pageAllocator) {
    // Write headers to disk
    page_idx_t headerPage = firstHeaderPage;
    for (page_idx_t indexInMemory = 0; indexInMemory < headersForWriteTrx.size(); indexInMemory++) {
        if (headersForWriteTrx[indexInMemory]->nextHeaderPage == INVALID_PAGE_IDX &&
            indexInMemory < headersForWriteTrx.size() - 1) {
            // This is the first time checkpointing the next disk array, allocate a page for its
            // header
            populateNextHeaderPage(pageAllocator, indexInMemory);
        }

        // Only update if the headers for the given page have changed
        // Or if the page has not yet been written
        if (indexInMemory >= headerPagesOnDisk ||
            *headersForWriteTrx[indexInMemory] != *headersForReadTrx[indexInMemory]) {
            ShadowUtils::updatePage(*pageAllocator.getDataFH(), headerPage,
                true /*writing full page*/, shadowFile, [&](auto* frame) {
                    memcpy(frame, headersForWriteTrx[indexInMemory].get(), sizeof(HeaderPage));
                    if constexpr (sizeof(HeaderPage) < LBUG_PAGE_SIZE) {
                        // Zero remaining data in the page
                        std::fill(frame + sizeof(HeaderPage), frame + LBUG_PAGE_SIZE, 0);
                    }
                });
        }
        headerPage = headersForWriteTrx[indexInMemory]->nextHeaderPage;
    }
    headerPagesOnDisk = headersForWriteTrx.size();
}

void DiskArrayCollection::populateNextHeaderPage(PageAllocator& pageAllocator,
    common::page_idx_t indexInMemory) {
    auto nextHeaderPage = pageAllocator.allocatePage();
    headersForWriteTrx[indexInMemory]->nextHeaderPage = nextHeaderPage;
    // We can't really roll back the structural changes in the PKIndex (the disk arrays are
    // created in the destructor and there are a fixed number which does not change after that
    // point), so we apply those to the version that would otherwise be identical to the one on
    // disk
    headersForReadTrx[indexInMemory]->nextHeaderPage = nextHeaderPage;
}

size_t DiskArrayCollection::addDiskArray() {
    auto oldSize = numHeaders++;
    // This may not be the last header page. If we rollback there may be header pages which are
    // empty
    auto pageIdx = numHeaders % HeaderPage::NUM_HEADERS_PER_PAGE;
    if (pageIdx >= headersForWriteTrx.size()) {

        headersForWriteTrx.emplace_back(std::make_unique<HeaderPage>());
        // Also add a new read header page as we need to pass read headers to the disk arrays
        // Newly added read headers will be empty until checkpointing
        headersForReadTrx.emplace_back(std::make_unique<HeaderPage>());
    }

    auto& headerPage = *headersForWriteTrx[pageIdx];
    KU_ASSERT(headerPage.numHeaders < HeaderPage::NUM_HEADERS_PER_PAGE);
    auto indexInPage = headerPage.numHeaders;
    headerPage.headers[indexInPage] = DiskArrayHeader();
    headerPage.numHeaders++;
    headersForReadTrx[pageIdx]->numHeaders++;
    return oldSize;
}

void DiskArrayCollection::reclaimStorage(PageAllocator& pageAllocator,
    common::page_idx_t firstHeaderPage) const {
    auto headerPage = firstHeaderPage;
    for (page_idx_t indexInMemory = 0; indexInMemory < headersForReadTrx.size(); indexInMemory++) {
        if (headerPage == INVALID_PAGE_IDX) {
            break;
        }
        pageAllocator.freePage(headerPage);
        headerPage = headersForReadTrx[indexInMemory]->nextHeaderPage;
    }
}

} // namespace storage
} // namespace lbug
