#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) : num_pages_(num_pages) {}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 */
// 找出并驱逐最少使用的页
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  // 如果双向列表为空，那么没有可以驱逐的页
  if (lru_list_.empty()){
    return false; // 驱逐失败
  }

  *frame_id = lru_list_.back(); // least recently used的元素
  lru_list_.pop_back();         // 从链表移除
  map_.erase(*frame_id);        // 从哈希表移除

  // 移除成功，返回true
  return true;
}

/**
 * TODO: Student Implement
 */
// 固定页，从LRU中移除
void LRUReplacer::Pin(frame_id_t frame_id) {
  auto it = map_.find(frame_id); // 查找是否在LRU中
  if (it != map_.end()) {
    lru_list_.erase(it -> second); // 从链表移除
    map_.erase(it); // 从哈希表中移除
  }
}

/**
 * TODO: Student Implement
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  // 如果frame_id超范围的话，无效
  if (frame_id >= static_cast<int>(num_pages_)){
    return;
  }
  // 已经在LRU中，就不要重复添加
  if (map_.find(frame_id) != map_.end()){
    return;
  }

  lru_list_.push_front(frame_id); // 加到链表头部
  map_[frame_id] = lru_list_.begin(); // 记录到哈希值 
}

/**
 * TODO: Student Implement
 */
size_t LRUReplacer::Size() {
  return lru_list_.size(); // 返回链表中元素数量
}