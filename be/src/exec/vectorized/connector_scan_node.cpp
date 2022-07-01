// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/connector_scan_node.h"

#include <atomic>
#include <memory>

#include "common/config.h"
#include "exec/pipeline/scan/connector_scan_operator.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "util/priority_thread_pool.hpp"

namespace starrocks::vectorized {

// ======================================================

class OpenLimitAllocator {
public:
    OpenLimitAllocator() = default;
    ~OpenLimitAllocator() {
        for (auto& it : _data) {
            delete it.second;
        }
    }

    OpenLimitAllocator(const OpenLimitAllocator&) = delete;
    void operator=(const OpenLimitAllocator&) = delete;

    static OpenLimitAllocator& instance() {
        static OpenLimitAllocator obj;
        return obj;
    }

    std::atomic<int32_t>* allocate(const std::string& key);

private:
    std::mutex _lock;
    std::unordered_map<std::string, std::atomic<int32_t>*> _data;
};

// TODO: find granualarity of open limit
static std::atomic<int32_t> connector_scan_node_open_limit;

// ======================================================
// if *lvalue == expect, swap(*lvalue,*rvalue)
inline bool atomic_cas(std::atomic_bool* lvalue, std::atomic_bool* rvalue, bool expect) {
    bool res = lvalue->compare_exchange_strong(expect, *rvalue);
    if (res) *rvalue = expect;
    return res;
}

class ConnectorScanner {
public:
    ConnectorScanner(connector::DataSourcePtr&& data_source) : _data_source(std::move(data_source)) {}

    Status init(RuntimeState* state) {
        _runtime_state = state;
        return Status::OK();
    }
    Status open(RuntimeState* state) {
        if (_is_open) return Status::OK();
        RETURN_IF_ERROR(_data_source->open(state));
        _is_open = true;
        return Status::OK();
    }
    void close(RuntimeState* state) { _data_source->close(state); }
    Status get_next(RuntimeState* state, ChunkPtr* chunk) {
        RETURN_IF_ERROR(_data_source->get_next(state, chunk));
        return Status::OK();
    }

    int64_t raw_rows_read() const { return _data_source->raw_rows_read(); }
    int64_t num_rows_read() const { return _data_source->num_rows_read(); }
    void set_keep_priority(bool v) { _keep_priority = v; }
    bool keep_priority() const { return _keep_priority; }
    bool is_open() { return _is_open; }

    RuntimeState* runtime_state() { return _runtime_state; }

    int32_t open_limit() { return connector_scan_node_open_limit.load(std::memory_order_relaxed); }

    bool acquire_pending_token(std::atomic_bool* token) {
        // acquire resource
        return atomic_cas(token, &_pending_token, true);
    }

    bool release_pending_token(std::atomic_bool* token) {
        if (_pending_token) {
            _pending_token = false;
            *token = true;
            return true;
        }
        return false;
    }

    bool has_pending_token() { return _pending_token; }

    void enter_pending_queue() { _pending_queue_sw.start(); }
    uint64_t exit_pending_queue() { return _pending_queue_sw.reset(); }

private:
    connector::DataSourcePtr _data_source = nullptr;
    RuntimeState* _runtime_state = nullptr;
    bool _is_open = false;
    bool _keep_priority = false;
    std::atomic_bool _pending_token = false;
    MonotonicStopWatch _pending_queue_sw;
};

// ======================================================

ConnectorScanNode::ConnectorScanNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ScanNode(pool, tnode, descs) {
    _name = "connector_scan";
    auto c = connector::ConnectorManager::default_instance()->get(tnode.connector_scan_node.connector_name);
    _data_source_provider = c->create_data_source_provider(this, tnode);
}

ConnectorScanNode::~ConnectorScanNode() {
    if (_runtime_state != nullptr) {
        close(_runtime_state);
    }
}

Status ConnectorScanNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ScanNode::init(tnode, state));
    return Status::OK();
}

pipeline::OpFactories ConnectorScanNode::decompose_to_pipeline(pipeline::PipelineBuilderContext* context) {
    size_t dop = context->dop_of_source_operator(id());
    auto scan_op = std::make_shared<pipeline::ConnectorScanOperatorFactory>(context->next_operator_id(), this, dop);

    auto&& rc_rf_probe_collector = std::make_shared<RcRfProbeCollector>(1, std::move(this->runtime_filter_collector()));
    this->init_runtime_filter_for_operator(scan_op.get(), context, rc_rf_probe_collector);

    auto operators = pipeline::decompose_scan_node_to_pipeline(scan_op, this, context);

    if (!_data_source_provider->insert_local_exchange_operator()) {
        operators = context->maybe_interpolate_local_passthrough_exchange(context->fragment_context()->runtime_state(),
                                                                          operators, context->degree_of_parallelism());
    }
    return operators;
}

// ==============================================================

Status ConnectorScanNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(ScanNode::prepare(state));
    RETURN_IF_ERROR(_data_source_provider->prepare(state));
    _init_counter();
    _runtime_state = state;
    return Status::OK();
}

Status ConnectorScanNode::open(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_ERROR(ScanNode::open(state));
    RETURN_IF_ERROR(_data_source_provider->open(state));
    return Status::OK();
}

Status ConnectorScanNode::_start_scan_thread(RuntimeState* state) {
    for (const TScanRangeParams& scan_range : _scan_ranges) {
        _create_and_init_scanner(state, scan_range.scan_range);
    }

    // init chunk pool
    _pending_scanners.reverse();
    _num_scanners = _pending_scanners.size();
    _chunks_per_scanner = config::doris_scanner_row_num / state->chunk_size();
    _chunks_per_scanner += static_cast<int>(config::doris_scanner_row_num % state->chunk_size() != 0);
    int concurrency = std::min<int>(config::max_hdfs_scanner_num, _num_scanners);
    int chunks = _chunks_per_scanner * concurrency;
    _chunk_pool.reserve(chunks);
    TRY_CATCH_BAD_ALLOC(_fill_chunk_pool(chunks));

    // start scanner
    std::lock_guard<std::mutex> l(_mtx);
    for (int i = 0; i < concurrency; i++) {
        CHECK(_submit_scanner(_pop_pending_scanner(), true));
    }

    return Status::OK();
}

Status ConnectorScanNode::_create_and_init_scanner(RuntimeState* state, const TScanRange& scan_range) {
    connector::DataSourcePtr data_source = _data_source_provider->create_data_source(scan_range);
    data_source->set_predicates(_conjunct_ctxs);
    data_source->set_runtime_filters(&_runtime_filter_collector);
    data_source->set_read_limit(_limit);
    data_source->set_runtime_profile(_runtime_profile.get());
    ConnectorScanner* scanner = _pool->add(new ConnectorScanner(std::move(data_source)));
    scanner->init(state);
    _push_pending_scanner(scanner);
    return Status::OK();
}

Status ConnectorScanNode::get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    if (!_start && _status.ok()) {
        Status status = _start_scan_thread(state);
        _update_status(status);
        LOG_IF(ERROR, !status.ok()) << "Failed to start scan node: " << status.to_string();
        _start = true;
        RETURN_IF_ERROR(status);
    } else if (!_start) {
        _result_chunks.shutdown();
        _start = true;
    }

    (*chunk).reset();

    Status status = _get_status();
    if (!status.ok()) {
        *eos = true;
        return status.is_end_of_file() ? Status::OK() : status;
    }

    {
        std::unique_lock<std::mutex> l(_mtx);
        const int32_t num_closed = _closed_scanners.load(std::memory_order_acquire);
        const int32_t num_pending = _pending_scanners.size();
        const int32_t num_running = _num_scanners - num_pending - num_closed;
        if ((num_pending > 0) && (num_running < config::max_hdfs_scanner_num)) {
            if (_chunk_pool.size() >= (num_running + 1) * _chunks_per_scanner) {
                ConnectorScanner* scanner = _pop_pending_scanner();
                l.unlock();
                (void)_submit_scanner(scanner, true);
            }
        }
    }

    if (_result_chunks.blocking_get(chunk)) {
        TRY_CATCH_BAD_ALLOC(_fill_chunk_pool(1));
        eval_join_runtime_filters(chunk);
        _num_rows_returned += (*chunk)->num_rows();
        COUNTER_SET(_rows_returned_counter, _num_rows_returned);
        if (reached_limit()) {
            int64_t num_rows_over = _num_rows_returned - _limit;
            (*chunk)->set_num_rows((*chunk)->num_rows() - num_rows_over);
            COUNTER_SET(_rows_returned_counter, _limit);
            _update_status(Status::EndOfFile("ConnectorScanNode has reach limit"));
            _result_chunks.shutdown();
        }
        *eos = false;
        DCHECK_CHUNK(*chunk);
        return Status::OK();
    }

    _update_status(Status::EndOfFile("EOF of ConnectorScanNode"));
    *eos = true;
    status = _get_status();
    return status.is_end_of_file() ? Status::OK() : status;
}

// The more tasks you submit, the less priority you get.
static int compute_priority(int32_t num_submitted_tasks) {
    // int nice = 20;
    // while (nice > 0 && num_submitted_tasks > (22 - nice) * (20 - nice) * 6) {
    //     --nice;
    // }
    // return nice;
    if (num_submitted_tasks < 5) return 20;
    if (num_submitted_tasks < 19) return 19;
    if (num_submitted_tasks < 49) return 18;
    if (num_submitted_tasks < 91) return 17;
    if (num_submitted_tasks < 145) return 16;
    if (num_submitted_tasks < 211) return 15;
    if (num_submitted_tasks < 289) return 14;
    if (num_submitted_tasks < 379) return 13;
    if (num_submitted_tasks < 481) return 12;
    if (num_submitted_tasks < 595) return 11;
    if (num_submitted_tasks < 721) return 10;
    if (num_submitted_tasks < 859) return 9;
    if (num_submitted_tasks < 1009) return 8;
    if (num_submitted_tasks < 1171) return 7;
    if (num_submitted_tasks < 1345) return 6;
    if (num_submitted_tasks < 1531) return 5;
    if (num_submitted_tasks < 1729) return 4;
    if (num_submitted_tasks < 1939) return 3;
    if (num_submitted_tasks < 2161) return 2;
    if (num_submitted_tasks < 2395) return 1;
    return 0;
}

bool ConnectorScanNode::_submit_scanner(ConnectorScanner* scanner, bool blockable) {
    auto* thread_pool = _runtime_state->exec_env()->thread_pool();
    int delta = static_cast<int>(!scanner->keep_priority());
    int32_t num_submit = _scanner_submit_count.fetch_add(delta, std::memory_order_relaxed);

    PriorityThreadPool::Task task;
    task.work_function = [this, scanner] { _scanner_thread(scanner); };
    task.priority = compute_priority(num_submit);
    _running_threads.fetch_add(1, std::memory_order_release);

    if (thread_pool->try_offer(task)) {
        return true;
    }
    if (blockable) {
        thread_pool->offer(task);
        return true;
    }

    LOG(WARNING) << "thread pool busy";
    _running_threads.fetch_sub(1, std::memory_order_release);
    _scanner_submit_count.fetch_sub(delta, std::memory_order_relaxed);
    return false;
}

void ConnectorScanNode::_scanner_thread(ConnectorScanner* scanner) {
    MemTracker* prev_tracker = tls_thread_status.set_mem_tracker(scanner->runtime_state()->instance_mem_tracker());
    DeferOp op([&] {
        tls_thread_status.set_mem_tracker(prev_tracker);
        _running_threads.fetch_sub(1, std::memory_order_release);

        if (_closed_scanners.load(std::memory_order_acquire) == _num_scanners) {
            _result_chunks.shutdown();
        }
    });

    // if global status was not ok
    // we need fast failure
    if (!_get_status().ok()) {
        _release_scanner(scanner);
        return;
    }

    int concurrency_limit = config::max_hdfs_file_handle;

    // There is a situation where once a resource overrun has occurred,
    // the scanners that were previously overrun are basically in a pending state,
    // so even if there are enough resources to follow, they cannot be fully utilized,
    // and we need to schedule the scanners that are in a pending state as well.
    if (scanner->has_pending_token()) {
        int concurrency = std::min<int>(config::max_hdfs_scanner_num, _num_scanners);
        int need_put = concurrency - _running_threads;
        int left_resource = concurrency_limit - scanner->open_limit();
        if (left_resource > 0) {
            need_put = std::min(left_resource, need_put);
            std::lock_guard<std::mutex> l(_mtx);
            while (need_put-- > 0 && !_pending_scanners.empty()) {
                if (!_submit_scanner(_pop_pending_scanner(), false)) {
                    break;
                }
            }
        }
    }

    if (!scanner->has_pending_token()) {
        scanner->acquire_pending_token(&_pending_token);
    }

    // if opened file greater than this. scanner will push back to pending list.
    // We can't have all scanners in the pending state, we need to
    // make sure there is at least one thread on each SCAN NODE that can be running
    if (!scanner->is_open() && scanner->open_limit() > concurrency_limit) {
        if (!scanner->has_pending_token()) {
            std::lock_guard<std::mutex> l(_mtx);
            _push_pending_scanner(scanner);
            return;
        }
    }

    Status status = scanner->open(_runtime_state);
    scanner->set_keep_priority(false);

    bool resubmit = false;
    int64_t raw_rows_threshold = scanner->raw_rows_read() + config::doris_scanner_row_num;

    ChunkPtr chunk = nullptr;

    while (status.ok()) {
        // if global status was not ok, we need fast failure and no need to read file
        if (!_get_status().ok()) {
            break;
        }

        {
            std::lock_guard<std::mutex> l(_mtx);
            if (_chunk_pool.empty()) {
                scanner->set_keep_priority(true);
                scanner->release_pending_token(&_pending_token);
                _push_pending_scanner(scanner);
                scanner = nullptr;
                break;
            }
            chunk = _chunk_pool.pop();
        }

        status = scanner->get_next(_runtime_state, &chunk);
        if (!status.ok()) {
            std::lock_guard<std::mutex> l(_mtx);
            _chunk_pool.push(chunk);
            break;
        }

        if (!_result_chunks.put(chunk)) {
            status = Status::Aborted("result chunks has been shutdown");
            break;
        }
        // Improve for select * from table limit x;
        if (limit() != -1 && scanner->num_rows_read() >= limit()) {
            status = Status::EndOfFile("limit reach");
            break;
        }
        if (scanner->raw_rows_read() >= raw_rows_threshold) {
            resubmit = true;
            break;
        }
    }

    Status global_status = _get_status();
    if (global_status.ok()) {
        if (status.ok() && resubmit) {
            if (!_submit_scanner(scanner, false)) {
                std::lock_guard<std::mutex> l(_mtx);
                scanner->release_pending_token(&_pending_token);
                _push_pending_scanner(scanner);
            }
        } else if (status.ok()) {
            DCHECK(scanner == nullptr);
        } else if (status.is_end_of_file()) {
            scanner->release_pending_token(&_pending_token);
            scanner->close(_runtime_state);
            _closed_scanners.fetch_add(1, std::memory_order_release);
            std::lock_guard<std::mutex> l(_mtx);
            auto nscanner = _pending_scanners.empty() ? nullptr : _pop_pending_scanner();
            if (nscanner != nullptr && !_submit_scanner(nscanner, false)) {
                _push_pending_scanner(nscanner);
            }
        } else {
            _update_status(status);
            _release_scanner(scanner);
        }
    } else {
        // sometimes state == ok but global_status was not ok
        if (scanner != nullptr) {
            _release_scanner(scanner);
        }
    }
}

void ConnectorScanNode::_release_scanner(ConnectorScanner* scanner) {
    scanner->release_pending_token(&_pending_token);
    scanner->close(_runtime_state);
    _closed_scanners.fetch_add(1, std::memory_order_release);
    _close_pending_scanners();
}

void ConnectorScanNode::_close_pending_scanners() {
    std::lock_guard<std::mutex> l(_mtx);
    while (!_pending_scanners.empty()) {
        auto* scanner = _pop_pending_scanner();
        scanner->close(_runtime_state);
        _closed_scanners.fetch_add(1, std::memory_order_release);
    }
}

void ConnectorScanNode::_push_pending_scanner(ConnectorScanner* scanner) {
    scanner->enter_pending_queue();
    _pending_scanners.push(scanner);
}

ConnectorScanner* ConnectorScanNode::_pop_pending_scanner() {
    ConnectorScanner* scanner = _pending_scanners.pop();
    uint64_t time = scanner->exit_pending_queue();
    COUNTER_UPDATE(_profile.scanner_queue_timer, time);
    COUNTER_UPDATE(_profile.scanner_queue_counter, 1);
    return scanner;
}

void ConnectorScanNode::_update_status(const Status& status) {
    std::lock_guard<SpinLock> lck(_status_mtx);
    if (_status.ok()) {
        _status = status;
    }
}

Status ConnectorScanNode::_get_status() {
    std::lock_guard<SpinLock> lck(_status_mtx);
    return _status;
}

void ConnectorScanNode::_fill_chunk_pool(int count) {
    std::lock_guard<std::mutex> l(_mtx);

    for (int i = 0; i < count; i++) {
        auto chunk = std::make_shared<Chunk>();
        _chunk_pool.push(std::move(chunk));
    }
}

Status ConnectorScanNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }
    _closed = true;
    _update_status(Status::Cancelled("closed"));
    _result_chunks.shutdown();
    while (_running_threads.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    _close_pending_scanners();
    _data_source_provider->close(state);
    RETURN_IF_ERROR(ScanNode::close(state));
    return Status::OK();
}

Status ConnectorScanNode::set_scan_ranges(const std::vector<TScanRangeParams>& scan_ranges) {
    _scan_ranges = scan_ranges;
    if (!accept_empty_scan_ranges() && scan_ranges.size() == 0) {
        // If scan ranges size is zero,
        // it means data source provider does not support reading by scan ranges.
        // So here we insert a single placeholder, to force data source provider
        // to create at least one data source
        _scan_ranges.emplace_back(TScanRangeParams());
    }
    COUNTER_UPDATE(_profile.scan_ranges_counter, scan_ranges.size());
    return Status::OK();
}

bool ConnectorScanNode::accept_empty_scan_ranges() const {
    return _data_source_provider->accept_empty_scan_ranges();
}

void ConnectorScanNode::_init_counter() {
    _profile.scanner_queue_timer = ADD_TIMER(_runtime_profile, "ScannerQueueTime");
    _profile.scanner_queue_counter = ADD_COUNTER(_runtime_profile, "ScannerQueueCounter", TUnit::UNIT);
    _profile.scan_ranges_counter = ADD_COUNTER(_runtime_profile, "ScanRanges", TUnit::UNIT);
}

} // namespace starrocks::vectorized
