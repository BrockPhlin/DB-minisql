#include "page/bitmap_page.h"

#include "glog/logging.h"

/*目的是找到一个空闲位，标记它为已分配，从0变成1*/
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  size_t max_size = GetMaxSupportedSize();

  for(uint32_t i = 0; i < max_size; i++){ //用max_size相当于最多把整个页表遍历一遍
    uint32_t try_offset = (next_free_page_ + i) % max_size; //在头文件的next_free_page_的基础上进行
    if (IsPageFree(try_offset)){
      uint32_t byte_index = try_offset / 8;
      uint8_t bit_index = try_offset % 8;
      bytes[byte_index] |= (1<<bit_index); //相应位赋值为1
      page_offset = try_offset;
      page_allocated_ ++;
      next_free_page_ = (try_offset + 1) % max_size;
      return true;
    }
  }

  return false; //找到最大了还是找不到
}

/*与Allocate对应，从已经分配标记成未分配，从1变成0*/
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  //超出容量，不可能顺利执行
  if (page_offset >= GetMaxSupportedSize()){
    return false;
  }
  //已经是0的话就不会成功释放
  if (IsPageFree(page_offset)) {
    return false;
  }

  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  bytes[byte_index] &= ~(1 << bit_index);//相应位置赋值为0，各位取反再取交集，相应为回变成0
  page_allocated_ --;
  return true;
}

/*依赖于IsPageFreeLow*/
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  uint32_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  return IsPageFreeLow(byte_index, bit_index);
}

/*最底层操作，检查某一位是否为0*/
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  unsigned char byte = bytes[byte_index]; //先提取某一个字节
  return (byte & (1 << bit_index)) == 0; //检查相应的位是不是0
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;