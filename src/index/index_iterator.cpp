#include "index/index_iterator.h"

#include "index/basic_comparator.h"
#include "index/generic_key.h"

IndexIterator::IndexIterator() = default;

IndexIterator::IndexIterator(page_id_t page_id, BufferPoolManager *bpm, int index)
    : current_page_id(page_id), item_index(index), buffer_pool_manager(bpm) {
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    ASSERT(raw_page != nullptr, "Leaf page not found when constructing index iterator.");
    page = reinterpret_cast<LeafPage *>(raw_page->GetData());
  }
}

IndexIterator::IndexIterator(const IndexIterator &other)
    : current_page_id(other.current_page_id),
      item_index(other.item_index),
      buffer_pool_manager(other.buffer_pool_manager) {
  // 迭代器持有 pinned 的叶子页。拷贝时不能直接复制 page 指针，
  // 否则两个迭代器析构时会对同一个 pin 重复 Unpin
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    ASSERT(raw_page != nullptr, "Leaf page not found when copying index iterator.");
    page = reinterpret_cast<LeafPage *>(raw_page->GetData());
  }
}

IndexIterator::IndexIterator(IndexIterator &&other) noexcept
    : current_page_id(other.current_page_id),
      page(other.page),
      item_index(other.item_index),
      buffer_pool_manager(other.buffer_pool_manager) {
  // 移动构造直接转移 pin 的所有权，源迭代器置为 End 哨兵，避免析构时重复 Unpin
  other.current_page_id = INVALID_PAGE_ID;
  other.page = nullptr;
  other.item_index = 0;
  other.buffer_pool_manager = nullptr;
}

IndexIterator::~IndexIterator() {
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr)
    buffer_pool_manager->UnpinPage(current_page_id, false);
}

IndexIterator &IndexIterator::operator=(const IndexIterator &other) {
  if (this == &other) {
    return *this;
  }

  // 先释放当前持有的页，再像拷贝构造一样为新位置单独 Fetch 一次。
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    buffer_pool_manager->UnpinPage(current_page_id, false);
  }

  current_page_id = other.current_page_id;
  page = nullptr;
  item_index = other.item_index;
  buffer_pool_manager = other.buffer_pool_manager;

  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    ASSERT(raw_page != nullptr, "Leaf page not found when assigning index iterator.");
    page = reinterpret_cast<LeafPage *>(raw_page->GetData());
  }
  return *this;
}

IndexIterator &IndexIterator::operator=(IndexIterator &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    buffer_pool_manager->UnpinPage(current_page_id, false);
  }

  current_page_id = other.current_page_id;
  page = other.page;
  item_index = other.item_index;
  buffer_pool_manager = other.buffer_pool_manager;

  other.current_page_id = INVALID_PAGE_ID;
  other.page = nullptr;
  other.item_index = 0;
  other.buffer_pool_manager = nullptr;
  return *this;
}

std::pair<GenericKey *, RowId> IndexIterator::operator*() {
  ASSERT(current_page_id != INVALID_PAGE_ID && page != nullptr, "Dereference of end index iterator.");
  ASSERT(item_index >= 0 && item_index < page->GetSize(), "Index iterator points outside the leaf page.");
  return page->GetItem(item_index);
}

IndexIterator &IndexIterator::operator++() {
  ASSERT(current_page_id != INVALID_PAGE_ID && page != nullptr, "Increment of end index iterator.");

  item_index++;
  if (item_index < page->GetSize()) {
    // 仍然在当前叶子页内部，直接前进到下一个 key/value。
    return *this;
  }

  // 当前叶子页已经走完，沿叶子链表跳到后继叶子页。
  page_id_t next_page_id = page->GetNextPageId();
  buffer_pool_manager->UnpinPage(current_page_id, false);
  current_page_id = INVALID_PAGE_ID;
  page = nullptr;
  item_index = 0;

  // 正常情况下叶子页不会为空；这里写成循环，是为了在删除后留下空页等异常状态下也能安全跳过。
  while (next_page_id != INVALID_PAGE_ID) {
    Page *next_page = buffer_pool_manager->FetchPage(next_page_id);
    ASSERT(next_page != nullptr, "Next leaf page not found when advancing index iterator.");
    auto *next_leaf = reinterpret_cast<LeafPage *>(next_page->GetData());

    current_page_id = next_page_id;
    page = next_leaf;
    item_index = 0;
    if (page->GetSize() > 0) {
      return *this;
    }

    next_page_id = page->GetNextPageId();
    buffer_pool_manager->UnpinPage(current_page_id, false);
    current_page_id = INVALID_PAGE_ID;
    page = nullptr;
  }

  // 没有后继叶子页，当前迭代器变成与 BPlusTree::End() 相同的哨兵状态。
  return *this;
}

bool IndexIterator::operator==(const IndexIterator &itr) const {
  return current_page_id == itr.current_page_id && item_index == itr.item_index;
}

bool IndexIterator::operator!=(const IndexIterator &itr) const {
  return !(*this == itr);
}
