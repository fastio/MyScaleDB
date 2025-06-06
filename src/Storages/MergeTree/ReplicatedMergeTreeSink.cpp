#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/MergeTree/ReplicatedMergeTreeQuorumEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeSink.h>
#include <Interpreters/PartLog.h>
#include <Common/ProfileEventsScope.h>
#include <Common/SipHash.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/ThreadFuzzer.h>
#include <Storages/MergeTree/AsyncBlockIDsCache.h>
#include <DataTypes/ObjectUtils.h>
#include <Core/Block.h>
#include <IO/Operators.h>
#include <fmt/core.h>

namespace ProfileEvents
{
    extern const Event DuplicatedInsertedBlocks;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_FEW_LIVE_REPLICAS;
    extern const int UNSATISFIED_QUORUM_FOR_PREVIOUS_WRITE;
    extern const int UNEXPECTED_ZOOKEEPER_ERROR;
    extern const int NO_ZOOKEEPER;
    extern const int READONLY;
    extern const int UNKNOWN_STATUS_OF_INSERT;
    extern const int INSERT_WAS_DEDUPLICATED;
    extern const int TIMEOUT_EXCEEDED;
    extern const int NO_ACTIVE_REPLICAS;
    extern const int DUPLICATE_DATA_PART;
    extern const int PART_IS_TEMPORARILY_LOCKED;
    extern const int LOGICAL_ERROR;
    extern const int TABLE_IS_READ_ONLY;
    extern const int QUERY_WAS_CANCELLED;
}

template<bool async_insert>
struct ReplicatedMergeTreeSinkImpl<async_insert>::DelayedChunk
{
    struct Partition
    {
        Poco::Logger * log;
        MergeTreeDataWriter::TemporaryPart temp_part;
        UInt64 elapsed_ns;
        BlockIDsType block_id;
        BlockWithPartition block_with_partition;
        std::unordered_map<String, std::vector<size_t>> block_id_to_offset_idx;
        ProfileEvents::Counters part_counters;

        Partition() = default;
        Partition(Poco::Logger * log_,
                  MergeTreeDataWriter::TemporaryPart && temp_part_,
                  UInt64 elapsed_ns_,
                  BlockIDsType && block_id_,
                  BlockWithPartition && block_,
                  ProfileEvents::Counters && part_counters_)
            : log(log_),
              temp_part(std::move(temp_part_)),
              elapsed_ns(elapsed_ns_),
              block_id(std::move(block_id_)),
              block_with_partition(std::move(block_)),
              part_counters(std::move(part_counters_))
        {
                initBlockIDMap();
        }

        void initBlockIDMap()
        {
            if constexpr (async_insert)
            {
                block_id_to_offset_idx.clear();
                for (size_t i = 0; i < block_id.size(); ++i)
                {
                    block_id_to_offset_idx[block_id[i]].push_back(i);
                }
            }
        }

        /// this function check if the block contains duplicate inserts.
        /// if so, we keep only one insert for every duplicate ones.
        bool filterSelfDuplicate()
        {
            if constexpr (async_insert)
            {
                std::vector<String> dup_block_ids;
                for (const auto & [hash_id, offset_indexes] : block_id_to_offset_idx)
                {
                    /// It means more than one inserts have the same hash id, in this case, we should keep only one of them.
                    if (offset_indexes.size() > 1)
                        dup_block_ids.push_back(hash_id);
                }
                if (dup_block_ids.empty())
                    return false;

                filterBlockDuplicate(dup_block_ids, true);
                return true;
            }
            return false;
        }

        /// remove the conflict parts of block for rewriting again.
        void filterBlockDuplicate(const std::vector<String> & block_paths, bool self_dedup)
        {
            if constexpr (async_insert)
            {
                std::vector<size_t> offset_idx;
                for (const auto & raw_path : block_paths)
                {
                    std::filesystem::path p(raw_path);
                    String conflict_block_id = p.filename();
                    auto it = block_id_to_offset_idx.find(conflict_block_id);
                    if (it == block_id_to_offset_idx.end())
                        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown conflict path {}", conflict_block_id);
                    /// if this filter is for self_dedup, that means the block paths is selected by `filterSelfDuplicate`, which is a self purge.
                    /// in this case, we don't know if zk has this insert, then we should keep one insert, to avoid missing this insert.
                    offset_idx.insert(std::end(offset_idx), std::begin(it->second) + self_dedup, std::end(it->second));
                }
                std::sort(offset_idx.begin(), offset_idx.end());

                auto & offsets = block_with_partition.offsets;
                size_t idx = 0, remove_count = 0;
                auto it = offset_idx.begin();
                std::vector<size_t> new_offsets;
                std::vector<String> new_block_ids;

                /// construct filter
                size_t rows = block_with_partition.block.rows();
                auto filter_col = ColumnUInt8::create(rows, 1u);
                ColumnUInt8::Container & vec = filter_col->getData();
                UInt8 * pos = vec.data();
                for (auto & offset : offsets)
                {
                    if (it != offset_idx.end() && *it == idx)
                    {
                        size_t start_pos = idx > 0 ? offsets[idx - 1] : 0;
                        size_t end_pos = offset;
                        remove_count += end_pos - start_pos;
                        while (start_pos < end_pos)
                        {
                            *(pos + start_pos) = 0;
                            start_pos++;
                        }
                        it++;
                    }
                    else
                    {
                        new_offsets.push_back(offset - remove_count);
                        new_block_ids.push_back(block_id[idx]);
                    }
                    idx++;
                }

                LOG_TRACE(log, "New block IDs: {}, new offsets: {}, size: {}", toString(new_block_ids), toString(new_offsets), new_offsets.size());

                block_with_partition.offsets = std::move(new_offsets);
                block_id = std::move(new_block_ids);
                auto cols = block_with_partition.block.getColumns();
                for (auto & col : cols)
                {
                    col = col->filter(vec, rows - remove_count);
                }
                block_with_partition.block.setColumns(cols);

                LOG_TRACE(log, "New block rows {}", block_with_partition.block.rows());

                initBlockIDMap();
            }
            else
            {
                throw Exception(ErrorCodes::LOGICAL_ERROR, "sync insert should not call rewriteBlock");
            }
        }
    };

    DelayedChunk() = default;
    explicit DelayedChunk(size_t replicas_num_) : replicas_num(replicas_num_) {}

    size_t replicas_num = 0;

    std::vector<Partition> partitions;
};

std::vector<Int64> testSelfDeduplicate(std::vector<Int64> data, std::vector<size_t> offsets, std::vector<String> hashes)
{
    MutableColumnPtr column = DataTypeInt64().createColumn();
    for (auto datum : data)
    {
        column->insert(datum);
    }
    Block block({ColumnWithTypeAndName(std::move(column), DataTypePtr(new DataTypeInt64()), "a")});

    BlockWithPartition block1(std::move(block), Row(), std::move(offsets));
    ProfileEvents::Counters profile_counters;
    ReplicatedMergeTreeSinkImpl<true>::DelayedChunk::Partition part(
        &Poco::Logger::get("testSelfDeduplicate"), MergeTreeDataWriter::TemporaryPart(), 0, std::move(hashes), std::move(block1), std::move(profile_counters));

    part.filterSelfDuplicate();

    ColumnPtr col = part.block_with_partition.block.getColumns()[0];
    std::vector<Int64> result;
    for (size_t i = 0; i < col->size(); i++)
    {
        result.push_back(col->getInt(i));
    }
    return result;
}

namespace
{
    /// Convert block id vector to string. Output at most 50 ids.
    template<typename T>
    inline String toString(const std::vector<T> & vec)
    {
        size_t size = vec.size();
        if (size > 50) size = 50;
        return fmt::format("({})", fmt::join(vec.begin(), vec.begin() + size, ","));
    }

    std::vector<String> getHashesForBlocks(BlockWithPartition & block, String partition_id)
    {
        size_t start = 0;
        auto cols = block.block.getColumns();
        std::vector<String> block_id_vec;
        for (auto offset : block.offsets)
        {
            SipHash hash;
            for (size_t i = start; i < offset; ++i)
                for (const auto & col : cols)
                    col->updateHashWithValue(i, hash);
            union
            {
                char bytes[16];
                UInt64 words[2];
            } hash_value;
            hash.get128(hash_value.bytes);

            block_id_vec.push_back(partition_id + "_" + DB::toString(hash_value.words[0]) + "_" + DB::toString(hash_value.words[1]));

            start = offset;
        }
        return block_id_vec;
    }
}

template<bool async_insert>
ReplicatedMergeTreeSinkImpl<async_insert>::ReplicatedMergeTreeSinkImpl(
    StorageReplicatedMergeTree & storage_,
    const StorageMetadataPtr & metadata_snapshot_,
    size_t quorum_size,
    size_t quorum_timeout_ms_,
    size_t max_parts_per_block_,
    bool quorum_parallel_,
    bool deduplicate_,
    bool majority_quorum,
    ContextPtr context_,
    bool is_attach_)
    : SinkToStorage(metadata_snapshot_->getSampleBlock())
    , storage(storage_)
    , metadata_snapshot(metadata_snapshot_)
    , required_quorum_size(majority_quorum ? std::nullopt : std::make_optional<size_t>(quorum_size))
    , quorum_timeout_ms(quorum_timeout_ms_)
    , max_parts_per_block(max_parts_per_block_)
    , is_attach(is_attach_)
    , quorum_parallel(quorum_parallel_)
    , deduplicate(deduplicate_)
    , log(&Poco::Logger::get(storage.getLogName() + " (Replicated OutputStream)"))
    , context(context_)
    , storage_snapshot(storage.getStorageSnapshotWithoutData(metadata_snapshot, context_))
{
    /// The quorum value `1` has the same meaning as if it is disabled.
    if (required_quorum_size == 1)
        required_quorum_size = 0;
}

template<bool async_insert>
ReplicatedMergeTreeSinkImpl<async_insert>::~ReplicatedMergeTreeSinkImpl() = default;

/// Allow to verify that the session in ZooKeeper is still alive.
static void assertSessionIsNotExpired(const zkutil::ZooKeeperPtr & zookeeper)
{
    if (!zookeeper)
        throw Exception(ErrorCodes::NO_ZOOKEEPER, "No ZooKeeper session.");

    if (zookeeper->expired())
        throw Exception(ErrorCodes::NO_ZOOKEEPER, "ZooKeeper session has been expired.");
}

template<bool async_insert>
size_t ReplicatedMergeTreeSinkImpl<async_insert>::checkQuorumPrecondition(const ZooKeeperWithFaultInjectionPtr & zookeeper)
{
    if (!isQuorumEnabled())
        return 0;

    quorum_info.status_path = storage.zookeeper_path + "/quorum/status";

    Strings replicas = zookeeper->getChildren(fs::path(storage.zookeeper_path) / "replicas");

    Strings exists_paths;
    exists_paths.reserve(replicas.size());
    for (const auto & replica : replicas)
        if (replica != storage.replica_name)
            exists_paths.emplace_back(fs::path(storage.zookeeper_path) / "replicas" / replica / "is_active");

    auto exists_result = zookeeper->exists(exists_paths);
    auto get_results = zookeeper->get(Strings{storage.replica_path + "/is_active", storage.replica_path + "/host"});

    Coordination::Error keeper_error = Coordination::Error::ZOK;
    size_t active_replicas = 1;     /// Assume current replica is active (will check below)
    for (size_t i = 0; i < exists_paths.size(); ++i)
    {
        auto error = exists_result[i].error;
        if (error == Coordination::Error::ZOK)
            ++active_replicas;
        else if (Coordination::isHardwareError(error))
            keeper_error = error;
    }

    size_t replicas_number = replicas.size();
    size_t quorum_size = getQuorumSize(replicas_number);

    if (active_replicas < quorum_size)
    {
        if (Coordination::isHardwareError(keeper_error))
            throw Coordination::Exception("Failed to check number of alive replicas", keeper_error);

        throw Exception(ErrorCodes::TOO_FEW_LIVE_REPLICAS, "Number of alive replicas ({}) is less than requested quorum ({}/{}).",
                        active_replicas, quorum_size, replicas_number);
    }

    /** Is there a quorum for the last part for which a quorum is needed?
        * Write of all the parts with the included quorum is linearly ordered.
        * This means that at any time there can be only one part,
        *  for which you need, but not yet reach the quorum.
        * Information about this part will be located in `/quorum/status` node.
        * If the quorum is reached, then the node is deleted.
        */

    String quorum_status;
    if (!quorum_parallel && zookeeper->tryGet(quorum_info.status_path, quorum_status))
        throw Exception(ErrorCodes::UNSATISFIED_QUORUM_FOR_PREVIOUS_WRITE,
                        "Quorum for previous write has not been satisfied yet. Status: {}", quorum_status);

    /// Both checks are implicitly made also later (otherwise there would be a race condition).

    auto is_active = get_results[0];
    auto host = get_results[1];

    if (is_active.error == Coordination::Error::ZNONODE || host.error == Coordination::Error::ZNONODE)
        throw Exception(ErrorCodes::READONLY, "Replica is not active right now");

    quorum_info.is_active_node_version = is_active.stat.version;
    quorum_info.host_node_version = host.stat.version;

    return replicas_number;
}

template<bool async_insert>
void ReplicatedMergeTreeSinkImpl<async_insert>::consume(Chunk chunk)
{
    auto block = getHeader().cloneWithColumns(chunk.detachColumns());

    const auto & settings = context->getSettingsRef();
    zookeeper_retries_info = ZooKeeperRetriesInfo(
        "ReplicatedMergeTreeSink::consume",
        settings.insert_keeper_max_retries ? log : nullptr,
        settings.insert_keeper_max_retries,
        settings.insert_keeper_retry_initial_backoff_ms,
        settings.insert_keeper_retry_max_backoff_ms);

    ZooKeeperWithFaultInjectionPtr zookeeper = ZooKeeperWithFaultInjection::createInstance(
        settings.insert_keeper_fault_injection_probability,
        settings.insert_keeper_fault_injection_seed,
        storage.getZooKeeper(),
        "ReplicatedMergeTreeSink::consume",
        log);

    /** If write is with quorum, then we check that the required number of replicas is now live,
      *  and also that for all previous parts for which quorum is required, this quorum is reached.
      * And also check that during the insertion, the replica was not reinitialized or disabled (by the value of `is_active` node).
      * TODO Too complex logic, you can do better.
      */
    size_t replicas_num = 0;
    ZooKeeperRetriesControl quorum_retries_ctl("checkQuorumPrecondition", zookeeper_retries_info);
    quorum_retries_ctl.retryLoop(
        [&]()
        {
            zookeeper->setKeeper(storage.getZooKeeper());
            replicas_num = checkQuorumPrecondition(zookeeper);
        });

    if (!storage_snapshot->object_columns.empty())
        convertDynamicColumnsToTuples(block, storage_snapshot);


    ChunkOffsetsPtr chunk_offsets;

    if constexpr (async_insert)
    {
        const auto & chunk_info = chunk.getChunkInfo();
        if (const auto * chunk_offsets_ptr = typeid_cast<const ChunkOffsets *>(chunk_info.get()))
            chunk_offsets = std::make_shared<ChunkOffsets>(chunk_offsets_ptr->offsets);
        else
            throw Exception(ErrorCodes::LOGICAL_ERROR, "No chunk info for async inserts");
    }

    auto part_blocks = storage.writer.splitBlockIntoParts(block, max_parts_per_block, metadata_snapshot, context, chunk_offsets);

    using DelayedPartition = typename ReplicatedMergeTreeSinkImpl<async_insert>::DelayedChunk::Partition;
    using DelayedPartitions = std::vector<DelayedPartition>;
    DelayedPartitions partitions;

    size_t streams = 0;
    bool support_parallel_write = false;

    for (auto & current_block : part_blocks)
    {
        Stopwatch watch;

        ProfileEvents::Counters part_counters;
        auto profile_events_scope = std::make_unique<ProfileEventsScope>(&part_counters);

        /// Write part to the filesystem under temporary name. Calculate a checksum.

        auto temp_part = storage.writer.writeTempPart(current_block, metadata_snapshot, context);

        /// If optimize_on_insert setting is true, current_block could become empty after merge
        /// and we didn't create part.
        if (!temp_part.part)
            continue;

        BlockIDsType block_id;

        if constexpr (async_insert)
        {
            /// TODO consider insert_deduplication_token
            block_id = getHashesForBlocks(current_block, temp_part.part->info.partition_id);
            LOG_TRACE(log, "async insert part, part id {}, block id {}, offsets {}, size {}", temp_part.part->info.partition_id, toString(block_id), toString(current_block.offsets), current_block.offsets.size());
        }
        else if (deduplicate)
        {
            String block_dedup_token;

            /// We add the hash from the data and partition identifier to deduplication ID.
            /// That is, do not insert the same data to the same partition twice.

            const String & dedup_token = settings.insert_deduplication_token;
            if (!dedup_token.empty())
            {
                /// multiple blocks can be inserted within the same insert query
                /// an ordinal number is added to dedup token to generate a distinctive block id for each block
                block_dedup_token = fmt::format("{}_{}", dedup_token, chunk_dedup_seqnum);
                ++chunk_dedup_seqnum;
            }

            block_id = temp_part.part->getZeroLevelPartBlockID(block_dedup_token);
            LOG_DEBUG(log, "Wrote block with ID '{}', {} rows{}", block_id, current_block.block.rows(), quorumLogMessage(replicas_num));
        }
        else
        {
            LOG_DEBUG(log, "Wrote block with {} rows{}", current_block.block.rows(), quorumLogMessage(replicas_num));
        }

        profile_events_scope.reset();
        UInt64 elapsed_ns = watch.elapsed();

        size_t max_insert_delayed_streams_for_parallel_write = DEFAULT_DELAYED_STREAMS_FOR_PARALLEL_WRITE;
        if (!support_parallel_write || settings.max_insert_delayed_streams_for_parallel_write.changed)
            max_insert_delayed_streams_for_parallel_write = settings.max_insert_delayed_streams_for_parallel_write;

        /// In case of too much columns/parts in block, flush explicitly.
        streams += temp_part.streams.size();
        if (streams > max_insert_delayed_streams_for_parallel_write)
        {
            finishDelayedChunk(zookeeper);
            delayed_chunk = std::make_unique<ReplicatedMergeTreeSinkImpl<async_insert>::DelayedChunk>(replicas_num);
            delayed_chunk->partitions = std::move(partitions);
            finishDelayedChunk(zookeeper);

            streams = 0;
            support_parallel_write = false;
            partitions = DelayedPartitions{};
        }


        partitions.emplace_back(DelayedPartition(
            log,
            std::move(temp_part),
            elapsed_ns,
            std::move(block_id),
            std::move(current_block),
            std::move(part_counters) /// profile_events_scope must be reset here.
        ));
    }

    finishDelayedChunk(zookeeper);
    delayed_chunk = std::make_unique<ReplicatedMergeTreeSinkImpl::DelayedChunk>();
    delayed_chunk->partitions = std::move(partitions);

    /// If deduplicated data should not be inserted into MV, we need to set proper
    /// value for `last_block_is_duplicate`, which is possible only after the part is committed.
    /// Othervide we can delay commit.
    /// TODO: we can also delay commit if there is no MVs.
    if (!settings.deduplicate_blocks_in_dependent_materialized_views)
        finishDelayedChunk(zookeeper);
}

template<>
void ReplicatedMergeTreeSinkImpl<false>::finishDelayedChunk(const ZooKeeperWithFaultInjectionPtr & zookeeper)
{
    if (!delayed_chunk)
        return;

    last_block_is_duplicate = false;

    for (auto & partition : delayed_chunk->partitions)
    {
        ProfileEventsScope scoped_attach(&partition.part_counters);

        partition.temp_part.finalize();

        auto & part = partition.temp_part.part;

        try
        {
            commitPart(zookeeper, part, partition.block_id, delayed_chunk->replicas_num, false);

            /// init vector index
            for (auto & vec_desc : metadata_snapshot->getVectorIndices())
                part->vector_index.addVectorIndex(vec_desc);

            last_block_is_duplicate = last_block_is_duplicate || part->is_duplicate;

            /// Set a special error code if the block is duplicate
            int error = (deduplicate && part->is_duplicate) ? ErrorCodes::INSERT_WAS_DEDUPLICATED : 0;
            auto counters_snapshot = std::make_shared<ProfileEvents::Counters::Snapshot>(partition.part_counters.getPartiallyAtomicSnapshot());
            PartLog::addNewPart(storage.getContext(), PartLog::PartLogEntry(part, partition.elapsed_ns, counters_snapshot), ExecutionStatus(error));
            storage.incrementInsertedPartsProfileEvent(part->getType());
        }
        catch (...)
        {
            auto counters_snapshot = std::make_shared<ProfileEvents::Counters::Snapshot>(partition.part_counters.getPartiallyAtomicSnapshot());
            PartLog::addNewPart(storage.getContext(), PartLog::PartLogEntry(part, partition.elapsed_ns, counters_snapshot), ExecutionStatus::fromCurrentException("", true));
            throw;
        }
    }

    delayed_chunk.reset();
}

template<>
void ReplicatedMergeTreeSinkImpl<true>::finishDelayedChunk(const ZooKeeperWithFaultInjectionPtr & zookeeper)
{
    if (!delayed_chunk)
        return;

    for (auto & partition: delayed_chunk->partitions)
    {
        int retry_times = 0;
        /// users may have lots of same inserts. It will be helpful to deduplicate in advance.
        if (partition.filterSelfDuplicate())
        {
            LOG_TRACE(log, "found duplicated inserts in the block");
            partition.block_with_partition.partition = std::move(partition.temp_part.part->partition.value);
            partition.temp_part = storage.writer.writeTempPart(partition.block_with_partition, metadata_snapshot, context);
        }

        /// reset the cache version to zero for every partition write.
        cache_version = 0;
        while (true)
        {
            partition.temp_part.finalize();
            auto conflict_block_ids = commitPart(zookeeper, partition.temp_part.part, partition.block_id, delayed_chunk->replicas_num, false);
            if (conflict_block_ids.empty())
                break;
            ++retry_times;
            LOG_DEBUG(log, "Found duplicate block IDs: {}, retry times {}", toString(conflict_block_ids), retry_times);
            /// partition clean conflict
            partition.filterBlockDuplicate(conflict_block_ids, false);
            if (partition.block_id.empty())
                break;
            partition.block_with_partition.partition = std::move(partition.temp_part.part->partition.value);
            partition.temp_part = storage.writer.writeTempPart(partition.block_with_partition, metadata_snapshot, context);
        }
    }

    delayed_chunk.reset();
}

template<bool async_insert>
void ReplicatedMergeTreeSinkImpl<async_insert>::writeExistingPart(MergeTreeData::MutableDataPartPtr & part)
{
    /// NOTE: No delay in this case. That's Ok.

    auto origin_zookeeper = storage.getZooKeeper();
    assertSessionIsNotExpired(origin_zookeeper);
    auto zookeeper = std::make_shared<ZooKeeperWithFaultInjection>(origin_zookeeper);

    size_t replicas_num = checkQuorumPrecondition(zookeeper);

    Stopwatch watch;
    ProfileEventsScope profile_events_scope;

    try
    {
        part->version.setCreationTID(Tx::PrehistoricTID, nullptr);
        commitPart(zookeeper, part, BlockIDsType(), replicas_num, true);
        PartLog::addNewPart(storage.getContext(), PartLog::PartLogEntry(part, watch.elapsed(), profile_events_scope.getSnapshot()));
    }
    catch (...)
    {
        PartLog::addNewPart(storage.getContext(), PartLog::PartLogEntry(part, watch.elapsed(), profile_events_scope.getSnapshot()), ExecutionStatus::fromCurrentException("", true));
        throw;
    }
}

template<bool async_insert>
std::vector<String> ReplicatedMergeTreeSinkImpl<async_insert>::commitPart(
    const ZooKeeperWithFaultInjectionPtr & zookeeper,
    MergeTreeData::MutableDataPartPtr & part,
    const BlockIDsType & block_id,
    size_t replicas_num,
    bool writing_existing_part)
{
    /// It is possible that we alter a part with different types of source columns.
    /// In this case, if column was not altered, the result type will be different with what we have in metadata.
    /// For now, consider it is ok. See 02461_alter_update_respect_part_column_type_bug for an example.
    ///
    /// metadata_snapshot->check(part->getColumns());

    const String temporary_part_relative_path = part->getDataPartStorage().getPartDirectory();

    /// There is one case when we need to retry transaction in a loop.
    /// But don't do it too many times - just as defensive measure.
    size_t loop_counter = 0;
    constexpr size_t max_iterations = 10;

    bool is_already_existing_part = false;

    /// for retries due to keeper error
    bool part_committed_locally_but_zookeeper = false;
    Coordination::Error write_part_info_keeper_error = Coordination::Error::ZOK;
    std::vector<String> conflict_block_ids;

    ZooKeeperRetriesControl retries_ctl("commitPart", zookeeper_retries_info);
    retries_ctl.retryLoop([&]()
    {
        zookeeper->setKeeper(storage.getZooKeeper());
        if (storage.is_readonly)
        {
            /// stop retries if in shutdown
            if (storage.shutdown_called)
                throw Exception(
                    ErrorCodes::TABLE_IS_READ_ONLY, "Table is in readonly mode due to shutdown: replica_path={}", storage.replica_path);

            /// When we attach existing parts it's okay to be in read-only mode
            /// For example during RESTORE REPLICA.
            if (!writing_existing_part)
            {
                retries_ctl.setUserError(ErrorCodes::TABLE_IS_READ_ONLY, "Table is in readonly mode: replica_path={}", storage.replica_path);
                return;
            }
        }

        if (retries_ctl.isRetry())
        {
            /// If we are retrying, check if last iteration was actually successful,
            /// we could get network error on committing part to zk
            /// but the operation could be completed by zk server

            /// If this flag is true, then part is in Active state, and we'll not retry anymore
            /// we only check if part was committed to zk and return success or failure correspondingly
            /// Note: if commit to zk failed then cleanup thread will mark the part as Outdated later
            if (part_committed_locally_but_zookeeper)
            {
                /// check that info about the part was actually written in zk
                if (zookeeper->exists(fs::path(storage.replica_path) / "parts" / part->name))
                {
                    LOG_DEBUG(log, "Part was successfully committed on previous iteration: part_id={}", part->name);
                }
                else
                {
                    retries_ctl.setUserError(
                        ErrorCodes::UNEXPECTED_ZOOKEEPER_ERROR,
                        "Insert failed due to zookeeper error. Please retry. Reason: {}",
                        Coordination::errorMessage(write_part_info_keeper_error));
                }

                retries_ctl.stopRetries();
                return;
            }
        }

        /// Obtain incremental block number and lock it. The lock holds our intention to add the block to the filesystem.
        /// We remove the lock just after renaming the part. In case of exception, block number will be marked as abandoned.
        /// Also, make deduplication check. If a duplicate is detected, no nodes are created.

        /// Allocate new block number and check for duplicates
        bool deduplicate_block = !block_id.empty();
        BlockIDsType block_id_path ;
        if constexpr (async_insert)
        {
            /// prefilter by cache
            conflict_block_ids = storage.async_block_ids_cache.detectConflicts(block_id, cache_version);
            if (!conflict_block_ids.empty())
            {
                cache_version = 0;
                return;
            }
            for (const auto & single_block_id : block_id)
                block_id_path.push_back(storage.zookeeper_path + "/async_blocks/" + single_block_id);
        }
        else if (deduplicate_block)
            block_id_path = storage.zookeeper_path + "/blocks/" + block_id;
        auto block_number_lock = storage.allocateBlockNumber(part->info.partition_id, zookeeper, block_id_path);
        ThreadFuzzer::maybeInjectSleep();

        /// Prepare transaction to ZooKeeper
        /// It will simultaneously add information about the part to all the necessary places in ZooKeeper and remove block_number_lock.
        Coordination::Requests ops;

        Int64 block_number = 0;
        size_t block_unlock_op_idx = std::numeric_limits<size_t>::max();
        String existing_part_name;
        if (block_number_lock)
        {
            if constexpr (async_insert)
            {
                /// The truth is that we always get only one path from block_number_lock.
                /// This is a restriction of Keeper. Here I would like to use vector because
                /// I wanna keep extensibility for future optimization, for instance, using
                /// cache to resolve conflicts in advance.
                String conflict_path = block_number_lock->getConflictPath();
                if (!conflict_path.empty())
                {
                    LOG_TRACE(log, "Cannot get lock, the conflict path is {}", conflict_path);
                    conflict_block_ids.push_back(conflict_path);
                    return;
                }
            }
            is_already_existing_part = false;
            block_number = block_number_lock->getNumber();

            /// Set part attributes according to part_number. Prepare an entry for log.

            part->info.min_block = block_number;
            part->info.max_block = block_number;
            part->info.level = 0;
            part->info.mutation = 0;

            part->name = part->getNewName(part->info);

            StorageReplicatedMergeTree::LogEntry log_entry;

            if (is_attach)
            {
                log_entry.type = StorageReplicatedMergeTree::LogEntry::ATTACH_PART;

                /// We don't need to involve ZooKeeper to obtain checksums as by the time we get
                /// MutableDataPartPtr here, we already have the data thus being able to
                /// calculate the checksums.
                log_entry.part_checksum = part->checksums.getTotalChecksumHex();
            }
            else
                log_entry.type = StorageReplicatedMergeTree::LogEntry::GET_PART;

            log_entry.create_time = time(nullptr);
            log_entry.source_replica = storage.replica_name;
            log_entry.new_part_name = part->name;
            /// TODO maybe add UUID here as well?
            log_entry.quorum = getQuorumSize(replicas_num);
            log_entry.new_part_format = part->getFormat();

            if constexpr (!async_insert)
                log_entry.block_id = block_id;

            ops.emplace_back(zkutil::makeCreateRequest(
                storage.zookeeper_path + "/log/log-",
                log_entry.toString(),
                zkutil::CreateMode::PersistentSequential));

            /// Deletes the information that the block number is used for writing.
            block_unlock_op_idx = ops.size();
            block_number_lock->getUnlockOp(ops);

            /** If we need a quorum - create a node in which the quorum is monitored.
              * (If such a node already exists, then someone has managed to make another quorum record at the same time,
              *  but for it the quorum has not yet been reached.
              *  You can not do the next quorum record at this time.)
              */
            if (isQuorumEnabled())
            {
                ReplicatedMergeTreeQuorumEntry quorum_entry;
                quorum_entry.part_name = part->name;
                quorum_entry.required_number_of_replicas = getQuorumSize(replicas_num);
                quorum_entry.replicas.insert(storage.replica_name);

                /** At this point, this node will contain information that the current replica received a part.
                    * When other replicas will receive this part (in the usual way, processing the replication log),
                    *  they will add themselves to the contents of this node.
                    * When it contains information about `quorum` number of replicas, this node is deleted,
                    *  which indicates that the quorum has been reached.
                    */

                if (quorum_parallel)
                    quorum_info.status_path = storage.zookeeper_path + "/quorum/parallel/" + part->name;

                ops.emplace_back(
                    zkutil::makeCreateRequest(
                        quorum_info.status_path,
                        quorum_entry.toString(),
                        zkutil::CreateMode::Persistent));

                /// Make sure that during the insertion time, the replica was not reinitialized or disabled (when the server is finished).
                ops.emplace_back(
                    zkutil::makeCheckRequest(
                        storage.replica_path + "/is_active",
                        quorum_info.is_active_node_version));

                /// Unfortunately, just checking the above is not enough, because `is_active`
                /// node can be deleted and reappear with the same version.
                /// But then the `host` value will change. We will check this.
                /// It's great that these two nodes change in the same transaction (see MergeTreeRestartingThread).
                ops.emplace_back(
                    zkutil::makeCheckRequest(
                        storage.replica_path + "/host",
                        quorum_info.host_node_version));
            }
        }
        /// async_insert will never return null lock, because they need the conflict path.
        else if constexpr (!async_insert)
        {
            is_already_existing_part = true;

            /// This block was already written to some replica. Get the part name for it.
            /// Note: race condition with DROP PARTITION operation is possible. User will get "No node" exception and it is Ok.
            existing_part_name = zookeeper->get(storage.zookeeper_path + "/blocks/" + block_id);

            /// If it exists on our replica, ignore it.
            if (storage.getActiveContainingPart(existing_part_name))
            {
                part->is_duplicate = true;
                ProfileEvents::increment(ProfileEvents::DuplicatedInsertedBlocks);
                if (isQuorumEnabled())
                {
                    LOG_INFO(log, "Block with ID {} already exists locally as part {}; ignoring it, but checking quorum.", block_id, existing_part_name);

                    std::string quorum_path;
                    if (quorum_parallel)
                        quorum_path = storage.zookeeper_path + "/quorum/parallel/" + existing_part_name;
                    else
                        quorum_path = storage.zookeeper_path + "/quorum/status";

                    if (!retries_ctl.callAndCatchAll(
                            [&]()
                            {
                                waitForQuorum(
                                    zookeeper, existing_part_name, quorum_path, quorum_info.is_active_node_version, replicas_num);
                            }))
                        return;
                }
                else
                {
                    LOG_INFO(log, "Block with ID {} already exists locally as part {}; ignoring it.", block_id, existing_part_name);
                }

                return;
            }

            LOG_INFO(log, "Block with ID {} already exists on other replicas as part {}; will write it locally with that name.",
                block_id, existing_part_name);

            /// If it does not exist, we will write a new part with existing name.
            /// Note that it may also appear on filesystem right now in PreActive state due to concurrent inserts of the same data.
            /// It will be checked when we will try to rename directory.

            part->name = existing_part_name;
            part->info = MergeTreePartInfo::fromPartName(existing_part_name, storage.format_version);
            /// Used only for exception messages.
            block_number = part->info.min_block;

            /// Do not check for duplicate on commit to ZK.
            block_id_path.clear();
        }
        else
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                            "Conflict block ids and block number lock should not "
                            "be empty at the same time for async inserts");

        /// Information about the part.
        storage.getCommitPartOps(ops, part, block_id_path);

        /// It's important to create it outside of lock scope because
        /// otherwise it can lock parts in destructor and deadlock is possible.
        MergeTreeData::Transaction transaction(storage, NO_TRANSACTION_RAW); /// If you can not add a part to ZK, we'll remove it back from the working set.
        bool renamed = false;

        try
        {
            auto lock = storage.lockParts();
            renamed = storage.renameTempPartAndAdd(part, transaction, lock);
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::DUPLICATE_DATA_PART && e.code() != ErrorCodes::PART_IS_TEMPORARILY_LOCKED)
                throw;
        }

        if (!renamed)
        {
            if (is_already_existing_part)
            {
                LOG_INFO(log, "Part {} is duplicate and it is already written by concurrent request or fetched; ignoring it.", part->name);
                return;
            }
            else
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Part with name {} is already written by concurrent request."
                    " It should not happen for non-duplicate data parts because unique names are assigned for them. It's a bug",
                    part->name);
        }

        auto rename_part_to_temporary = [&temporary_part_relative_path, &transaction, &part]()
        {
            transaction.rollbackPartsToTemporaryState();

            part->is_temp = true;
            part->renameTo(temporary_part_relative_path, false);
        };

        try
        {
            ThreadFuzzer::maybeInjectSleep();
            storage.lockSharedData(*part, zookeeper, false, {});
            ThreadFuzzer::maybeInjectSleep();
        }
        catch (const Exception &)
        {
            rename_part_to_temporary();
            throw;
        }

        ThreadFuzzer::maybeInjectSleep();

        Coordination::Responses responses;
        Coordination::Error multi_code = zookeeper->tryMultiNoThrow(ops, responses); /// 1 RTT
        if (multi_code == Coordination::Error::ZOK)
        {
            transaction.commit();
            storage.merge_selecting_task->schedule();

            /// Lock nodes have been already deleted, do not delete them in destructor
            if (block_number_lock)
                block_number_lock->assumeUnlocked();
        }
        else if (multi_code == Coordination::Error::ZNONODE && zkutil::getFailedOpIndex(multi_code, responses) == block_unlock_op_idx)
        {
            throw Exception(ErrorCodes::QUERY_WAS_CANCELLED,
                            "Insert query (for block {}) was cancelled by concurrent ALTER PARTITION", block_number_lock->getPath());
        }
        else if (Coordination::isHardwareError(multi_code))
        {
            write_part_info_keeper_error = multi_code;
            /** If the connection is lost, and we do not know if the changes were applied, we can not delete the local part
             *  if the changes were applied, the inserted block appeared in `/blocks/`, and it can not be inserted again.
             */
            transaction.commit();

            /// Setting this flag is point of no return
            /// On next retry, we'll just check if actually operation succeed or failed
            /// and return ok or error correspondingly
            part_committed_locally_but_zookeeper = true;

            /// if all retries will be exhausted by accessing zookeeper on fresh retry -> we'll add committed part to queue in the action
            /// here lambda capture part name, it's ok since we'll not generate new one for this insert,
            /// see comments around 'part_committed_locally_but_zookeeper' flag
            retries_ctl.actionAfterLastFailedRetry(
                [&storage = storage, part_name = part->name]()
                { storage.enqueuePartForCheck(part_name, MAX_AGE_OF_LOCAL_PART_THAT_WASNT_ADDED_TO_ZOOKEEPER); });

            /// We do not know whether or not data has been inserted.
            retries_ctl.setUserError(
                ErrorCodes::UNKNOWN_STATUS_OF_INSERT,
                "Unknown status, client must retry. Reason: {}",
                Coordination::errorMessage(multi_code));
            return;
        }
        else if (Coordination::isUserError(multi_code))
        {
            String failed_op_path = ops[zkutil::getFailedOpIndex(multi_code, responses)]->getPath();

            auto contains = [](const auto & block_ids, const String & path)
            {
                if constexpr (async_insert)
                {
                    for (const auto & local_block_id : block_ids)
                        if (local_block_id == path)
                            return true;
                    return false;
                }
                else
                    return block_ids == path;
            };

            if (multi_code == Coordination::Error::ZNODEEXISTS && deduplicate_block && contains(block_id_path, failed_op_path))
            {
                /// Block with the same id have just appeared in table (or other replica), rollback the insertion.
                LOG_INFO(log, "Block with ID {} already exists (it was just appeared). Renaming part {} back to {}. Will retry write.",
                    toString(block_id), part->name, temporary_part_relative_path);

                /// We will try to add this part again on the new iteration as it's just a new part.
                /// So remove it from storage parts set immediately and transfer state to temporary.
                rename_part_to_temporary();

                if constexpr (async_insert)
                {
                    conflict_block_ids = std::vector<String>({failed_op_path});
                    LOG_TRACE(log, "conflict when committing, the conflict block ids are {}", toString(conflict_block_ids));
                    return;
                }

                /// If this part appeared on other replica than it's better to try to write it locally one more time. If it's our part
                /// than it will be ignored on the next iteration.
                ++loop_counter;
                if (loop_counter == max_iterations)
                {
                    part->is_duplicate = true; /// Part is duplicate, just remove it from local FS
                    throw Exception(ErrorCodes::DUPLICATE_DATA_PART, "Too many transaction retries - it may indicate an error");
                }
                retries_ctl.requestUnconditionalRetry(); /// we want one more iteration w/o counting it as a try and timeout
                return;
            }
            else if (multi_code == Coordination::Error::ZNODEEXISTS && failed_op_path == quorum_info.status_path)
            {
                try
                {
                    storage.unlockSharedData(*part, zookeeper);
                }
                catch (const zkutil::KeeperException & e)
                {
                    /// suppress this exception since need to rename part to temporary next
                    LOG_DEBUG(log, "Unlocking shared data failed during error handling: code={} message={}", e.code, e.message());
                }

                /// Part was not committed to keeper
                /// So make it temporary to avoid its resurrection on restart
                rename_part_to_temporary();

                throw Exception(ErrorCodes::UNSATISFIED_QUORUM_FOR_PREVIOUS_WRITE, "Another quorum insert has been already started");
            }
            else
            {
                storage.unlockSharedData(*part, zookeeper);
                /// NOTE: We could be here if the node with the quorum existed, but was quickly removed.
                transaction.rollback();
                throw Exception(
                    ErrorCodes::UNEXPECTED_ZOOKEEPER_ERROR,
                    "Unexpected logical error while adding block {} with ID '{}': {}, path {}",
                    block_number,
                    toString(block_id),
                    Coordination::errorMessage(multi_code),
                    failed_op_path);
            }
        }
        else
        {
            storage.unlockSharedData(*part, zookeeper);
            transaction.rollback();
            throw Exception(
                ErrorCodes::UNEXPECTED_ZOOKEEPER_ERROR,
                "Unexpected ZooKeeper error while adding block {} with ID '{}': {}",
                block_number,
                toString(block_id),
                Coordination::errorMessage(multi_code));
        }
    },
    [&zookeeper]() { zookeeper->cleanupEphemeralNodes(); });

    if (!conflict_block_ids.empty())
        return conflict_block_ids;

    if (isQuorumEnabled())
    {
        ZooKeeperRetriesControl quorum_retries_ctl("waitForQuorum", zookeeper_retries_info);
        quorum_retries_ctl.retryLoop([&]()
        {
            if (storage.is_readonly)
            {
                /// stop retries if in shutdown
                if (storage.shutdown_called)
                    throw Exception(
                        ErrorCodes::TABLE_IS_READ_ONLY, "Table is in readonly mode due to shutdown: replica_path={}", storage.replica_path);

                quorum_retries_ctl.setUserError(ErrorCodes::TABLE_IS_READ_ONLY, "Table is in readonly mode: replica_path={}", storage.replica_path);
                return;
            }

            zookeeper->setKeeper(storage.getZooKeeper());

            if (is_already_existing_part)
            {
                /// We get duplicate part without fetch
                /// Check if this quorum insert is parallel or not
                if (zookeeper->exists(storage.zookeeper_path + "/quorum/parallel/" + part->name))
                    storage.updateQuorum(part->name, true);
                else if (zookeeper->exists(storage.zookeeper_path + "/quorum/status"))
                    storage.updateQuorum(part->name, false);
            }

            if (!quorum_retries_ctl.callAndCatchAll(
                    [&]()
                    { waitForQuorum(zookeeper, part->name, quorum_info.status_path, quorum_info.is_active_node_version, replicas_num); }))
                return;
        });
    }
    return {};
}

template<bool async_insert>
void ReplicatedMergeTreeSinkImpl<async_insert>::onStart()
{
    /// Only check "too many parts" before write,
    /// because interrupting long-running INSERT query in the middle is not convenient for users.
    storage.delayInsertOrThrowIfNeeded(&storage.partial_shutdown_event, context);
}

template<bool async_insert>
void ReplicatedMergeTreeSinkImpl<async_insert>::onFinish()
{
    auto zookeeper = storage.getZooKeeper();
    assertSessionIsNotExpired(zookeeper);
    finishDelayedChunk(std::make_shared<ZooKeeperWithFaultInjection>(zookeeper));
}

template<bool async_insert>
void ReplicatedMergeTreeSinkImpl<async_insert>::waitForQuorum(
    const ZooKeeperWithFaultInjectionPtr & zookeeper,
    const std::string & part_name,
    const std::string & quorum_path,
    Int32 is_active_node_version,
    size_t replicas_num) const
{
    /// We are waiting for quorum to be satisfied.
    LOG_TRACE(log, "Waiting for quorum '{}' for part {}{}", quorum_path, part_name, quorumLogMessage(replicas_num));

    try
    {
        while (true)
        {
            zkutil::EventPtr event = std::make_shared<Poco::Event>();

            std::string value;
            /// `get` instead of `exists` so that `watch` does not leak if the node is no longer there.
            if (!zookeeper->tryGet(quorum_path, value, nullptr, event))
                break;

            LOG_TRACE(log, "Quorum node {} still exists, will wait for updates", quorum_path);

            ReplicatedMergeTreeQuorumEntry quorum_entry(value);

            /// If the node has time to disappear, and then appear again for the next insert.
            if (quorum_entry.part_name != part_name)
                break;

            if (!event->tryWait(quorum_timeout_ms))
                throw Exception(ErrorCodes::TIMEOUT_EXCEEDED, "Timeout while waiting for quorum");

            LOG_TRACE(log, "Quorum {} for part {} updated, will check quorum node still exists", quorum_path, part_name);
        }

        /// And what if it is possible that the current replica at this time has ceased to be active
        /// and the quorum is marked as failed and deleted?
        Coordination::Stat stat;
        String value;
        if (!zookeeper->tryGet(storage.replica_path + "/is_active", value, &stat)
            || stat.version != is_active_node_version)
            throw Exception(ErrorCodes::NO_ACTIVE_REPLICAS, "Replica become inactive while waiting for quorum");
    }
    catch (...)
    {
        /// We do not know whether or not data has been inserted
        /// - whether other replicas have time to download the part and mark the quorum as done.
        throw Exception(ErrorCodes::UNKNOWN_STATUS_OF_INSERT, "Unknown status, client must retry. Reason: {}",
            getCurrentExceptionMessage(false));
    }

    LOG_TRACE(log, "Quorum '{}' for part {} satisfied", quorum_path, part_name);
}

template<bool async_insert>
String ReplicatedMergeTreeSinkImpl<async_insert>::quorumLogMessage(size_t replicas_num) const
{
    if (!isQuorumEnabled())
        return "";
    return fmt::format(" (quorum {} of {} replicas)", getQuorumSize(replicas_num), replicas_num);
}

template<bool async_insert>
size_t ReplicatedMergeTreeSinkImpl<async_insert>::getQuorumSize(size_t replicas_num) const
{
    if (!isQuorumEnabled())
        return 0;

    if (required_quorum_size)
        return required_quorum_size.value();

    return replicas_num / 2 + 1;
}

template<bool async_insert>
bool ReplicatedMergeTreeSinkImpl<async_insert>::isQuorumEnabled() const
{
    return !required_quorum_size.has_value() || required_quorum_size.value() > 1;
}

template class ReplicatedMergeTreeSinkImpl<true>;
template class ReplicatedMergeTreeSinkImpl<false>;

}
