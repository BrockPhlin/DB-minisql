#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  //首先加锁避免并发错乱
  std::scoped_lock<std::recursive_mutex> lock(latch_);

  // 1) 先查 page_table_：该 page 是否已经在内存
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    // 命中：直接 pin 这一帧并返回
    frame_id_t fid = it->second; // 取出对应的物理 frame
    pages_[fid].pin_count_++;    // 引用计数 +1
    replacer_->Pin(fid);         // 从 LRU 中摘掉，标记为不可淘汰
    return &pages_[fid];
  }
    // 2) 未命中：选一个 victim frame（free_list 优先，replacer 次之）
  frame_id_t fid = TryToFindFreePage();
  if (fid == INVALID_FRAME_ID) {
    // 没有任何可淘汰的页（free_list 空 + 所有页都还被 pin）
    return nullptr;
  }

  // 3) victim frame 之前的内容若是脏页，必须先写回磁盘
  if (pages_[fid].IsDirty()) {
    disk_manager_->WritePage(pages_[fid].GetPageId(), pages_[fid].GetData());
    pages_[fid].is_dirty_ = false;      // 写回后清脏标志
  }

  // 4) 把 victim frame 上原本对应的 page_id 从 page_table_ 移除
  //    （如果旧 page_id 是 INVALID_PAGE_ID，erase 是 no-op）
  page_table_.erase(pages_[fid].GetPageId());

  // 5) 把 frame 重新初始化为新的 page_id
  pages_[fid].page_id_   = page_id;     // 切换到新 page_id
  pages_[fid].pin_count_ = 1;           // FetchPage 默认会让调用方持有
  pages_[fid].is_dirty_  = false;       // 新读入的页尚未被修改
  // data_ 不用清零：下面的 ReadPage 会整页覆盖

  // 6) 把磁盘内容读入 frame 的 data_ 区域
  disk_manager_->ReadPage(page_id, pages_[fid].GetData());

  // 7) 建立新的 page_id -> frame_id 映射
  page_table_[page_id] = fid;

  // 8) 标记不可被 LRU 替换（pin_count > 0）
  replacer_->Pin(fid);

  return &pages_[fid];
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_);

  // 1) 选一个 victim frame（与 FetchPage 同策略）
  frame_id_t fid = TryToFindFreePage();
  if (fid == INVALID_FRAME_ID) {
    return nullptr;                     // 所有页都被 pin，分配失败
  }

  // 2) victim 旧内容若脏，先写回
  if (pages_[fid].IsDirty()) {
    disk_manager_->WritePage(pages_[fid].GetPageId(), pages_[fid].GetData());
    pages_[fid].is_dirty_ = false;
  }

  // 3) 移除旧映射
  page_table_.erase(pages_[fid].GetPageId());

  // 4) 向 DiskManager 申请一个新的逻辑页号（单调递增）
  page_id = AllocatePage();

  // 5) 重新初始化 frame
  pages_[fid].page_id_   = page_id;
  pages_[fid].pin_count_ = 1;           // NewPage 也让调用方持有
  pages_[fid].is_dirty_  = false;
  // data_ 不用清零：调用方通常会立刻写入

  // 6) 建立映射
  page_table_[page_id] = fid;

  // 7) 标记不可被替换
  replacer_->Pin(fid);

  return &pages_[fid];
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_);

  // 1) 查 page_table_
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return true;                        // 内存里没有，视为删除成功
  }
  frame_id_t fid = it->second;

  // 2) 还在被使用，禁止删除
  if (pages_[fid].pin_count_ > 0) {
    return false;
  }

  // 3) 脏页写回（与 FetchPage/NewPage 选 victim 时一致）
  if (pages_[fid].IsDirty()) {
    disk_manager_->WritePage(page_id, pages_[fid].GetData());
    pages_[fid].is_dirty_ = false;
  }

  // 4) 在磁盘上把这个逻辑页号释放（更新 DiskFileMetaPage 的位图）
  DeallocatePage(page_id);

  // 5) 把 frame 还原成"空 frame"：page_id_ 必须是 INVALID_PAGE_ID
  //    （按 Page 类注释，Page 不含物理页时 page_id_ 必须是 INVALID_PAGE_ID）
  page_table_.erase(it);
  pages_[fid].ResetMemory();            // 清空 data_（可选但更稳）
  pages_[fid].page_id_   = INVALID_PAGE_ID;
  pages_[fid].is_dirty_  = false;
  pages_[fid].pin_count_ = 0;

  // 6) 关键：frame 既放回 free_list_，也要从 LRU 摘掉（即便它不在 LRU 也安全）
  replacer_->Pin(fid);
  free_list_.push_back(fid);
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::scoped_lock<std::recursive_mutex> lock(latch_);

  // 1) 查 page_table_
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;                       // 不在内存
  }
  frame_id_t fid = it->second;

  // 2) 脏标志只能从 false 升到 true
  if (is_dirty) {
    pages_[fid].is_dirty_ = true;
  }

  // 3) 重复 unpin 是非法的
  if (pages_[fid].pin_count_ <= 0) {
    return false;
  }

  // 4) pin_count 减 1；只有降到 0 时才让 LRU 接管
  if (--pages_[fid].pin_count_ == 0) {
    replacer_->Unpin(fid);              // 加入 LRU，可被淘汰
  }
  return true;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_);

  // 1) 查 page_table_
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;                       // 不在内存
  }
  frame_id_t fid = it->second;

  // 2) 脏页才需要写回
  if (pages_[fid].IsDirty()) {
    disk_manager_->WritePage(page_id, pages_[fid].GetData());
    pages_[fid].is_dirty_ = false;      // 写完清脏
  }
  return true;
}

frame_id_t BufferPoolManager::TryToFindFreePage() {
  // 1) 优先用 free_list_
  if (!free_list_.empty()) {
    frame_id_t fid = free_list_.front();  // 取队首
    free_list_.pop_front();               // 从空闲链表移除
    return fid;
  }

  // 2) free_list_ 空了，尝试让 LRU 淘汰一个
  frame_id_t fid;
  if (replacer_->Victim(&fid)) {
    return fid;                           // Victim 会把 frame 从 LRU 摘掉
  }

  // 3) 没有任何可淘汰的页
  return INVALID_FRAME_ID;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}