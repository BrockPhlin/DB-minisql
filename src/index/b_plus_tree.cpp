#include "index/b_plus_tree.h"

#include <algorithm>
#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager,
                    const KeyManager &KM, int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
    // 当调用者没有显式指定页容量时，根据页头大小和 key / value pair 的真实宽度计算
    // -1 是为了给插入后的临时 overflow 留出一个槽位
    // 叶子页/内部页会先插入到当前页，再在 size > max_size 时分裂
    if (leaf_max_size_ == UNDEFINED_SIZE) {
        leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(RowId)) - 1;
    }
    if (internal_max_size_ == UNDEFINED_SIZE) {
        internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (processor_.GetKeySize() + sizeof(page_id_t)) - 1;
    }

    // INDEX_ROOTS_PAGE_ID 保存 index_id -> root_page_id 的映射
    // 如果这个索引以前已经创建过，就从这里恢复根页；否则保持 INVALID_PAGE_ID，表示空树
    Page *page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
    if (page != nullptr) {
        auto *root_page = reinterpret_cast<IndexRootsPage *>(page->GetData());
        page_id_t root_id = INVALID_PAGE_ID;
        if (root_page->GetRootId(index_id_, &root_id)) {
            root_page_id_ = root_id;
        }
        buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
    }
}

void BPlusTree::Destroy(page_id_t current_page_id) {
    if (current_page_id == INVALID_PAGE_ID) {
        current_page_id = root_page_id_;
    }
    if (current_page_id == INVALID_PAGE_ID) {
        return; // 树已经是空的了
    }
    Page *page = buffer_pool_manager_->FetchPage(current_page_id);
    if (page == nullptr) return;  // FetchPage 失败，提前返回
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    // 是内部节点，先递归销毁所有子节点
    if (!node->IsLeafPage()) {
        auto *internal = reinterpret_cast<InternalPage *>(node);
        for (int i = 0; i < internal->GetSize(); i++) {
            Destroy(internal->ValueAt(i));
        }
    }
    // 是根节点，重置 root_page_id_ 并更新 index_roots_page
    if (node->IsRootPage()) {
        root_page_id_ = INVALID_PAGE_ID;
        UpdateRootPageId();
    }
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    buffer_pool_manager_->DeletePage(current_page_id);
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
    return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
    // 空树
    if (IsEmpty()) return false;

    Page *leaf_page = FindLeafPage(key);

    // leaf_page 为 nullptr 时不需要 unpin
    if (leaf_page == nullptr) return false;

    // Page 中的 data_ 存的是 BPlusTreeLeafPage
    auto *leaf_node = reinterpret_cast<LeafPage *>(leaf_page->GetData());

    // 如果查到了，就把对应 RowId 写入 value 并返回 true
    RowId value;
    bool found = leaf_node->Lookup(key, value, processor_);

    // unpin
    buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), false);

    // 找到 key 时，才把 RowId 加进去
    if (found) {
        result.push_back(value);
    }

    return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
    // 树为空时，直接创建新的根叶子页并插入第一条记录
    if (IsEmpty()) {
        StartNewTree(key, value);
        return true;
    }
    
    // 树非空，找合适的叶子页插入。如果 key 已存在则返回 false
    return InsertIntoLeaf(key, value, transaction);
}

/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
    page_id_t root_page_id = INVALID_PAGE_ID;
    Page *page = buffer_pool_manager_->NewPage(root_page_id);
    ASSERT(page != nullptr, "Out of memory when creating a new B+Tree root page.");

    // 空树的第一个页一定是叶子页，同时也是根页。
    auto *root = reinterpret_cast<LeafPage *>(page->GetData());
    root->Init(root_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
    root->Insert(key, value, processor_);

    // root_page_id_ 是 BPlusTree 对象的内存状态，IndexRootsPage 是持久化目录。
    // 新建索引时需要插入一条新的 root 记录。
    root_page_id_ = root_page_id;
    UpdateRootPageId(1);

    // NewPage 返回的页处于 pinned 状态；初始化和插入都修改了页内容，需要标记 dirty。
    buffer_pool_manager_->UnpinPage(root_page_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
    Page *page = FindLeafPage(key);
    if (page == nullptr) {
        return false;
    }

    auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
    RowId old_value;
    if (leaf->Lookup(key, old_value, processor_)) {
        // 本实验的 B+ 树只支持 unique key；重复 key 不能插入。
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
        return false;
    }

    leaf->Insert(key, value, processor_);
    if (leaf->GetSize() <= leaf->GetMaxSize()) {
        // 未溢出时只需要写回当前叶子页。
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
        return true;
    }

    // 叶子页溢出：把后一半 key/value 移到新叶子页，再把新叶子的最小 key 插入父节点。
    LeafPage *new_leaf = Split(leaf, transaction);
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);

    // Split 返回的新页仍然 pinned；原叶子页来自 FindLeafPage，也仍然 pinned。
    // InsertIntoParent 只负责父链调整，不接管这两个叶子页的 pin 生命周期。
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
    return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
    page_id_t new_page_id = INVALID_PAGE_ID;
    Page *page = buffer_pool_manager_->NewPage(new_page_id);
    ASSERT(page != nullptr, "Out of memory when splitting an internal page.");

    auto *new_node = reinterpret_cast<InternalPage *>(page->GetData());
    new_node->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);

    // MoveHalfTo 会把后一半 child 指针移动到 new_node，并把这些 child 的 parent 改成 new_node。
    // 内部页的第 0 个 key 是占位 key；分裂后 new_node->KeyAt(0) 会作为向父节点提升的分隔 key 使用。
    node->MoveHalfTo(new_node, buffer_pool_manager_);
    return new_node;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
    page_id_t new_page_id = INVALID_PAGE_ID;
    Page *page = buffer_pool_manager_->NewPage(new_page_id);
    ASSERT(page != nullptr, "Out of memory when splitting a leaf page.");

    auto *new_node = reinterpret_cast<LeafPage *>(page->GetData());
    new_node->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);

    // 叶子页之间通过 next_page_id_ 组成有序链表。分裂时，新叶子插入到 node 与原 next 之间。
    page_id_t old_next_page_id = node->GetNextPageId();
    node->MoveHalfTo(new_node);
    new_node->SetNextPageId(old_next_page_id);
    node->SetNextPageId(new_page_id);
    return new_node;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node,
                                 Txn *transaction) {
    if (old_node->IsRootPage()) {
        page_id_t new_root_page_id = INVALID_PAGE_ID;
        Page *page = buffer_pool_manager_->NewPage(new_root_page_id);
        ASSERT(page != nullptr, "Out of memory when creating a new B+Tree root page.");

        auto *new_root = reinterpret_cast<InternalPage *>(page->GetData());
        new_root->Init(new_root_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
        new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

        // old_node 与 new_node 原来同层；新根生成后，它们都成为新根的孩子。
        old_node->SetParentPageId(new_root_page_id);
        new_node->SetParentPageId(new_root_page_id);
        root_page_id_ = new_root_page_id;
        UpdateRootPageId();

        buffer_pool_manager_->UnpinPage(new_root_page_id, true);
        return;
    }

    // 非根分裂：把 <key, new_node_page_id> 插到 old_node 在父节点中的右侧。
    page_id_t parent_page_id = old_node->GetParentPageId();
    Page *parent_page = buffer_pool_manager_->FetchPage(parent_page_id);
    ASSERT(parent_page != nullptr, "Parent page not found during B+Tree insertion.");
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    new_node->SetParentPageId(parent_page_id);

    if (parent->GetSize() <= parent->GetMaxSize()) {
        buffer_pool_manager_->UnpinPage(parent_page_id, true);
        return;
    }

    // 父节点也溢出时继续向上分裂。递归调用不负责释放 parent/new_internal，
    // 所以本层在递归返回后统一 unpin。
    InternalPage *new_internal = Split(parent, transaction);
    InsertIntoParent(parent, new_internal->KeyAt(0), new_internal, transaction);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_internal->GetPageId(), true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
    if (IsEmpty()) {
        return;
    }

    Page *page = FindLeafPage(key);
    if (page == nullptr) {
        return;
    }

    auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
    int old_size = leaf->GetSize();
    int new_size = leaf->RemoveAndDeleteRecord(key, processor_);
    if (new_size == old_size) {
        // key 不存在，页面没有变化。
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
        return;
    }

    if (leaf->GetSize() >= leaf->GetMinSize()) {
        // 删除后仍然满足半满约束，直接写回当前叶子页。
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
        return;
    }

    // 删除导致 underflow：CoalesceOrRedistribute 会负责释放 leaf 及递归过程中访问到的页。
    CoalesceOrRedistribute(leaf, transaction);
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
    if (node->IsRootPage()) {
        // 根节点允许低于普通节点的 min size。
        // 如果根只剩一个孩子或空叶子，AdjustRoot 会把树高降低或置为空树。
        page_id_t old_root_page_id = node->GetPageId();
        bool should_delete_root = AdjustRoot(node);
        buffer_pool_manager_->UnpinPage(old_root_page_id, true);
        if (should_delete_root) {
            buffer_pool_manager_->DeletePage(old_root_page_id);
        }
        return should_delete_root;
    }

    if (node->GetSize() >= node->GetMinSize()) {
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        return false;
    }

    Page *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
    ASSERT(parent_page != nullptr, "Parent page not found during B+Tree deletion.");
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    int index = parent->ValueIndex(node->GetPageId());
    ASSERT(index >= 0, "Child page is not found in its parent.");

    // 优先选择左兄弟；如果当前节点是最左孩子，只能选择右兄弟。
    int neighbor_index = index == 0 ? 1 : index - 1;
    Page *neighbor_page = buffer_pool_manager_->FetchPage(parent->ValueAt(neighbor_index));
    ASSERT(neighbor_page != nullptr, "Sibling page not found during B+Tree deletion.");
    auto *neighbor_node = reinterpret_cast<N *>(neighbor_page->GetData());

    if (node->GetSize() + neighbor_node->GetSize() <= node->GetMaxSize()) {
        // 合并时统一保持“neighbor 在左，node 在右”，这样把 node 追加到 neighbor 后仍然有序。
        // 如果当前 node 是最左孩子，就交换两者，让右兄弟成为待删除的 node。
        if (index == 0) {
            std::swap(node, neighbor_node);
            index = 1;
        }
        return Coalesce(neighbor_node, node, parent, index, transaction);
    }

    // 兄弟节点有富余元素，借一个即可；父节点的分隔 key 会在 Redistribute 中同步更新。
    Redistribute(neighbor_node, node, index);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), false);
    return false;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
    page_id_t node_page_id = node->GetPageId();

    // node 是右兄弟，neighbor_node 是左兄弟；MoveAllTo 会把 node 的所有 key/value
    // 追加到左兄弟尾部，并把左兄弟的 next_page_id 指向 node 原来的后继页。
    node->MoveAllTo(neighbor_node);
    parent->Remove(index);

    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(node_page_id, true);
    buffer_pool_manager_->DeletePage(node_page_id);

    if (parent->GetSize() < parent->GetMinSize()) {
        // 父节点少了一个 child 指针，可能继续 underflow，需要向上递归。
        return CoalesceOrRedistribute(parent, transaction);
    }

    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return true;
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
    page_id_t node_page_id = node->GetPageId();
    int old_neighbor_size = neighbor_node->GetSize();

    // 内部页合并时，父节点中夹在两个孩子之间的 separator key 需要下沉。
    // 下沉后的 key 放在左兄弟原末尾位置，用来分隔左兄弟原最后一个孩子和右兄弟第一个孩子。
    neighbor_node->SetKeyAt(old_neighbor_size, parent->KeyAt(index));

    for (int i = 0; i < node->GetSize(); i++) {
        int dest = old_neighbor_size + i;
        if (i > 0) {
            neighbor_node->SetKeyAt(dest, node->KeyAt(i));
        }
        neighbor_node->SetValueAt(dest, node->ValueAt(i));

        // 所有被搬到左兄弟的孩子，其 parent_page_id 都必须改成左兄弟页号。
        Page *child_page = buffer_pool_manager_->FetchPage(node->ValueAt(i));
        ASSERT(child_page != nullptr, "Child page not found during internal page merge.");
        auto *child = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
        child->SetParentPageId(neighbor_node->GetPageId());
        buffer_pool_manager_->UnpinPage(child->GetPageId(), true);
    }
    neighbor_node->SetSize(old_neighbor_size + node->GetSize());
    parent->Remove(index);

    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(node_page_id, true);
    buffer_pool_manager_->DeletePage(node_page_id);

    if (parent->GetSize() < parent->GetMinSize()) {
        return CoalesceOrRedistribute(parent, transaction);
    }

    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
    return true;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
    Page *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
    ASSERT(parent_page != nullptr, "Parent page not found during leaf redistribution.");
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    if (index == 0) {
        // node 是最左孩子，只能从右兄弟借最小 key，追加到 node 尾部。
        // 右兄弟的新最小 key 需要写回父节点作为新的 separator。
        neighbor_node->MoveFirstToEndOf(node);
        parent->SetKeyAt(1, neighbor_node->KeyAt(0));
    } else {
        // node 有左兄弟，从左兄弟借最大 key，插入到 node 头部。
        // node 的新最小 key 就是父节点对应 separator。
        neighbor_node->MoveLastToFrontOf(node);
        parent->SetKeyAt(index, node->KeyAt(0));
    }

    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}

void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
    Page *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
    ASSERT(parent_page != nullptr, "Parent page not found during internal redistribution.");
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

    if (index == 0) {
        // 从右兄弟借第一个 child 到 node 尾部。
        // 原父 separator 下移到 node；右兄弟原来的第一个有效 key 上移为新的 separator。
        neighbor_node->MoveFirstToEndOf(node, parent->KeyAt(1), buffer_pool_manager_);
        parent->SetKeyAt(1, neighbor_node->KeyAt(0));
    } else {
        // 从左兄弟借最后一个 child 到 node 头部。
        // 左兄弟最后一个 key 上移为新的父 separator，原父 separator 下移到 node。
        GenericKey *new_parent_key = neighbor_node->KeyAt(neighbor_node->GetSize() - 1);
        neighbor_node->MoveLastToFrontOf(node, parent->KeyAt(index), buffer_pool_manager_);
        parent->SetKeyAt(index, new_parent_key);
    }

    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
    if (old_root_node->IsLeafPage()) {
        // 根叶子页没有任何 key/value 时，整棵树变为空树。
        if (old_root_node->GetSize() == 0) {
            root_page_id_ = INVALID_PAGE_ID;
            UpdateRootPageId();
            return true;
        }
        return false;
    }

    auto *root = reinterpret_cast<InternalPage *>(old_root_node);
    if (root->GetSize() == 1) {
        // 内部根节点只剩一个孩子时，树高可以降低一层：唯一孩子成为新根。
        page_id_t new_root_page_id = root->RemoveAndReturnOnlyChild();
        Page *new_root_page = buffer_pool_manager_->FetchPage(new_root_page_id);
        ASSERT(new_root_page != nullptr, "New root page not found during root adjustment.");
        auto *new_root = reinterpret_cast<BPlusTreePage *>(new_root_page->GetData());
        new_root->SetParentPageId(INVALID_PAGE_ID);
        buffer_pool_manager_->UnpinPage(new_root_page_id, true);

        root_page_id_ = new_root_page_id;
        UpdateRootPageId();
        return true;
    }

    return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
    if (IsEmpty()) {
        return End();
    }

    Page *page = FindLeafPage(nullptr, INVALID_PAGE_ID, true);
    if (page == nullptr) {
        return End();
    }

    auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
    page_id_t leaf_page_id = leaf->GetPageId();
    if (leaf->GetSize() == 0) {
        buffer_pool_manager_->UnpinPage(leaf_page_id, false);
        return End();
    }

    // FindLeafPage 返回的页已经 pinned；IndexIterator 构造函数会重新 Fetch 并持有自己的 pin。
    buffer_pool_manager_->UnpinPage(leaf_page_id, false);
    return IndexIterator(leaf_page_id, buffer_pool_manager_, 0);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
    if (IsEmpty()) {
        return End();
    }

    Page *page = FindLeafPage(key);
    if (page == nullptr) {
        return End();
    }

    auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
    int index = leaf->KeyIndex(key, processor_);

    // lower_bound 可能落在当前叶子的末尾。此时继续沿叶子链找下一个非空叶子；
    // 如果已经没有后继叶子，则返回 End()。
    while (index >= leaf->GetSize()) {
        page_id_t current_page_id = leaf->GetPageId();
        page_id_t next_page_id = leaf->GetNextPageId();
        buffer_pool_manager_->UnpinPage(current_page_id, false);
        if (next_page_id == INVALID_PAGE_ID) {
            return End();
        }

        page = buffer_pool_manager_->FetchPage(next_page_id);
        ASSERT(page != nullptr, "Next leaf page not found when creating lower-bound iterator.");
        leaf = reinterpret_cast<LeafPage *>(page->GetData());
        index = 0;
    }

    page_id_t leaf_page_id = leaf->GetPageId();
    buffer_pool_manager_->UnpinPage(leaf_page_id, false);
    return IndexIterator(leaf_page_id, buffer_pool_manager_, index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  // Default iterator has INVALID_PAGE_ID and represents the scan sentinel.
  return IndexIterator();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
    // 如果调用者没有指定起始页，就默认从整棵树的根节点开始找。
    page_id_t current_page_id = page_id == INVALID_PAGE_ID ? root_page_id_ : page_id;

    // root_page_id_ 也是 INVALID_PAGE_ID 时，说明当前 B+ 树为空，没有叶子页可返回。
    if (current_page_id == INVALID_PAGE_ID) {
        return nullptr;
    }

    // FetchPage 会把页面 pin 住，后面不再使用某个内部页时必须 unpin。
    // 最后找到的叶子页不能在这里 unpin，因为调用者还要读取它。
    Page *page = buffer_pool_manager_->FetchPage(current_page_id);
    while (page != nullptr) {
        auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

        // B+ 树所有真实的 key/value 都在叶子页里。
        // 找到叶子页后直接返回，并保持 pinned 状态，交给调用者用完后 unpin。
        if (node->IsLeafPage()) {
            return page;
        }

        // 现在还在内部页，需要根据查找模式选择下一层孩子页：
        // 1. leftMost == true，一直走第 0 个孩子，最终得到最左叶子页。
        // 2. leftMost == false，根据 key 在内部页中二分查找应该进入的孩子。
        auto *internal = reinterpret_cast<InternalPage *>(node);
        page_id_t next_page_id = leftMost ? internal->ValueAt(0) : internal->Lookup(key, processor_);

        // 当前内部页的作用已经完成，下一轮只需要访问子页。
        // 这里只是读路径，不修改页面内容，所以 dirty 参数传 false。
        buffer_pool_manager_->UnpinPage(current_page_id, false);

        // 继续向下一层走。FetchPage 后，新的 page 同样处于 pinned 状态。
        current_page_id = next_page_id;
        page = buffer_pool_manager_->FetchPage(current_page_id);
    }

    // 正常情况下不会走到这里；除非 FetchPage 失败，比如页号无效或缓冲池取页失败。
    return nullptr;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
    Page *page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
    if (page == nullptr) return;
    auto *root_page = reinterpret_cast<IndexRootsPage *>(page->GetData());
    if (insert_record != 0) {
        // 插入新记录
        root_page->Insert(index_id_, root_page_id_);
    } else {
        if (root_page_id_ == INVALID_PAGE_ID) {
            // 树被删了，删除记录
            root_page->Delete(index_id_);
        } else {
            // 更新根页ID
            root_page->Update(index_id_, root_page_id_);
        }
    }
    buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}
