#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()

/**
 * TODO: Student Implement
 */
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
    SetPageType(IndexPageType::INTERNAL_PAGE);  // set page type
    SetKeySize(key_size);
    SetLSN();
    SetSize(0);
    SetMaxSize(max_size);
    SetParentPageId(parent_id);
    SetPageId(page_id);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value)
      return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * pair_size);
}
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 * 用了二分查找
 */
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
    int left = 1, right = GetSize() - 1;
    int mid = (left + right) / 2;
    while (left <= right){
        if (KM.CompareKeys(key, KeyAt(mid)) > 0){ // 左 > 右
            left = mid + 1;
        }else if (KM.CompareKeys(key, KeyAt(mid)) < 0){
            right = mid - 1;
        }else{
            return ValueAt(mid);
        }
        mid = (left + right) / 2;
    }
    // 走到这说明没找到
    return ValueAt(right);
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
    SetValueAt(0, old_value);
    SetValueAt(1, new_value);
    SetKeyAt(1, new_key);
    SetSize(2);
}

/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
    int old_index = ValueIndex(old_value);
    if (old_index == -1) {
        return GetSize();  // old_value 不存在
    }
    
    int index = old_index + 1;
    int size = GetSize();
    
    // 将从 index 到末尾的所有元素向后移动一位
    for (int i = size - 1; i >= index; i--) {
        PairCopy(PairPtrAt(i + 1), PairPtrAt(i));
    }
    
    SetValueAt(index, new_value);
    SetKeyAt(index, new_key);
    SetSize(size + 1);
    return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 * buffer_pool_manager 是干嘛的？传给CopyNFrom()用于Fetch数据页
 */
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
    int total_size = GetSize();
    int start_index = total_size / 2;
    int move_count = total_size - start_index;
    
    // 将后半部分拷贝到 recipient
    recipient->CopyNFrom(PairPtrAt(start_index), move_count, buffer_pool_manager);
    
    // 更新当前页的大小
    SetSize(start_index);
}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger
 *
 */
void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
    int old_size = GetSize();
    
    // 将 size 个 pair 拷贝到当前页末尾
    PairCopy(PairPtrAt(old_size), src, size);
    
    // 更新当前页大小
    SetSize(old_size + size);
    
    // 更新所有被移动的子节点的父指针
    for (int i = old_size; i < GetSize(); i++) {
        page_id_t child_page_id = ValueAt(i);
        Page *page = buffer_pool_manager->FetchPage(child_page_id);
        if (page != nullptr) {
            auto *child = reinterpret_cast<BPlusTreePage *>(page->GetData());
            child->SetParentPageId(GetPageId());
            buffer_pool_manager->UnpinPage(child_page_id, true);
        }
    }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
void InternalPage::Remove(int index) {
    int size = GetSize();
    for (int i = index; i < size - 1; i++) {
        PairCopy(PairPtrAt(i), PairPtrAt(i + 1));
    }
    SetSize(size - 1);
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 */
page_id_t InternalPage::RemoveAndReturnOnlyChild() {
    page_id_t only_child = ValueAt(0);
    SetSize(0);
    return only_child;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id
 * for those pages that are moved to the recipient
 */
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key,
                            BufferPoolManager *buffer_pool_manager) {
    // set middle key at recipient's current end
    int size = recipient->GetSize();
    recipient->SetKeyAt(size, middle_key);
    
    // move all pairs from current page to recipient
    recipient->CopyNFrom(PairPtrAt(0), GetSize(), buffer_pool_manager);

    // clear current page
    SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id
 * for those pages that are moved to the recipient
 */
void InternalPage::MoveFirstToEndOf(InternalPage *recipient,
    GenericKey *middle_key, BufferPoolManager *buffer_pool_manager) {
    // 把 middle_key 和第一个 value 追加到 recipient 末尾
    page_id_t first_value = ValueAt(0);
    recipient->CopyLastFrom(middle_key, first_value, buffer_pool_manager);
    
    // 删除当前节点的第一个元素
    Remove(0);
}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value,
                                BufferPoolManager *buffer_pool_manager) {
    // 在末尾追加一个键值对
    int size = GetSize();
    SetKeyAt(size, key);
    SetValueAt(size, value);
    SetSize(size + 1);
    
    // 更新被移动的子节点的父指针
    Page *page = buffer_pool_manager->FetchPage(value);
    if (page != nullptr) {
        auto *child = reinterpret_cast<BPlusTreePage *>(page->GetData());
        child->SetParentPageId(GetPageId());
        buffer_pool_manager->UnpinPage(value, true);
    }
}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array to position the middle_key at the
 * right place.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those pages that are
 * moved to the recipient
 */
void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {
    // 获取当前节点最后一个元素的value
    page_id_t last_value = ValueAt(GetSize() - 1);
    
    // 调用recipient的CopyFirstFrom，把last_value插入到开头
    recipient->CopyFirstFrom(last_value, buffer_pool_manager);
    
    // 设置middle_key到recipient的位置1（因为位置0是INVALID）
    recipient->SetKeyAt(1, middle_key);
    
    // 删除当前节点的最后一个元素
    SetSize(GetSize() - 1);
}

/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyFirstFrom(const page_id_t value, BufferPoolManager *buffer_pool_manager) {
    // 将所有元素向后移动一位（从位置1开始，因为位置0的键是INVALID）
    int size = GetSize();
    for (int i = size; i > 0; i--) {
        PairCopy(PairPtrAt(i), PairPtrAt(i - 1));
    }
    
    // 在位置0设置新的value
    SetValueAt(0, value);
    SetSize(size + 1);
    
    // 更新被移动的子节点的父指针
    Page *page = buffer_pool_manager->FetchPage(value);
    if (page != nullptr) {
        auto *child = reinterpret_cast<BPlusTreePage *>(page->GetData());
        child->SetParentPageId(GetPageId());
        buffer_pool_manager->UnpinPage(value, true);
    }
}