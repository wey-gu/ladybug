#include "storage/wal/wal_replayer.h"

#include <algorithm>

#include "binder/binder.h"
#include "catalog/catalog_entry/scalar_macro_catalog_entry.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "catalog/catalog_entry/type_catalog_entry.h"
#include "common/exception/internal.h"
#include "common/exception/runtime.h"
#include "common/file_system/file_info.h"
#include "common/file_system/file_system.h"
#include "common/file_system/virtual_file_system.h"
#include "common/serializer/buffered_file.h"
#include "common/sha256.h"
#include "extension/extension_manager.h"
#include "main/client_context.h"
#include "main/database.h"
#include "processor/expression_mapper.h"
#include "storage/database_header.h"
#include "storage/file_db_id_utils.h"
#include "storage/local_storage/local_rel_table.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "storage/wal/checksum_reader.h"
#include "storage/wal/malformed_wal.h"
#include "storage/wal/wal_record.h"
#include "transaction/transaction_context.h"

using namespace lbug::binder;
using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;
using namespace lbug::transaction;

namespace lbug {
namespace storage {

static constexpr std::string_view checksumMismatchMessage =
    "Checksum verification failed, the WAL file is corrupted.";

WALReplayer::WALReplayer(main::ClientContext& clientContext) : clientContext{clientContext} {
    walPath = StorageUtils::getWALFilePath(clientContext.getDatabasePath());
    shadowFilePath = StorageUtils::getShadowFilePath(clientContext.getDatabasePath());
}

static WALHeader readWALHeader(Deserializer& deserializer) {
    WALHeader header{};
    deserializer.deserializeValue(header.databaseID);

    // It is possible to read a value other than 0/1 when deserializing the flag
    // This causes some weird behaviours with some toolchains so we manually do the conversion here
    uint8_t enableChecksumsBytes = 0;
    deserializer.deserializeValue(enableChecksumsBytes);
    header.enableChecksums = enableChecksumsBytes != 0;

    return header;
}

static Deserializer initDeserializer(FileInfo& fileInfo, main::ClientContext& clientContext,
    bool enableChecksums) {
    if (enableChecksums) {
        return Deserializer{std::make_unique<ChecksumReader>(fileInfo,
            *MemoryManager::Get(clientContext), checksumMismatchMessage)};
    } else {
        return Deserializer{std::make_unique<BufferedFileReader>(fileInfo)};
    }
}

static void checkWALHeader(const WALHeader& header, bool enableChecksums) {
    if (enableChecksums != header.enableChecksums) {
        throw RuntimeException(stringFormat(
            "The database you are trying to open was serialized with enableChecksums={} but you "
            "are trying to open it with enableChecksums={}. Please open your database using the "
            "correct enableChecksums config. If you wish to change this for your database, please "
            "use the export/import functionality.",
            TypeUtils::toString(header.enableChecksums), TypeUtils::toString(enableChecksums)));
    }
}

static uint64_t getReadOffset(Deserializer& deSer, bool enableChecksums) {
    if (enableChecksums) {
        return deSer.getReader()->cast<ChecksumReader>()->getReadOffset();
    } else {
        return deSer.getReader()->cast<BufferedFileReader>()->getReadOffset();
    }
}

// Decode one record during dry replay. Decoder invariant failures on persisted
// bytes (KU_UNREACHABLE/assertions and decoder RuntimeExceptions) become a typed
// cause so strict recovery can offer the verified committed prefix instead of
// refusing on an unclassified internal error. Causes that already carry a type,
// and environmental failures (I/O, memory, interruption), keep their meaning.
static std::unique_ptr<WALRecord> decodeWALRecord(Deserializer& deserializer,
    const main::ClientContext& clientContext) {
    try {
        return WALRecord::deserialize(deserializer, clientContext);
    } catch (const InvalidWalRecordTypeException&) {
        throw;
    } catch (const UnexpectedEndOfFileException&) {
        throw;
    } catch (const InternalException& error) {
        throw UndecodableWalRecordException(error.what());
    } catch (const RuntimeException& error) {
        throw UndecodableWalRecordException(error.what());
    }
}

// Hash with fixed memory before releasing the WAL reader and after callback publication.
static std::string prefixSha256(FileInfo& file, uint64_t end) {
    SHA256 digest;
    uint8_t buffer[65536];
    for (uint64_t offset = 0; offset < end;) {
        const auto size = std::min<uint64_t>(sizeof(buffer), end - offset);
        file.readFromFile(buffer, size, offset);
        digest.addString(std::string{reinterpret_cast<char*>(buffer), static_cast<size_t>(size)});
        offset += size;
    }
    std::string value(SHA256::SHA256_HASH_LENGTH_TEXT, '\0');
    digest.finishSHA256(value.data());
    return value;
}

void WALReplayer::replay(bool throwOnWalReplayFailure, bool enableChecksums) const {
    auto vfs = VirtualFileSystem::GetUnsafe(clientContext);
    Checkpointer checkpointer(clientContext);
    bool checkpointAlreadyLoaded = false;
    // First, check if the WAL file exists. If it does not, we can safely remove the shadow file.
    if (!vfs->fileOrPathExists(walPath, &clientContext)) {
        removeFileIfExists(shadowFilePath);
        // Read the checkpointed data from the disk.
        checkpointer.readCheckpoint();
        return;
    }
    // If the WAL file exists, we need to replay it.
    auto fileInfo = openWALFile();
    // Check if the wal file is empty. If so, we do not need to replay anything.
    if (fileInfo->getFileSize() == 0) {
        removeWALAndShadowFiles();
        // Read the checkpointed data from the disk.
        checkpointer.readCheckpoint();
        return;
    }
    // A previous unclean exit may have left non-durable contents in the WAL, so before we start
    // replaying the WAL records, make a best-effort attempt at ensuring the WAL is fully durable.
    syncWALFile(*fileInfo);

    // Start replaying the WAL records.
    try {
        // First, we dry run the replay to find out the offset of the last record that was
        // CHECKPOINT or COMMIT.
        WALReplayInfo replayInfo;
        try {
            replayInfo = dryReplay(*fileInfo, throwOnWalReplayFailure, enableChecksums);
        } catch (const MalformedWalException& malformed) {
            // Windows cannot rename an open WAL. Dry replay and its reader have
            // unwound; close this last handle before calling the evidence owner.
            fileInfo.reset();
            // A catchably invalid checkpoint cannot authorize WAL isolation.
            // This loads checkpoint metadata without applying WAL/shadow pages.
            try {
                checkpointer.readCheckpoint();
                checkpointAlreadyLoaded = true;
            } catch (const std::exception& failure) {
                throw RuntimeException(
                    std::string{"Checkpoint validation failed during malformed-WAL admission: "} +
                    failure.what() + "; original WAL failure: " + malformed.what());
            }
            auto* main = StorageManager::Get(clientContext)->getDataFH()->getFileInfo();
            auto recovered = lbug::main::WalRecoveryAction::Decline;
            try {
                recovered = clientContext.getDatabase()->tryRecoverMalformedWal(*main,
                    malformed.prefix ? &*malformed.prefix : nullptr);
            } catch (const std::exception& failure) {
                throw RuntimeException(std::string{"Malformed-WAL recovery callback failed: "} +
                                       failure.what() +
                                       "; original WAL failure: " + malformed.what());
            }
            if (recovered == lbug::main::WalRecoveryAction::Decline) {
                throw;
            }
            if (recovered == lbug::main::WalRecoveryAction::Isolate) {
                if (vfs->fileOrPathExists(walPath, &clientContext)) {
                    throw RuntimeException(std::string{"Malformed-WAL recovery left the active WAL "
                                                       "present; original WAL failure: "} +
                                           malformed.what());
                }
                return;
            }
            // Explicit prefix action is a different contract from boolean WAL isolation.
            // Reopen after Windows rename and verify the exact admitted bytes before replay.
            if (!malformed.prefix || !enableChecksums || malformed.prefix->transactionCount == 0 ||
                malformed.prefix->commitEnd == 0) {
                throw RuntimeException(
                    "Verified-prefix recovery requires a checksummed committed prefix.");
            }
            fileInfo = openWALFile();
            if (fileInfo->getFileSize() != malformed.prefix->commitEnd ||
                prefixSha256(*fileInfo, malformed.prefix->commitEnd) !=
                    malformed.prefix->prefixSha256) {
                throw RuntimeException(
                    "Published WAL prefix differs from the native verified prefix.");
            }
            syncWALFile(*fileInfo);
            replayInfo = dryReplay(*fileInfo, true, enableChecksums);
            if (replayInfo.isLastRecordCheckpoint ||
                replayInfo.offsetDeserialized != malformed.prefix->commitEnd) {
                throw RuntimeException("Published WAL prefix is not the verified COMMIT boundary.");
            }
        }
        auto [offsetDeserialized, isLastRecordCheckpoint] = replayInfo;
        if (isLastRecordCheckpoint) {
            // If the last record is a checkpoint, we resume by replaying the shadow file.
            ShadowFile::replayShadowPageRecords(clientContext);
            removeWALAndShadowFiles();
            // Re-read checkpointed data from disk again as now the shadow file is applied.
            checkpointer.readCheckpoint();
        } else {
            // There is no checkpoint record, so we should remove the shadow file if it exists.
            removeFileIfExists(shadowFilePath);
            // Read the checkpointed data from the disk.
            if (!checkpointAlreadyLoaded)
                checkpointer.readCheckpoint();
            // Resume by replaying the WAL file from the beginning until the last COMMIT record.
            Deserializer deserializer = initDeserializer(*fileInfo, clientContext, enableChecksums);

            if (offsetDeserialized > 0) {
                // Make sure the WAL file is for the current database
                deserializer.getReader()->onObjectBegin();
                const auto walHeader = readWALHeader(deserializer);
                FileDBIDUtils::verifyDatabaseID(*fileInfo,
                    StorageManager::Get(clientContext)->getOrInitDatabaseID(clientContext),
                    walHeader.databaseID);
                deserializer.getReader()->onObjectEnd();
            }

            while (getReadOffset(deserializer, enableChecksums) < offsetDeserialized) {
                KU_ASSERT(!deserializer.finished());
                auto walRecord = WALRecord::deserialize(deserializer, clientContext);
                replayWALRecord(*walRecord);
            }
            // After replaying all the records, we should truncate the WAL file to the last
            // COMMIT/CHECKPOINT record.
            truncateWALFile(*fileInfo, offsetDeserialized);
        }
    } catch (const std::exception&) {
        auto transactionContext = TransactionContext::Get(clientContext);
        if (transactionContext->hasActiveTransaction()) {
            // Handle the case that some transaction went during replaying. We should roll back
            // under this case. Usually this shouldn't happen, but it is possible if we have a bug
            // with the replay logic. This is to handle cases like that so we don't corrupt
            // transactions that have been replayed.
            transactionContext->rollback();
        }
        throw;
    }
}

WALReplayer::WALReplayInfo WALReplayer::dryReplay(FileInfo& fileInfo, bool throwOnWalReplayFailure,
    bool enableChecksums) const {
    uint64_t offsetDeserialized = 0;
    bool isLastRecordCheckpoint = false;
    bool validatedHeader = false;
    bool inTransaction = false;
    uint64_t transactionCount = 0;
    uint64_t failedRecordStart = 0;
    auto malformed = [&](const std::exception& error, WalPrefixStopReason reason) {
        std::optional<VerifiedWalPrefix> prefix;
        if (enableChecksums) {
            prefix = VerifiedWalPrefix{offsetDeserialized, transactionCount, failedRecordStart,
                fileInfo.getFileSize(), reason, false, prefixSha256(fileInfo, offsetDeserialized)};
            if (reason == WalPrefixStopReason::InvalidRecordType) {
                // A zero suffix is an observation only. It may have replaced durable data.
                uint8_t buffer[4096];
                bool allZero = true;
                for (auto offset = failedRecordStart; offset < prefix->walSize;) {
                    const auto size = std::min<uint64_t>(sizeof(buffer), prefix->walSize - offset);
                    fileInfo.readFromFile(buffer, size, offset);
                    for (uint64_t i = 0; i < size; ++i)
                        allZero = allZero && buffer[i] == 0;
                    offset += size;
                }
                prefix->zeroSuffix = allZero;
            }
        }
        return MalformedWalException(error.what(), prefix);
    };
    try {
        Deserializer deserializer = initDeserializer(fileInfo, clientContext, enableChecksums);

        // Verify identity before assigning any malformed-record authority.
        deserializer.getReader()->onObjectBegin();
        const auto walHeader = readWALHeader(deserializer);
        checkWALHeader(walHeader, enableChecksums);
        bool matchedPersistedHeader = false;
        if (throwOnWalReplayFailure) {
            auto* retainedMain = StorageManager::Get(clientContext)->getDataFH()->getFileInfo();
            const auto mainHeader = DatabaseHeader::readDatabaseHeader(*retainedMain);
            if (mainHeader) {
                FileDBIDUtils::verifyDatabaseID(fileInfo, mainHeader->databaseID,
                    walHeader.databaseID);
                matchedPersistedHeader = true;
            }
        }
        deserializer.getReader()->onObjectEnd();
        // Additional identity admission belongs only to strict recovery. Lenient
        // dry replay must still reach ordinary replay's identity check; swallowing
        // a new early identity error would silently truncate a foreign WAL.
        validatedHeader = matchedPersistedHeader;

        bool finishedDeserializing = deserializer.finished();
        while (!finishedDeserializing) {
            failedRecordStart = getReadOffset(deserializer, enableChecksums);
            auto walRecord = decodeWALRecord(deserializer, clientContext);
            finishedDeserializing = deserializer.finished();
            switch (walRecord->type) {
            case WALRecordType::BEGIN_TRANSACTION_RECORD:
                if (inTransaction)
                    throw InvalidWalTransactionException("Nested WAL transaction.");
                inTransaction = true;
                break;
            case WALRecordType::COMMIT_RECORD:
                if (!inTransaction)
                    throw InvalidWalTransactionException("WAL COMMIT without BEGIN.");
                inTransaction = false;
                ++transactionCount;
                // deserialize includes onObjectEnd/checksum verification.
                offsetDeserialized = getReadOffset(deserializer, enableChecksums);
                break;
            case WALRecordType::CHECKPOINT_RECORD:
                // A checkpoint may bind shadow pages. Never salvage around uncertain publication.
                if (inTransaction || !finishedDeserializing) {
                    throw RuntimeException(
                        "WAL checkpoint is not a terminal transaction boundary.");
                }
                isLastRecordCheckpoint = true;
                offsetDeserialized = getReadOffset(deserializer, enableChecksums);
                break;
            default:
                if (!inTransaction)
                    throw InvalidWalTransactionException("WAL data outside transaction.");
                break;
            }
        }
        // Complete checksummed records without COMMIT are an ordinary crashed
        // transaction, not malformed input. Keep the previous COMMIT boundary
        // so generic strict open can roll back this tail without a callback.
    } catch (const InvalidWalRecordTypeException& error) {
        if (throwOnWalReplayFailure && validatedHeader) {
            throw malformed(error, WalPrefixStopReason::InvalidRecordType);
        }
        if (throwOnWalReplayFailure) {
            throw;
        }
    } catch (const WalChecksumMismatchException& error) {
        if (throwOnWalReplayFailure && validatedHeader) {
            throw malformed(error, WalPrefixStopReason::ChecksumMismatch);
        }
        if (throwOnWalReplayFailure) {
            throw;
        }
    } catch (const UnexpectedEndOfFileException& error) {
        if (throwOnWalReplayFailure && validatedHeader) {
            throw malformed(error, WalPrefixStopReason::UnexpectedEof);
        }
        if (throwOnWalReplayFailure)
            throw;
    } catch (const InvalidWalTransactionException& error) {
        if (throwOnWalReplayFailure && validatedHeader) {
            throw malformed(error, WalPrefixStopReason::InvalidTransaction);
        }
        if (throwOnWalReplayFailure)
            throw;
    } catch (const UndecodableWalRecordException& error) {
        if (throwOnWalReplayFailure && validatedHeader) {
            throw malformed(error, WalPrefixStopReason::UndecodableRecord);
        }
        if (throwOnWalReplayFailure)
            throw;
    } catch (...) {
        // If we hit an exception while deserializing, we assume that the WAL file is (partially)
        // corrupted. This should only happen for records of the last transaction recorded.
        if (throwOnWalReplayFailure) {
            throw;
        }
    }
    return {offsetDeserialized, isLastRecordCheckpoint};
}

void WALReplayer::replayWALRecord(WALRecord& walRecord) const {
    switch (walRecord.type) {
    case WALRecordType::BEGIN_TRANSACTION_RECORD: {
        TransactionContext::Get(clientContext)->beginRecoveryTransaction();
    } break;
    case WALRecordType::COMMIT_RECORD: {
        TransactionContext::Get(clientContext)->commit();
    } break;
    case WALRecordType::CREATE_CATALOG_ENTRY_RECORD: {
        replayCreateCatalogEntryRecord(walRecord);
    } break;
    case WALRecordType::DROP_CATALOG_ENTRY_RECORD: {
        replayDropCatalogEntryRecord(walRecord);
    } break;
    case WALRecordType::ALTER_TABLE_ENTRY_RECORD: {
        replayAlterTableEntryRecord(walRecord);
    } break;
    case WALRecordType::TABLE_INSERTION_RECORD: {
        replayTableInsertionRecord(walRecord);
    } break;
    case WALRecordType::NODE_DELETION_RECORD: {
        replayNodeDeletionRecord(walRecord);
    } break;
    case WALRecordType::NODE_UPDATE_RECORD: {
        replayNodeUpdateRecord(walRecord);
    } break;
    case WALRecordType::REL_DELETION_RECORD: {
        replayRelDeletionRecord(walRecord);
    } break;
    case WALRecordType::REL_DETACH_DELETE_RECORD: {
        replayRelDetachDeletionRecord(walRecord);
    } break;
    case WALRecordType::REL_UPDATE_RECORD: {
        replayRelUpdateRecord(walRecord);
    } break;
    case WALRecordType::COPY_TABLE_RECORD: {
        replayCopyTableRecord(walRecord);
    } break;
    case WALRecordType::UPDATE_SEQUENCE_RECORD: {
        replayUpdateSequenceRecord(walRecord);
    } break;
    case WALRecordType::LOAD_EXTENSION_RECORD: {
        replayLoadExtensionRecord(walRecord);
    } break;
    case WALRecordType::CHECKPOINT_RECORD: {
        // This record should not be replayed. It is only used to indicate that the previous records
        // had been replayed and shadow files are created.
        KU_UNREACHABLE;
    }
    default:
        KU_UNREACHABLE;
    }
}

void WALReplayer::replayCreateCatalogEntryRecord(WALRecord& walRecord) const {
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto storageManager = StorageManager::Get(clientContext);
    auto& record = walRecord.cast<CreateCatalogEntryRecord>();
    switch (record.ownedCatalogEntry->getType()) {
    case CatalogEntryType::NODE_TABLE_ENTRY:
    case CatalogEntryType::REL_GROUP_ENTRY: {
        auto& entry = record.ownedCatalogEntry->constCast<TableCatalogEntry>();
        auto newEntry = catalog->createTableEntry(transaction,
            entry.getBoundCreateTableInfo(transaction, record.isInternal));
        storageManager->createTable(newEntry->ptrCast<TableCatalogEntry>());
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        auto& macroEntry = record.ownedCatalogEntry->constCast<ScalarMacroCatalogEntry>();
        catalog->addScalarMacroFunction(transaction, macroEntry.getName(),
            macroEntry.getMacroFunction()->copy());
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        auto& sequenceEntry = record.ownedCatalogEntry->constCast<SequenceCatalogEntry>();
        catalog->createSequence(transaction,
            sequenceEntry.getBoundCreateSequenceInfo(record.isInternal));
    } break;
    case CatalogEntryType::TYPE_ENTRY: {
        auto& typeEntry = record.ownedCatalogEntry->constCast<TypeCatalogEntry>();
        catalog->createType(transaction, typeEntry.getName(), typeEntry.getLogicalType().copy());
    } break;
    case CatalogEntryType::INDEX_ENTRY: {
        catalog->createIndex(transaction, std::move(record.ownedCatalogEntry));
    } break;
    default: {
        KU_UNREACHABLE;
    }
    }
}

void WALReplayer::replayDropCatalogEntryRecord(const WALRecord& walRecord) const {
    auto& dropEntryRecord = walRecord.constCast<DropCatalogEntryRecord>();
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    const auto entryID = dropEntryRecord.entryID;
    switch (dropEntryRecord.entryType) {
    case CatalogEntryType::NODE_TABLE_ENTRY:
    case CatalogEntryType::REL_GROUP_ENTRY: {
        KU_ASSERT(Catalog::Get(clientContext));
        catalog->dropTableEntry(transaction, entryID);
    } break;
    case CatalogEntryType::SEQUENCE_ENTRY: {
        catalog->dropSequence(transaction, entryID);
    } break;
    case CatalogEntryType::INDEX_ENTRY: {
        catalog->dropIndex(transaction, entryID);
    } break;
    case CatalogEntryType::SCALAR_MACRO_ENTRY: {
        catalog->dropMacroEntry(transaction, entryID);
    } break;
    default: {
        KU_UNREACHABLE;
    }
    }
}

void WALReplayer::replayAlterTableEntryRecord(const WALRecord& walRecord) const {
    auto binder = Binder(&clientContext);
    auto& alterEntryRecord = walRecord.constCast<AlterTableEntryRecord>();
    auto catalog = Catalog::Get(clientContext);
    auto transaction = transaction::Transaction::Get(clientContext);
    auto storageManager = StorageManager::Get(clientContext);
    auto ownedAlterInfo = alterEntryRecord.ownedAlterInfo.get();
    catalog->alterTableEntry(transaction, *ownedAlterInfo);
    auto& pageAllocator = *PageManager::Get(clientContext);
    switch (ownedAlterInfo->alterType) {
    case AlterType::ADD_PROPERTY: {
        const auto exprBinder = binder.getExpressionBinder();
        const auto addInfo = ownedAlterInfo->extraInfo->constPtrCast<BoundExtraAddPropertyInfo>();
        // We don't implicit cast here since it must already be done the first time
        const auto boundDefault =
            exprBinder->bindExpression(*addInfo->propertyDefinition.defaultExpr);
        auto exprMapper = ExpressionMapper();
        const auto defaultValueEvaluator = exprMapper.getEvaluator(boundDefault);
        defaultValueEvaluator->init(ResultSet(0) /* dummy ResultSet */, &clientContext);
        const auto entry = catalog->getTableCatalogEntry(transaction, ownedAlterInfo->tableName);
        const auto& addedProp = entry->getProperty(addInfo->propertyDefinition.getName());
        TableAddColumnState state{addedProp, *defaultValueEvaluator};
        KU_ASSERT(StorageManager::Get(clientContext));
        switch (entry->getTableType()) {
        case TableType::REL: {
            for (auto& relEntryInfo : entry->cast<RelGroupCatalogEntry>().getRelEntryInfos()) {
                storageManager->getTable(relEntryInfo.oid)
                    ->addColumn(transaction, state, pageAllocator);
            }
        } break;
        case TableType::NODE: {
            storageManager->getTable(entry->getTableID())
                ->addColumn(transaction, state, pageAllocator);
        } break;
        default: {
            KU_UNREACHABLE;
        }
        }
    } break;
    case AlterType::ADD_FROM_TO_CONNECTION: {
        auto extraInfo = ownedAlterInfo->extraInfo->constPtrCast<BoundExtraAlterFromToConnection>();
        auto relGroupEntry = catalog->getTableCatalogEntry(transaction, ownedAlterInfo->tableName)
                                 ->ptrCast<RelGroupCatalogEntry>();
        auto relEntryInfo =
            relGroupEntry->getRelEntryInfo(extraInfo->fromTableID, extraInfo->toTableID);
        storageManager->addRelTable(relGroupEntry, *relEntryInfo);
    } break;
    default:
        break;
    }
}

void WALReplayer::replayTableInsertionRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    switch (insertionRecord.tableType) {
    case TableType::NODE: {
        replayNodeTableInsertRecord(walRecord);
    } break;
    case TableType::REL: {
        replayRelTableInsertRecord(walRecord);
    } break;
    default: {
        throw RuntimeException("Invalid table type for insertion replay in WAL record.");
    }
    }
}

static std::shared_ptr<DataChunkState> getInsertionAnchorState(const TableInsertionRecord& record) {
    if (record.ownedVectors.empty() || !record.ownedVectors[0] || !record.ownedVectors[0]->state) {
        throw RuntimeException("Corrupted WAL record: table insertion has no vector state.");
    }
    return record.ownedVectors[0]->state;
}

void WALReplayer::replayNodeTableInsertRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    const auto anchorState = getInsertionAnchorState(insertionRecord);
    const auto tableID = insertionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    const auto numNodes = anchorState->getSelVector().getSelSize();
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        insertionRecord.ownedVectors[i]->setState(anchorState);
    }
    std::vector<ValueVector*> propertyVectors(insertionRecord.ownedVectors.size());
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        propertyVectors[i] = insertionRecord.ownedVectors[i].get();
    }
    KU_ASSERT(table.getPKColumnID() < insertionRecord.ownedVectors.size());
    auto& pkVector = *insertionRecord.ownedVectors[table.getPKColumnID()];
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    const auto insertState =
        std::make_unique<NodeTableInsertState>(*nodeIDVector, pkVector, propertyVectors);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    table.initInsertState(&clientContext, *insertState);
    anchorState->getSelVectorUnsafe().setToFiltered(1);
    for (auto i = 0u; i < numNodes; i++) {
        anchorState->getSelVectorUnsafe()[0] = i;
        table.insert(transaction::Transaction::Get(clientContext), *insertState);
    }
}

void WALReplayer::replayRelTableInsertRecord(const WALRecord& walRecord) const {
    const auto& insertionRecord = walRecord.constCast<TableInsertionRecord>();
    const auto anchorState = getInsertionAnchorState(insertionRecord);
    const auto tableID = insertionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    const auto numRels = anchorState->getSelVector().getSelSize();
    anchorState->getSelVectorUnsafe().setToFiltered(1);
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        insertionRecord.ownedVectors[i]->setState(anchorState);
    }
    std::vector<ValueVector*> propertyVectors;
    for (auto i = 0u; i < insertionRecord.ownedVectors.size(); i++) {
        if (i < LOCAL_REL_ID_COLUMN_ID) {
            // Skip the first two vectors which are the src nodeID and the dst nodeID.
            continue;
        }
        propertyVectors.push_back(insertionRecord.ownedVectors[i].get());
    }
    const auto insertState = std::make_unique<RelTableInsertState>(
        *insertionRecord.ownedVectors[LOCAL_BOUND_NODE_ID_COLUMN_ID],
        *insertionRecord.ownedVectors[LOCAL_NBR_NODE_ID_COLUMN_ID], propertyVectors);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    for (auto i = 0u; i < numRels; i++) {
        anchorState->getSelVectorUnsafe()[0] = i;
        table.initInsertState(&clientContext, *insertState);
        table.insert(transaction::Transaction::Get(clientContext), *insertState);
    }
}

void WALReplayer::replayNodeDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<NodeDeletionRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    const auto anchorState = deletionRecord.ownedPKVector->state;
    KU_ASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    nodeIDVector->setValue<internalID_t>(0,
        internalID_t{deletionRecord.nodeOffset, deletionRecord.tableID});
    const auto deleteState =
        std::make_unique<NodeTableDeleteState>(*nodeIDVector, *deletionRecord.ownedPKVector);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    table.delete_(transaction::Transaction::Get(clientContext), *deleteState);
}

void WALReplayer::replayNodeUpdateRecord(const WALRecord& walRecord) const {
    const auto& updateRecord = walRecord.constCast<NodeUpdateRecord>();
    const auto tableID = updateRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<NodeTable>();
    const auto anchorState = updateRecord.ownedPropertyVector->state;
    KU_ASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID());
    nodeIDVector->setState(anchorState);
    nodeIDVector->setValue<internalID_t>(0,
        internalID_t{updateRecord.nodeOffset, updateRecord.tableID});
    const auto updateState = std::make_unique<NodeTableUpdateState>(updateRecord.columnID,
        *nodeIDVector, *updateRecord.ownedPropertyVector);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    table.update(transaction::Transaction::Get(clientContext), *updateState);
}

void WALReplayer::replayRelDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<RelDeletionRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    const auto anchorState = deletionRecord.ownedRelIDVector->state;
    KU_ASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto deleteState =
        std::make_unique<RelTableDeleteState>(*deletionRecord.ownedSrcNodeIDVector,
            *deletionRecord.ownedDstNodeIDVector, *deletionRecord.ownedRelIDVector);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    table.delete_(transaction::Transaction::Get(clientContext), *deleteState);
}

void WALReplayer::replayRelDetachDeletionRecord(const WALRecord& walRecord) const {
    const auto& deletionRecord = walRecord.constCast<RelDetachDeleteRecord>();
    const auto tableID = deletionRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    const auto anchorState = deletionRecord.ownedSrcNodeIDVector->state;
    KU_ASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto dstNodeIDVector =
        std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    const auto relIDVector = std::make_unique<ValueVector>(LogicalType{LogicalTypeID::INTERNAL_ID});
    dstNodeIDVector->setState(anchorState);
    relIDVector->setState(anchorState);
    const auto deleteState = std::make_unique<RelTableDeleteState>(
        *deletionRecord.ownedSrcNodeIDVector, *dstNodeIDVector, *relIDVector);
    deleteState->detachDeleteDirection = deletionRecord.direction;
    table.detachDelete(transaction::Transaction::Get(clientContext), deleteState.get());
}

void WALReplayer::replayRelUpdateRecord(const WALRecord& walRecord) const {
    const auto& updateRecord = walRecord.constCast<RelUpdateRecord>();
    const auto tableID = updateRecord.tableID;
    auto& table = StorageManager::Get(clientContext)->getTable(tableID)->cast<RelTable>();
    const auto anchorState = updateRecord.ownedRelIDVector->state;
    KU_ASSERT(anchorState == updateRecord.ownedSrcNodeIDVector->state &&
              anchorState == updateRecord.ownedSrcNodeIDVector->state &&
              anchorState == updateRecord.ownedPropertyVector->state);
    KU_ASSERT(anchorState->getSelVector().getSelSize() == 1);
    const auto updateState = std::make_unique<RelTableUpdateState>(updateRecord.columnID,
        *updateRecord.ownedSrcNodeIDVector, *updateRecord.ownedDstNodeIDVector,
        *updateRecord.ownedRelIDVector, *updateRecord.ownedPropertyVector);
    KU_ASSERT(transaction::Transaction::Get(clientContext) &&
              transaction::Transaction::Get(clientContext)->isRecovery());
    table.update(transaction::Transaction::Get(clientContext), *updateState);
}

void WALReplayer::replayCopyTableRecord(const WALRecord&) const {
    // DO NOTHING.
}

void WALReplayer::replayUpdateSequenceRecord(const WALRecord& walRecord) const {
    auto& sequenceEntryRecord = walRecord.constCast<UpdateSequenceRecord>();
    const auto sequenceID = sequenceEntryRecord.sequenceID;
    const auto entry =
        Catalog::Get(clientContext)
            ->getSequenceEntry(transaction::Transaction::Get(clientContext), sequenceID);
    entry->nextKVal(transaction::Transaction::Get(clientContext), sequenceEntryRecord.kCount);
}

void WALReplayer::replayLoadExtensionRecord(const WALRecord& walRecord) const {
    const auto& loadExtensionRecord = walRecord.constCast<LoadExtensionRecord>();
    extension::ExtensionManager::Get(clientContext)
        ->loadExtension(loadExtensionRecord.path, &clientContext);
}

void WALReplayer::removeWALAndShadowFiles() const {
    removeFileIfExists(shadowFilePath);
    removeFileIfExists(walPath);
}

void WALReplayer::removeFileIfExists(const std::string& path) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    auto vfs = VirtualFileSystem::GetUnsafe(clientContext);
    if (vfs->fileOrPathExists(path, &clientContext)) {
        vfs->removeFileIfExists(path);
    }
}

std::unique_ptr<FileInfo> WALReplayer::openWALFile() const {
    auto flag = FileFlags::READ_ONLY;
    if (!StorageManager::Get(clientContext)->isReadOnly()) {
        flag |= FileFlags::WRITE; // The write flag here is to ensure the file is opened with O_RDWR
                                  // so that we can sync it.
    }
    return VirtualFileSystem::GetUnsafe(clientContext)->openFile(walPath, FileOpenFlags(flag));
}

void WALReplayer::syncWALFile(const FileInfo& fileInfo) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    fileInfo.syncFile();
}

void WALReplayer::truncateWALFile(FileInfo& fileInfo, uint64_t size) const {
    if (StorageManager::Get(clientContext)->isReadOnly()) {
        return;
    }
    if (fileInfo.getFileSize() > size) {
        fileInfo.truncate(size);
        fileInfo.syncFile();
    }
}

} // namespace storage
} // namespace lbug
