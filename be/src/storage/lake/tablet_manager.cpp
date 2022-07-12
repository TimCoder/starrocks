// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "storage/lake/tablet_manager.h"

#include <variant>

#include "fmt/format.h"
#include "fs/fs.h"
#include "fs/fs_util.h"
#include "gen_cpp/AgentService_types.h"
#include "gen_cpp/lake_types.pb.h"
#include "gutil/strings/util.h"
#include "storage/lake/horizontal_compaction_task.h"
#include "storage/lake/location_provider.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_metadata.h"
#include "storage/lake/txn_log.h"
#include "storage/metadata_util.h"
#include "storage/tablet_schema_map.h"
#include "util/lru_cache.h"
#include "util/raw_container.h"

namespace starrocks::lake {

Status apply_txn_log(const TxnLog& log, TabletMetadata* metadata);
Status publish(Tablet* tablet, int64_t base_version, int64_t new_version, const int64_t* txns, int txns_size);

TabletManager::TabletManager(LocationProvider* location_provider, int64_t cache_capacity)
        : _location_provider(location_provider), _metacache(new_lru_cache(cache_capacity)) {}

std::string TabletManager::tablet_root_location(int64_t tablet_id) const {
    return _location_provider->root_location(tablet_id);
}

std::string TabletManager::tablet_metadata_location(int64_t tablet_id, int64_t version) const {
    return _location_provider->tablet_metadata_location(tablet_id, version);
}

std::string TabletManager::txn_log_location(int64_t tablet_id, int64_t txn_id) const {
    return _location_provider->txn_log_location(tablet_id, txn_id);
}

std::string TabletManager::segment_location(int64_t tablet_id, std::string_view segment_name) const {
    return _location_provider->segment_location(tablet_id, segment_name);
}

std::string TabletManager::tablet_schema_cache_key(int64_t tablet_id) {
    return fmt::format("schema_{}", tablet_id);
}

bool TabletManager::fill_metacache(const std::string& key, CacheValue* ptr, int size) {
    Cache::Handle* handle = _metacache->insert(CacheKey(key), ptr, size, cache_value_deleter);
    if (handle == nullptr) {
        delete ptr;
        return false;
    } else {
        _metacache->release(handle);
        return true;
    }
}

TabletMetadataPtr TabletManager::lookup_tablet_metadata(const std::string& key) {
    auto handle = _metacache->lookup(CacheKey(key));
    if (handle == nullptr) {
        return nullptr;
    }
    auto value = static_cast<CacheValue*>(_metacache->value(handle));
    auto metadata = std::get<TabletMetadataPtr>(*value);
    _metacache->release(handle);
    return metadata;
}

TabletSchemaPtr TabletManager::lookup_tablet_schema(const std::string& key) {
    auto handle = _metacache->lookup(CacheKey(key));
    if (handle == nullptr) {
        return nullptr;
    }
    auto value = static_cast<CacheValue*>(_metacache->value(handle));
    auto schema = std::get<TabletSchemaPtr>(*value);
    _metacache->release(handle);
    return schema;
}

TxnLogPtr TabletManager::lookup_txn_log(const std::string& key) {
    auto handle = _metacache->lookup(CacheKey(key));
    if (handle == nullptr) {
        return nullptr;
    }
    auto value = static_cast<CacheValue*>(_metacache->value(handle));
    auto log = std::get<TxnLogPtr>(*value);
    _metacache->release(handle);
    return log;
}

void TabletManager::erase_metacache(const std::string& key) {
    _metacache->erase(CacheKey(key));
}

void TabletManager::prune_metacache() {
    _metacache->prune();
}

Status TabletManager::create_tablet(const TCreateTabletReq& req) {
    // generate tablet metadata pb
    TabletMetadataPB tablet_metadata_pb;
    tablet_metadata_pb.set_id(req.tablet_id);
    tablet_metadata_pb.set_version(1);
    tablet_metadata_pb.set_next_rowset_id(1);

    // schema
    std::unordered_map<uint32_t, uint32_t> col_idx_to_unique_id;
    uint32_t next_unique_id = req.tablet_schema.columns.size();
    for (uint32_t col_idx = 0; col_idx < next_unique_id; ++col_idx) {
        col_idx_to_unique_id[col_idx] = col_idx;
    }
    RETURN_IF_ERROR(starrocks::convert_t_schema_to_pb_schema(req.tablet_schema, next_unique_id, col_idx_to_unique_id,
                                                             tablet_metadata_pb.mutable_schema()));

    return put_tablet_metadata(tablet_metadata_pb);
}

StatusOr<Tablet> TabletManager::get_tablet(int64_t tablet_id) {
    return Tablet(this, tablet_id);
}

Status TabletManager::drop_tablet(int64_t tablet_id) {
    std::vector<std::string> objects;
    // TODO: construct prefix in LocationProvider or a common place
    const auto tablet_metadata_prefix = fmt::format("tbl_{:016X}_", tablet_id);
    const auto txnlog_prefix = fmt::format("txn_{:016X}_", tablet_id);
    auto root_path = _location_provider->root_location(tablet_id);
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(root_path));
    auto scan_cb = [&](std::string_view name) {
        if (HasPrefixString(name, tablet_metadata_prefix) || HasPrefixString(name, txnlog_prefix)) {
            objects.emplace_back(_location_provider->join_path(root_path, name));
        }
        return true;
    };
    RETURN_IF_ERROR(fs->iterate_dir(root_path, scan_cb));
    for (const auto& obj : objects) {
        erase_metacache(obj);
        (void)fs->delete_file(obj);
    }
    //drop tablet schema from metacache;
    erase_metacache(tablet_schema_cache_key(tablet_id));
    return Status::OK();
}

Status TabletManager::put_tablet_metadata(TabletMetadataPtr metadata) {
    auto options = WritableFileOptions{.sync_on_close = true, .mode = FileSystem::CREATE_OR_OPEN_WITH_TRUNCATE};
    auto metadata_location = tablet_metadata_location(metadata->id(), metadata->version());
    ASSIGN_OR_RETURN(auto wf, fs::new_writable_file(options, metadata_location));
    RETURN_IF_ERROR(wf->append(metadata->SerializeAsString()));
    RETURN_IF_ERROR(wf->close());

    // put into metacache
    auto value_ptr = std::make_unique<CacheValue>(metadata);
    bool inserted = fill_metacache(metadata_location, value_ptr.release(), static_cast<int>(metadata->SpaceUsedLong()));
    LOG_IF(WARNING, !inserted) << "Failed to put into meta cache " << metadata_location;
    return Status::OK();
}

Status TabletManager::put_tablet_metadata(const TabletMetadata& metadata) {
    auto metadata_ptr = std::make_shared<TabletMetadata>(metadata);
    return put_tablet_metadata(std::move(metadata_ptr));
}

StatusOr<TabletMetadataPtr> TabletManager::load_tablet_metadata(const string& metadata_location) {
    std::string read_buf;
    ASSIGN_OR_RETURN(auto rf, fs::new_random_access_file(metadata_location));
    ASSIGN_OR_RETURN(auto size, rf->get_size());
    if (UNLIKELY(size > std::numeric_limits<int>::max())) {
        return Status::Corruption("file size exceeded the int range");
    }
    raw::stl_string_resize_uninitialized(&read_buf, size);
    RETURN_IF_ERROR(rf->read_at_fully(0, read_buf.data(), size));

    std::shared_ptr<TabletMetadata> meta = std::make_shared<TabletMetadata>();
    bool parsed = meta->ParseFromArray(read_buf.data(), static_cast<int>(size));
    if (!parsed) {
        return Status::Corruption(fmt::format("failed to parse tablet meta {}", metadata_location));
    }
    return std::move(meta);
}

StatusOr<TabletMetadataPtr> TabletManager::get_tablet_metadata(int64_t tablet_id, int64_t version) {
    return get_tablet_metadata(tablet_metadata_location(tablet_id, version));
}

StatusOr<TabletMetadataPtr> TabletManager::get_tablet_metadata(const string& path, bool fill_cache) {
    if (auto ptr = lookup_tablet_metadata(path); ptr != nullptr) {
        return ptr;
    }
    ASSIGN_OR_RETURN(auto ptr, load_tablet_metadata(path));
    if (fill_cache) {
        auto value_ptr = std::make_unique<CacheValue>(ptr);
        bool inserted = fill_metacache(path, value_ptr.release(), static_cast<int>(ptr->SpaceUsedLong()));
        LOG_IF(WARNING, !inserted) << "Failed to put tablet metadata into cache " << path;
    }
    return ptr;
}

Status TabletManager::delete_tablet_metadata(int64_t tablet_id, int64_t version) {
    auto location = tablet_metadata_location(tablet_id, version);
    erase_metacache(location);
    return fs::delete_file(location);
}

StatusOr<TabletMetadataIter> TabletManager::list_tablet_metadata(int64_t tablet_id, bool filter_tablet) {
    std::vector<std::string> objects{};
    // TODO: construct prefix in LocationProvider
    std::string prefix;
    if (filter_tablet) {
        prefix = fmt::format("tbl_{:016X}_", tablet_id);
    } else {
        prefix = "tbl_";
    }

    auto root = _location_provider->root_location(tablet_id);
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(root));
    auto scan_cb = [&](std::string_view name) {
        if (HasPrefixString(name, prefix)) {
            objects.emplace_back(_location_provider->join_path(root, name));
        }
        return true;
    };

    RETURN_IF_ERROR(fs->iterate_dir(root, scan_cb));
    return TabletMetadataIter{this, std::move(objects)};
}

StatusOr<TxnLogPtr> TabletManager::load_txn_log(const std::string& txn_log_path) {
    std::string read_buf;
    ASSIGN_OR_RETURN(auto rf, fs::new_random_access_file(txn_log_path));
    ASSIGN_OR_RETURN(auto size, rf->get_size());
    if (UNLIKELY(size > std::numeric_limits<int>::max())) {
        return Status::Corruption("file size exceeded the int range");
    }
    raw::stl_string_resize_uninitialized(&read_buf, size);
    RETURN_IF_ERROR(rf->read_at_fully(0, read_buf.data(), size));

    std::shared_ptr<TxnLog> meta = std::make_shared<TxnLog>();
    bool parsed = meta->ParseFromArray(read_buf.data(), static_cast<int>(size));
    if (!parsed) {
        return Status::Corruption(fmt::format("failed to parse txn log {}", txn_log_path));
    }
    return std::move(meta);
}

StatusOr<TxnLogPtr> TabletManager::get_txn_log(const std::string& path, bool fill_cache) {
    if (auto ptr = lookup_txn_log(path); ptr != nullptr) {
        return ptr;
    }
    ASSIGN_OR_RETURN(auto ptr, load_txn_log(path));
    if (fill_cache) {
        auto value_ptr = std::make_unique<CacheValue>(ptr);
        bool inserted = fill_metacache(path, value_ptr.release(), static_cast<int>(ptr->SpaceUsedLong()));
        LOG_IF(WARNING, !inserted) << "Failed to cache " << path;
    }
    return ptr;
}

StatusOr<TxnLogPtr> TabletManager::get_txn_log(int64_t tablet_id, int64_t txn_id) {
    return get_txn_log(txn_log_location(tablet_id, txn_id));
}

Status TabletManager::put_txn_log(TxnLogPtr log) {
    if (UNLIKELY(!log->has_tablet_id())) {
        return Status::InvalidArgument("txn log does not have tablet id");
    }
    if (UNLIKELY(!log->has_txn_id())) {
        return Status::InvalidArgument("txn log does not have txn id");
    }
    auto options = WritableFileOptions{.sync_on_close = true, .mode = FileSystem::CREATE_OR_OPEN_WITH_TRUNCATE};
    auto txn_log_path = txn_log_location(log->tablet_id(), log->txn_id());
    ASSIGN_OR_RETURN(auto wf, fs::new_writable_file(options, txn_log_path));
    RETURN_IF_ERROR(wf->append(log->SerializeAsString()));
    RETURN_IF_ERROR(wf->close());

    // put txnlog into cache
    auto value_ptr = std::make_unique<CacheValue>(log);
    bool inserted = fill_metacache(txn_log_path, value_ptr.release(), static_cast<int>(log->SpaceUsedLong()));
    LOG_IF(WARNING, !inserted) << "Failed to put txnlog into cache " << txn_log_path;
    return Status::OK();
}

Status TabletManager::put_txn_log(const TxnLog& log) {
    return put_txn_log(std::make_shared<TxnLog>(log));
}

Status TabletManager::delete_txn_log(int64_t tablet_id, int64_t txn_id) {
    auto location = txn_log_location(tablet_id, txn_id);
    erase_metacache(location);
    return fs::delete_file(location);
}

StatusOr<TxnLogIter> TabletManager::list_txn_log(int64_t tablet_id, bool filter_tablet) {
    std::vector<std::string> objects{};
    // TODO: construct prefix in LocationProvider
    std::string prefix;
    if (filter_tablet) {
        prefix = fmt::format("txn_{:016X}_", tablet_id);
    } else {
        prefix = "txn_";
    }

    auto root = _location_provider->root_location(tablet_id);
    ASSIGN_OR_RETURN(auto fs, FileSystem::CreateSharedFromString(root));
    auto scan_cb = [&](std::string_view name) {
        if (HasPrefixString(name, prefix)) {
            objects.emplace_back(_location_provider->join_path(root, name));
        }
        return true;
    };

    RETURN_IF_ERROR(fs->iterate_dir(root, scan_cb));
    return TxnLogIter{this, std::move(objects)};
}

StatusOr<TabletSchemaPtr> TabletManager::get_tablet_schema(int64_t tablet_id) {
    auto cache_key = tablet_schema_cache_key(tablet_id);
    auto ptr = lookup_tablet_schema(cache_key);
    RETURN_IF(ptr != nullptr, ptr);
    // TODO: limit the list size
    ASSIGN_OR_RETURN(TabletMetadataIter metadata_iter, list_tablet_metadata(tablet_id, true));
    if (!metadata_iter.has_next()) {
        return Status::NotFound(fmt::format("tablet {} metadata not found", tablet_id));
    }
    ASSIGN_OR_RETURN(auto metadata, metadata_iter.next());
    auto [schema, inserted] = GlobalTabletSchemaMap::Instance()->emplace(metadata->schema());
    if (UNLIKELY(schema == nullptr)) {
        return Status::InternalError(fmt::format("tablet schema {} failed to emplace in TabletSchemaMap", tablet_id));
    }
    auto cache_value = std::make_unique<CacheValue>(schema);
    auto cache_size = inserted ? (int)schema->mem_usage() : 0;
    (void)fill_metacache(cache_key, cache_value.release(), cache_size);
    return schema;
}

Status TabletManager::publish_version(int64_t tablet_id, int64_t base_version, int64_t new_version, const int64_t* txns,
                                      int txns_size) {
    ASSIGN_OR_RETURN(auto tablet, get_tablet(tablet_id));
    return publish(&tablet, base_version, new_version, txns, txns_size);
}

static Status apply_write_log(const TxnLogPB_OpWrite& op_write, TabletMetadata* metadata) {
    if (op_write.has_rowset() && op_write.rowset().num_rows() > 0) {
        auto rowset = metadata->add_rowsets();
        rowset->CopyFrom(op_write.rowset());
        rowset->set_id(metadata->next_rowset_id());
        metadata->set_next_rowset_id(metadata->next_rowset_id() + rowset->segments_size());
    }
    return Status::OK();
}

static Status apply_compaction_log(const TxnLogPB_OpCompaction& op_compaction, TabletMetadata* metadata) {
    // It's ok to have a compaction log without input rowset and output rowset.
    if (op_compaction.input_rowsets().empty()) {
        DCHECK(!op_compaction.has_output_rowset() || op_compaction.output_rowset().num_rows() == 0);
        return Status::OK();
    }

    struct Finder {
        int64_t id;
        bool operator()(const RowsetMetadata& r) const { return r.id() == id; }
    };

    auto input_id = op_compaction.input_rowsets(0);
    auto first_input_pos = std::find_if(metadata->rowsets().begin(), metadata->rowsets().end(), Finder{input_id});
    if (UNLIKELY(first_input_pos == metadata->rowsets().end())) {
        return Status::InternalError(fmt::format("input rowset {} not found", input_id));
    }

    // Safety check:
    // 1. All input rowsets must exist in |metadata->rowsets()|
    // 2. Position of the input rowsets must be adjacent.
    auto pre_input_pos = first_input_pos;
    for (int i = 1, sz = metadata->rowsets_size(); i < sz; i++) {
        input_id = op_compaction.input_rowsets(i);
        auto it = std::find_if(pre_input_pos + 1, metadata->rowsets().end(), Finder{input_id});
        if (it == metadata->rowsets().end()) {
            return Status::InternalError(fmt::format("input rowset {} not exist", input_id));
        } else if (it != pre_input_pos + 1) {
            return Status::InternalError(fmt::format("input rowset position not adjacent"));
        } else {
            pre_input_pos = it;
        }
    }

    if (op_compaction.has_output_rowset() && op_compaction.output_rowset().num_rows() > 0) {
        // Replace the first input rowset with output rowset
        auto first_idx = static_cast<int>(first_input_pos - metadata->rowsets().begin());
        auto output_rowset = metadata->mutable_rowsets(first_idx);
        output_rowset->CopyFrom(op_compaction.output_rowset());
        output_rowset->set_id(metadata->next_rowset_id());
        metadata->set_next_rowset_id(metadata->next_rowset_id() + output_rowset->segments_size());
        ++first_input_pos;
    }
    // Erase input rowsets from metadata
    auto end_input_pos = pre_input_pos + 1;
    metadata->mutable_rowsets()->erase(first_input_pos, end_input_pos);
    return Status::OK();
}

Status apply_txn_log(const TxnLog& log, TabletMetadata* metadata) {
    if (log.has_op_write()) {
        RETURN_IF_ERROR(apply_write_log(log.op_write(), metadata));
    }

    if (log.has_op_compaction()) {
        RETURN_IF_ERROR(apply_compaction_log(log.op_compaction(), metadata));
    }

    if (log.has_op_schema_change()) {
        return Status::NotSupported("does not support apply schema change log yet");
    }
    return Status::OK();
}

Status publish(Tablet* tablet, int64_t base_version, int64_t new_version, const int64_t* txns, int txns_size) {
    // Read base version metadata
    auto res = tablet->get_metadata(base_version);
    if (!res.ok()) {
        // Check if the new version metadata exist.
        if (res.status().is_not_found() && tablet->get_metadata(new_version).ok()) {
            // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ optimization, there is no need to invoke `get_metadata` in all
            // circumstances, e.g, network and permission problems.
            return Status::OK();
        }
        LOG(WARNING) << "Fail to get " << tablet->metadata_location(base_version) << ": " << res.status();
        return res.status();
    }
    const TabletMetadataPtr& base_metadata = res.value();

    // make a copy of metadata
    auto new_metadata = std::make_shared<TabletMetadata>(*base_metadata);
    new_metadata->set_version(new_version);

    // Apply txn logs
    for (int i = 0; i < txns_size; i++) {
        auto txnid = txns[i];
        auto txnlog = tablet->get_txn_log(txnid);
        if (txnlog.status().is_not_found() && tablet->get_metadata(new_version).ok()) {
            // txn log does not exist but the new version metadata has been generated, maybe
            // this is a duplicated publish version request.
            return Status::OK();
        } else if (!txnlog.ok()) {
            LOG(WARNING) << "Fail to get " << tablet->txn_log_location(txnid) << ": " << txnlog.status();
            return txnlog.status();
        }

        auto st = apply_txn_log(**txnlog, new_metadata.get());
        if (!st.ok()) {
            LOG(WARNING) << "Fail to apply " << tablet->txn_log_location(txnid) << ": " << st;
            return st;
        }
    }

    // Save new metadata
    if (auto st = tablet->put_metadata(new_metadata); !st.ok()) {
        LOG(WARNING) << "Fail to put " << tablet->metadata_location(new_version) << ": " << st;
        return st;
    }

    // Delete txn logs
    for (int i = 0; i < txns_size; i++) {
        auto txn_id = txns[i];
        auto st = tablet->delete_txn_log(txn_id);
        LOG_IF(WARNING, !st.ok()) << "Fail to delete " << tablet->txn_log_location(txn_id) << ": " << st;
    }
    return Status::OK();
}

// TODO: better input rowsets select policy.
StatusOr<CompactionTaskPtr> TabletManager::compact(int64_t tablet_id, int64_t version, int64_t txn_id) {
    ASSIGN_OR_RETURN(auto tablet, get_tablet(tablet_id));
    ASSIGN_OR_RETURN(auto metadata, tablet.get_metadata(version));
    auto tablet_ptr = std::make_shared<Tablet>(tablet);
    std::vector<RowsetPtr> input_rowsets;
    input_rowsets.reserve(metadata->rowsets_size());
    for (const auto& rowset : metadata->rowsets()) {
        auto metadata_ptr = std::make_shared<RowsetMetadata>(rowset);
        input_rowsets.emplace_back(std::make_shared<Rowset>(tablet_ptr.get(), std::move(metadata_ptr)));
    }
    return std::make_shared<HorizontalCompactionTask>(txn_id, version, std::move(tablet_ptr), std::move(input_rowsets));
}

} // namespace starrocks::lake
