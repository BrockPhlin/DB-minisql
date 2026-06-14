#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <memory>
#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * LogRec 表示内存 WAL 中的一条日志记录。
 * 包含所有日志类型可能用到的字段；对于特定类型，未使用的字段保持默认值。
 */
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};
    txn_id_t txn_id_{INVALID_TXN_ID};

    /* 数据操作相关字段 */
    KeyType key_{};
    ValType old_val_{0};
    ValType new_val_{0};

    /* 仅供测试使用 */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_ = {};
lsn_t LogRec::next_lsn_ = 0;

typedef std::shared_ptr<LogRec> LogRecPtr;

static LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kInsert;
    log->txn_id_ = txn_id;
    log->key_ = std::move(ins_key);
    log->new_val_ = ins_val;
    log->lsn_ = LogRec::next_lsn_++;
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kDelete;
    log->txn_id_ = txn_id;
    log->key_ = std::move(del_key);
    log->old_val_ = del_val;
    log->lsn_ = LogRec::next_lsn_++;
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kUpdate;
    log->txn_id_ = txn_id;
    log->key_ = std::move(new_key);
    log->old_val_ = old_val;
    log->new_val_ = new_val;
    log->lsn_ = LogRec::next_lsn_++;
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateBeginLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kBegin;
    log->txn_id_ = txn_id;
    log->lsn_ = LogRec::next_lsn_++;
    log->prev_lsn_ = INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateCommitLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kCommit;
    log->txn_id_ = txn_id;
    log->lsn_ = LogRec::next_lsn_++;
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateAbortLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kAbort;
    log->txn_id_ = txn_id;
    log->lsn_ = LogRec::next_lsn_++;
    auto it = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

#endif  // MINISQL_LOG_REC_H
