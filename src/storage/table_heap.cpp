#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  uint32_t size = row.GetSerializedSize(schema_);
  if (size > TablePage::SIZE_MAX_ROW) return false;

  // If heap is empty, allocate the first page.
  if (first_page_id_ == INVALID_PAGE_ID) {
    page_id_t new_pid;
    auto new_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_pid));
    if (new_page == nullptr) return false;
    new_page->Init(new_pid, INVALID_PAGE_ID, log_manager_, txn);
    first_page_id_ = new_pid;
    buffer_pool_manager_->UnpinPage(new_pid, true);
  }

  // First-fit: walk the page chain looking for space.
  page_id_t pid = first_page_id_;
  page_id_t last_pid = INVALID_PAGE_ID;
  while (pid != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(pid));
    if (page == nullptr) return false;
    page->WLatch();
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      page->WUnlatch();
      buffer_pool_manager_->UnpinPage(pid, true);
      return true;
    }
    page->WUnlatch();
    last_pid = pid;
    page_id_t next = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(pid, false);
    pid = next;
  }

  // No page had space: allocate a new page and link it at the end of the chain.
  page_id_t new_pid;
  auto new_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->NewPage(new_pid));
  if (new_page == nullptr) return false;
  new_page->Init(new_pid, last_pid == INVALID_PAGE_ID ? first_page_id_ : last_pid,
                  log_manager_, txn);

  // Link the new page after `last_pid` (or set it as the first page if still empty).
  if (last_pid == INVALID_PAGE_ID) {
    first_page_id_ = new_pid;
  } else {
    auto last_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(last_pid));
    last_page->SetNextPageId(new_pid);
    buffer_pool_manager_->UnpinPage(last_pid, true);
  }

  new_page->WLatch();
  bool ok = new_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
  new_page->WUnlatch();
  buffer_pool_manager_->UnpinPage(new_pid, true);
  return ok;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  // Otherwise, mark the tuple as deleted.
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

/**
 * TODO: Student Implement
 */
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) return false;
  page->WLatch();

  Row old_row(rid);
  if (page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_)) {
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
    return true;
  }

  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), false);

  ApplyDelete(rid, txn);
  return InsertTuple(row, txn);
}

/**
 * TODO: Student Implement
 */
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) return;
  page->WLatch();
  page->ApplyDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * TODO: Student Implement
 */
bool TableHeap::GetTuple(Row *row, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(
      buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) return false;
  page->RLatch();
  bool ok = page->GetTuple(row, schema_, txn, lock_manager_);
  page->RUnlatch();
  buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
  return ok;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::Begin(Txn *txn) {
  page_id_t pid = first_page_id_;
  while (pid != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(pid));
    RowId rid;
    if (page->GetFirstTupleRid(&rid)) {
      return TableIterator(this, rid, txn);
    }
    page_id_t next = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(pid, false);
    pid = next;
  }
  return End();
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::End() {
  return TableIterator(this, RowId(), nullptr);
}
