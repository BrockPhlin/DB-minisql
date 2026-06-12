#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  char *p = buf;
  uint32_t magic = SCHEMA_MAGIC_NUM;
  uint32_t cnt = columns_.size();
  memcpy(p, &magic, 4); p += 4;
  memcpy(p, &cnt, 4);   p += 4;
  for (auto col : columns_) {
    p += col->SerializeTo(p);
  }
  return p - buf;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = 4 + 4;  // MAGIC + column_count
  for (auto col : columns_) {
    size += col->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  char *p = buf;
  uint32_t magic; memcpy(&magic, p, 4); p += 4;
  ASSERT(magic == SCHEMA_MAGIC_NUM, "Schema magic mismatch");
  uint32_t cnt;    memcpy(&cnt, p, 4);   p += 4;

  std::vector<Column *> cols;
  cols.reserve(cnt);
  for (uint32_t i = 0; i < cnt; i++) {
    Column *c = nullptr;
    p += Column::DeserializeFrom(p, c);
    cols.push_back(c);
  }
  // is_manage_=true：schema 析构时会 delete 这些 Column
  schema = new Schema(cols, true);
  return p - buf;
}