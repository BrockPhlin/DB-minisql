#include "storage/disk_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <stdexcept>

#include "glog/logging.h"
#include "page/bitmap_page.h"

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  // directory or file does not exist
  if (!db_io_.is_open()) {
    db_io_.clear();
    // create a new file
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
    db_io_.close();
    // reopen with original mode
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      throw std::exception();
    }
  }
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
}

void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  if (!closed) {
    db_io_.close();
    closed = true;
  }
}

void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * TODO: Student Implement
 * 从磁盘中分配一个空闲数据页。
 * @return 分配到的逻辑页号；磁盘已满时返回 INVALID_PAGE_ID
 */
page_id_t DiskManager::AllocatePage() {
  // 把 4KB 内存缓冲 reinterpret 成元数据页对象（零拷贝，二进制布局一致）
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  // 用"已分配页总数"作为下一个可分配的逻辑页号 —— 这保证返回的页号总是递增
  uint32_t allocated_page_id = meta->num_allocated_pages_;

  // 现有 extent 还能容纳的页数
  uint32_t max_supported_pages = meta->num_extents_ * BITMAP_SIZE;
  if (allocated_page_id >= max_supported_pages) {
    // 所有现有 extent 都用完了，看 meta 页是否还有空位能再开新 extent
    // meta 页能记的 extent 数：(PAGE_SIZE - 8 字节计数器) / 4 字节每项 = 1024
    if (meta->num_extents_ >= (PAGE_SIZE - 8) / 4) {
      return INVALID_PAGE_ID;   // meta 也满了，整个数据库文件容量达到上限
    }
    // 在物理上开新 extent：先写一个全 0 的 bitmap 页（占一个物理页）
    // 物理位置 = 1（meta） + num_extents_ * (BITMAP_SIZE + 1)（前面 extent 占的物理页）
    page_id_t new_bitmap_physical_page_id = 1 + meta->num_extents_ * (BITMAP_SIZE + 1);
    char empty_page[PAGE_SIZE] = {0};
    WritePhysicalPage(new_bitmap_physical_page_id, empty_page);
    meta->num_extents_++;
  }

  // 定位当前逻辑页所在的 extent
  uint32_t extent_id = allocated_page_id / BITMAP_SIZE;
  // 该 extent 的 bitmap 页在物理文件中的位置
  page_id_t bitmap_physical_page_id = 1 + extent_id * (BITMAP_SIZE + 1);

  // 读 - 改 - 写：把 4KB bitmap 读进栈上缓冲，reinterpret 成位图页对象
  char bitmap_data[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page_id, bitmap_data);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_data);

  // 调位图页 API：找一个空闲位并标记为已分配（bit 从 0 变 1）
  uint32_t page_offset;
  if (!bitmap->AllocatePage(page_offset)) {
    return INVALID_PAGE_ID;   // 该 extent 真的满了（理论上前面已开新 extent，不会走到这里）
  }

  // 把修改后的 bitmap 整页写回磁盘
  WritePhysicalPage(bitmap_physical_page_id, bitmap_data);

  // 更新元数据：全局计数 + 当前段已用页数
  meta->num_allocated_pages_++;
  meta->extent_used_page_[extent_id]++;

  return allocated_page_id;   // 返回的是逻辑页号，不是 page_offset
}

/**
 * TODO: Student Implement
 */
void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_); // 获取meta指针

  if (logical_page_id >= meta->num_allocated_pages_) { // 检查页号是否有效
    return;
  }

  uint32_t extent_id = logical_page_id / BITMAP_SIZE; // 该页属于哪个extent
  uint32_t offset_in_extent = logical_page_id % BITMAP_SIZE; // 在extent内的偏移

  page_id_t bitmap_physical_page_id = 1 + extent_id * (BITMAP_SIZE + 1); // bitmap页的物理位置
  char bitmap_data[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page_id, bitmap_data);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_data);

  bitmap->DeAllocatePage(offset_in_extent); // 在bitmap中将该位置为0（空闲）
  WritePhysicalPage(bitmap_physical_page_id, bitmap_data);

  meta->num_allocated_pages_--; // 已分配页数-1
  meta->extent_used_page_[extent_id]--; // 该extent已用页数-1
}

/**
 * TODO: Student Implement
 */
bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;

  if (extent_id >= meta->num_extents_) {
    return true;  // extent不存在 → 从未分配过 → 空闲
  }

  page_id_t bitmap_physical_page_id = 1 + extent_id * (BITMAP_SIZE + 1);
  char bitmap_data[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page_id, bitmap_data);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_data);
  uint32_t offset_in_extent = logical_page_id % BITMAP_SIZE;
  return bitmap->IsPageFree(offset_in_extent);
}

page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;          // 计算逻辑页所造的extent编号
  uint32_t offset_in_extent = logical_page_id % BITMAP_SIZE;   // 计算逻辑页在extent中的偏移量
  return 1 + extent_id * (BITMAP_SIZE + 1) + offset_in_extent + 1; // 加的是disk_file_meta和bitmap_page（1+1=2）
}

int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // check if read beyond file length
  if (offset >= GetFileSize(file_name_)) {
#ifdef ENABLE_BPM_DEBUG
    LOG(INFO) << "Read less than a page" << std::endl;
#endif
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // set read cursor to offset
    db_io_.seekp(offset);
    db_io_.read(page_data, PAGE_SIZE);
    // if file ends before reading PAGE_SIZE
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
#ifdef ENABLE_BPM_DEBUG
      LOG(INFO) << "Read less than a page" << std::endl;
#endif
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // set write cursor to offset
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);
  // check for I/O error
  if (db_io_.bad()) {
    LOG(ERROR) << "I/O error while writing";
    return;
  }
  // needs to flush to keep disk file in sync
  db_io_.flush();
}