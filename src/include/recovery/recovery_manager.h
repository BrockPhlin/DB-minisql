#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <map>
#include <unordered_map>
#include <vector>

#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    void Init(CheckPoint &last_checkpoint) {
        persist_lsn_ = last_checkpoint.checkpoint_lsn_;
        active_txns_ = last_checkpoint.active_txns_;
        data_ = last_checkpoint.persist_data_;
    }

    void RedoPhase() {
        // 正向重做：从检查点之后重放所有日志记录
        auto it = log_recs_.upper_bound(persist_lsn_);
        for (; it != log_recs_.end(); ++it) {
            const auto &log = it->second;

            switch (log->type_) {
                case LogRecType::kBegin:
                    active_txns_[log->txn_id_] = log->lsn_;
                    break;

                case LogRecType::kCommit:
                    active_txns_.erase(log->txn_id_);
                    break;

                case LogRecType::kInsert:
                    data_[log->key_] = log->new_val_;
                    break;

                case LogRecType::kDelete:
                    data_.erase(log->key_);
                    break;

                case LogRecType::kUpdate:
                    data_[log->key_] = log->new_val_;
                    break;

                case LogRecType::kAbort: {
                    // 没有 CLR 日志：沿该事务的 prev_lsn 链逆序遍历，
                    // 将其所有修改反向物理撤销
                    lsn_t cur_lsn = log->prev_lsn_;
                    while (cur_lsn != INVALID_LSN) {
                        auto undo_it = log_recs_.find(cur_lsn);
                        if (undo_it == log_recs_.end()) break;
                        const auto &undo_log = undo_it->second;
                        if (undo_log->txn_id_ != log->txn_id_) break;

                        switch (undo_log->type_) {
                            case LogRecType::kInsert:
                                // 撤销插入：删除 key
                                data_.erase(undo_log->key_);
                                break;
                            case LogRecType::kDelete:
                                // 撤销删除：恢复 old_val
                                data_[undo_log->key_] = undo_log->old_val_;
                                break;
                            case LogRecType::kUpdate:
                                // 撤销更新：恢复 old_val
                                data_[undo_log->key_] = undo_log->old_val_;
                                break;
                            default:
                                break;
                        }
                        cur_lsn = undo_log->prev_lsn_;
                    }
                    active_txns_.erase(log->txn_id_);
                    break;
                }

                default:
                    break;
            }
        }
    }

    void UndoPhase() {
        // 反向撤销：逆序遍历日志，撤销仍活跃事务的所有操作
        for (auto it = log_recs_.rbegin(); it != log_recs_.rend(); ++it) {
            const auto &log = it->second;

            if (active_txns_.find(log->txn_id_) == active_txns_.end()) {
                continue;
            }

            switch (log->type_) {
                case LogRecType::kInsert:
                    // 撤销插入：删除 key
                    data_.erase(log->key_);
                    break;

                case LogRecType::kDelete:
                    // 撤销删除：恢复 old_val
                    data_[log->key_] = log->old_val_;
                    break;

                case LogRecType::kUpdate:
                    // 撤销更新：恢复 old_val
                    data_[log->key_] = log->old_val_;
                    break;

                case LogRecType::kBegin:
                    // 遇到未提交事务的 BEGIN，从活跃事务表中移除
                    active_txns_.erase(log->txn_id_);
                    break;

                default:
                    break;
            }
        }
    }

    // 仅供测试使用
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // 仅供测试使用
    inline KvDatabase &GetDatabase() { return data_; }

private:
    std::map<lsn_t, LogRecPtr> log_recs_{};
    lsn_t persist_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase data_{};  // 数据库中所有数据
};

#endif  // MINISQL_RECOVERY_MANAGER_H
