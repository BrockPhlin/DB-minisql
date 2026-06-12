#include "storage/table_iterator.h"

#include <cstring>

#include "common/macros.h"
#include "page/table_page.h"
#include "storage/table_heap.h"

/**
 * 默认构造：根据 (table_heap, rid) 构造一个迭代器。
 *   - 立即把 rid 指向的行 fetch 进来。
 *   - 哨兵：rid.Get() == INVALID_ROWID.Get() 时不 fetch（用于 End()）。
 */
TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), rid_(rid), txn_(txn), row_(rid) {
  if (table_heap_ != nullptr && rid_.Get() != INVALID_ROWID.Get()) {
    // 把 rid_ 指向的行 fetch 到 row_
    BufferPoolManager *bpm = table_heap_->GetBufferPoolManager();
    Schema *schema         = table_heap_->GetSchema();
    LockManager *lock_mgr  = table_heap_->GetLockManager();

    auto page = reinterpret_cast<TablePage *>(bpm->FetchPage(rid_.GetPageId()));
    ASSERT(page != nullptr, "FetchPage returned null in iterator ctor");
    page->GetTuple(&row_, schema, txn_, lock_mgr);
    bpm->UnpinPage(rid_.GetPageId(), false);
  }
}

// 拷贝构造：复用对方 row_（已经 fetch 过了）
TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), rid_(other.rid_), txn_(other.txn_), row_(other.row_) {}

// 析构：default 即可（row_ 是 Row 值，析构时会 delete 内部 Field*）
TableIterator::~TableIterator() = default;

bool TableIterator::operator==(const TableIterator &itr) const {
  return rid_ == itr.rid_;
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(rid_ == itr.rid_);
}

const Row &TableIterator::operator*() {
  if (rid_.Get() == INVALID_ROWID.Get()) {
    ASSERT(false, "dereference of End iterator");
  }
  return row_;
}

Row *TableIterator::operator->() {
  if (rid_.Get() == INVALID_ROWID.Get()) {
    ASSERT(false, "dereference of End iterator");
  }
  return &row_;
}

// 赋值：直接复制字段即可（没有缓存的 page，所以没有 unpin 要做）
TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  table_heap_ = itr.table_heap_;
  rid_        = itr.rid_;
  txn_        = itr.txn_;
  row_        = itr.row_;
  return *this;
}

// 前缀 ++：把 rid_ 推到下一个有效 tuple；走完则置 End()
TableIterator &TableIterator::operator++() {
  if (rid_.Get() == INVALID_ROWID.Get()) {
    ASSERT(false, "increment past End");
  }

  BufferPoolManager *bpm = table_heap_->GetBufferPoolManager();
  page_id_t pid = rid_.GetPageId();
  while (pid != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(bpm->FetchPage(pid));
    ASSERT(page != nullptr, "FetchPage returned null in iterator advance");

    // 1) 当前 page 找下一条
    RowId next_rid;
    if (page->GetNextTupleRid(rid_, &next_rid)) {
      bpm->UnpinPage(pid, false);
      rid_ = next_rid;
      // fetch 新 rid 对应的 row
      Schema *schema        = table_heap_->GetSchema();
      LockManager *lock_mgr = table_heap_->GetLockManager();
      auto new_page = reinterpret_cast<TablePage *>(bpm->FetchPage(rid_.GetPageId()));
      ASSERT(new_page != nullptr, "FetchPage returned null in ++");
      row_ = Row(rid_);
      new_page->GetTuple(&row_, schema, txn_, lock_mgr);
      bpm->UnpinPage(rid_.GetPageId(), false);
      return *this;
    }

    // 2) 当前 page 没有更多，去下一页
    page_id_t next_pid = page->GetNextPageId();
    bpm->UnpinPage(pid, false);

    if (next_pid == INVALID_PAGE_ID) {
      break;
    }

    // 3) 翻到 next_pid，找它的第一条
    auto next_page = reinterpret_cast<TablePage *>(bpm->FetchPage(next_pid));
    ASSERT(next_page != nullptr, "FetchPage returned null on advance");

    RowId first_rid;
    if (next_page->GetFirstTupleRid(&first_rid)) {
      bpm->UnpinPage(next_pid, false);
      rid_ = first_rid;
      // fetch 新 rid 对应的 row
      Schema *schema        = table_heap_->GetSchema();
      LockManager *lock_mgr = table_heap_->GetLockManager();
      auto new_page = reinterpret_cast<TablePage *>(bpm->FetchPage(rid_.GetPageId()));
      ASSERT(new_page != nullptr, "FetchPage returned null in ++ cross-page");
      row_ = Row(rid_);
      new_page->GetTuple(&row_, schema, txn_, lock_mgr);
      bpm->UnpinPage(rid_.GetPageId(), false);
      return *this;
    }

    // next page 也没，继续翻
    bpm->UnpinPage(next_pid, false);
    pid = next_page->GetNextPageId();
  }

  // 4) 整条链都翻完了：置为 End() 哨兵
  rid_ = RowId();
  row_ = Row(rid_);
  return *this;
}

// 后缀 ++
TableIterator TableIterator::operator++(int) {
  TableIterator old(*this);   // 直接构造（拷贝构造是 explicit，不能用 =）
  ++(*this);
  return TableIterator(old);  // 同样：直接构造返回值
}
