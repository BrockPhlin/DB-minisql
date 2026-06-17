#include "concurrency/lock_manager.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "common/rowid.h"
#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"

void LockManager::SetTxnMgr(TxnManager *txn_mgr) { txn_mgr_ = txn_mgr; }

/**
 * TODO: Student Implement
 */
bool LockManager::LockShared(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    // READ_UNCOMMITTED reads dirty data and therefore never takes shared locks.
    if (txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockSharedOnReadUncommitted);
    }

    // Reject locking in the shrinking phase and make sure the queue exists.
    LockPrepare(txn, rid);

    LockRequestQueue &req_queue = lock_table_[rid];
    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);

    // A shared lock only conflicts with an exclusive lock that is currently held.
    if (req_queue.is_writing_) {
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted || !req_queue.is_writing_;
        });
    }

    // We may have been woken because the deadlock detector aborted us.
    CheckAbort(txn, req_queue);

    txn->GetSharedLockSet().emplace(rid);
    req_queue.sharing_cnt_++;
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kShared;
    return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    LockPrepare(txn, rid);

    LockRequestQueue &req_queue = lock_table_[rid];
    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);

    // An exclusive lock conflicts with every other holder: writer or sharers.
    if (req_queue.is_writing_ || req_queue.sharing_cnt_ > 0) {
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted ||
                   (!req_queue.is_writing_ && req_queue.sharing_cnt_ == 0);
        });
    }

    CheckAbort(txn, req_queue);

    txn->GetExclusiveLockSet().emplace(rid);
    req_queue.is_writing_ = true;
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kExclusive;
    return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }

    LockRequestQueue &req_queue = lock_table_[rid];

    // Only one transaction may upgrade on a given record at a time.
    if (req_queue.is_upgrading_) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
    }

    // Mark the (already granted) shared request as wanting an exclusive lock.
    req_queue.GetLockRequestIter(txn->GetTxnId())->lock_mode_ = LockMode::kExclusive;

    // Wait until this transaction is the sole remaining shared holder and no writer exists.
    if (req_queue.is_writing_ || req_queue.sharing_cnt_ > 1) {
        req_queue.is_upgrading_ = true;
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted ||
                   (!req_queue.is_writing_ && req_queue.sharing_cnt_ == 1);
        });
        req_queue.is_upgrading_ = false;
    }

    CheckAbort(txn, req_queue);

    // Convert the shared lock into an exclusive one.
    req_queue.sharing_cnt_--;
    req_queue.is_writing_ = true;
    txn->GetSharedLockSet().erase(rid);
    txn->GetExclusiveLockSet().emplace(rid);
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kExclusive;
    return true;
}

/**
 * TODO: Student Implement
 */
bool LockManager::Unlock(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    // Drop the record from the transaction's bookkeeping and remember its mode.
    bool was_shared = (txn->GetSharedLockSet().erase(rid) > 0);
    bool was_exclusive = (txn->GetExclusiveLockSet().erase(rid) > 0);

    // 2PL: the first release moves a GROWING transaction into the SHRINKING phase.
    // Under REPEATABLE_READ every release counts; under the weaker isolation levels
    // only releasing an exclusive lock ends the growing phase.
    if (txn->GetState() == TxnState::kGrowing &&
        (txn->GetIsolationLevel() == IsolationLevel::kRepeatedRead || was_exclusive)) {
        txn->SetState(TxnState::kShrinking);
    }

    auto table_iter = lock_table_.find(rid);
    if (table_iter == lock_table_.end()) {
        return false;
    }
    LockRequestQueue &req_queue = table_iter->second;
    req_queue.EraseLockRequest(txn->GetTxnId());

    if (was_exclusive) {
        req_queue.is_writing_ = false;
    } else if (was_shared && req_queue.sharing_cnt_ > 0) {
        req_queue.sharing_cnt_--;
    }

    // Let blocked transactions re-evaluate their wait conditions.
    req_queue.cv_.notify_all();
    return true;
}

/**
 * TODO: Student Implement
 */
void LockManager::LockPrepare(Txn *txn, const RowId &rid) {
    // No new lock may be acquired once the transaction has started releasing locks.
    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }
    // Create the request queue for this record if it does not exist yet.
    if (lock_table_.find(rid) == lock_table_.end()) {
        lock_table_.emplace(std::piecewise_construct, std::forward_as_tuple(rid), std::forward_as_tuple());
    }
}

/**
 * TODO: Student Implement
 */
void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
    // A transaction that was aborted while waiting (e.g. by the deadlock detector)
    // removes its pending request and reports the abort to the caller.
    if (txn->GetState() == TxnState::kAborted) {
        req_queue.EraseLockRequest(txn->GetTxnId());
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
    }
}

/**
 * TODO: Student Implement
 */
void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) { waits_for_[t1].insert(t2); }

/**
 * TODO: Student Implement
 */
void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
    auto iter = waits_for_.find(t1);
    if (iter == waits_for_.end()) {
        return;
    }
    iter->second.erase(t2);
    if (iter->second.empty()) {
        waits_for_.erase(iter);
    }
}

bool LockManager::DFS(txn_id_t txn_id) {
    visited_set_.insert(txn_id);
    visited_path_.push(txn_id);

    auto iter = waits_for_.find(txn_id);
    if (iter != waits_for_.end()) {
        // std::set keeps neighbours sorted, so they are explored low-to-high (deterministic).
        for (const txn_id_t next : iter->second) {
            if (visited_set_.find(next) != visited_set_.end()) {
                // A node already on the current path closes a cycle.
                revisited_node_ = next;
                return true;
            }
            if (DFS(next)) {
                return true;
            }
        }
    }

    visited_path_.pop();
    visited_set_.erase(txn_id);
    return false;
}

/**
 * TODO: Student Implement
 */
bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
    visited_set_.clear();
    while (!visited_path_.empty()) {
        visited_path_.pop();
    }
    revisited_node_ = INVALID_TXN_ID;

    // Always start DFS from the lowest transaction id for determinism.
    std::set<txn_id_t> nodes;
    for (const auto &entry : waits_for_) {
        nodes.insert(entry.first);
    }

    for (const txn_id_t start : nodes) {
        if (DFS(start)) {
            // The cycle is the suffix of the path from revisited_node_ to the top.
            // Report the youngest (largest id) transaction in that cycle.
            newest_tid_in_cycle = revisited_node_;
            while (!visited_path_.empty()) {
                txn_id_t node = visited_path_.top();
                visited_path_.pop();
                newest_tid_in_cycle = std::max(newest_tid_in_cycle, node);
                if (node == revisited_node_) {
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

void LockManager::DeleteNode(txn_id_t txn_id) {
    waits_for_.erase(txn_id);

    auto *txn = txn_mgr_->GetTransaction(txn_id);

    for (const auto &row_id: txn->GetSharedLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }

    for (const auto &row_id: txn->GetExclusiveLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }
}

/**
 * TODO: Student Implement
 */
void LockManager::RunCycleDetection() {
    while (enable_cycle_detection_) {
        std::this_thread::sleep_for(cycle_detection_interval_);
        {
            std::unique_lock<std::mutex> lock(latch_);

            // Build the waits-for graph from scratch on every wake-up. A waiting
            // transaction waits for every transaction holding a lock on the same record.
            waits_for_.clear();
            for (const auto &entry : lock_table_) {
                const LockRequestQueue &req_queue = entry.second;
                std::vector<txn_id_t> granted;
                std::vector<txn_id_t> waiting;
                for (const auto &req : req_queue.req_list_) {
                    Txn *t = txn_mgr_->GetTransaction(req.txn_id_);
                    // Never add nodes for / edges to already aborted transactions.
                    if (t == nullptr || t->GetState() == TxnState::kAborted) {
                        continue;
                    }
                    if (req.granted_ == LockMode::kNone) {
                        waiting.push_back(req.txn_id_);
                    } else {
                        granted.push_back(req.txn_id_);
                    }
                }
                for (const txn_id_t w : waiting) {
                    for (const txn_id_t g : granted) {
                        if (w != g) {
                            AddEdge(w, g);
                        }
                    }
                }
            }

            // Break every cycle by aborting its youngest transaction.
            txn_id_t youngest = INVALID_TXN_ID;
            while (HasCycle(youngest)) {
                Txn *victim = txn_mgr_->GetTransaction(youngest);
                if (victim != nullptr) {
                    victim->SetState(TxnState::kAborted);
                }
                DeleteNode(youngest);
            }

            // Notify all waiters so the aborted ones wake up and throw.
            for (auto &entry : lock_table_) {
                entry.second.cv_.notify_all();
            }

            waits_for_.clear();
        }
    }
}

/**
 * TODO: Student Implement
 */
std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetEdgeList() {
    std::vector<std::pair<txn_id_t, txn_id_t>> result;
    for (const auto &entry : waits_for_) {
        for (const txn_id_t t2 : entry.second) {
            result.emplace_back(entry.first, t2);
        }
    }
    return result;
}
