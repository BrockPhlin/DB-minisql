#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  char *p = buf;
  uint32_t n = fields_.size();
  memcpy(p, &n, 4);
  p += 4;

  // 1) null bitmap
  uint32_t bitmap_bytes = (n + 7) / 8;
  memset(p, 0, bitmap_bytes);
  for (uint32_t i = 0; i < n; i++) {
    if (fields_[i]->IsNull()) {
      p[i / 8] |= (1 << (i % 8));
    }
  }
  p += bitmap_bytes;

  // 2) 每个 Field
  for (uint32_t i = 0; i < n; i++) {
    if (!fields_[i]->IsNull()) {
      p += fields_[i]->SerializeTo(p);
    }
  }
  return p - buf;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  char *p = buf;
  uint32_t n; memcpy(&n, p, 4);                          p += 4;

  uint32_t bitmap_bytes = (n + 7) / 8;
  // 注意：先把 bitmap 复制出来再前进 p；不要破坏原 buf
  std::vector<char> bitmap(bitmap_bytes);
  memcpy(bitmap.data(), p, bitmap_bytes);
  p += bitmap_bytes;

  for (uint32_t i = 0; i < n; i++) {
    bool is_null = (bitmap[i / 8] >> (i % 8)) & 1;
    Field *f = nullptr;
    p += Field::DeserializeFrom(p, schema->GetColumn(i)->GetType(), &f, is_null);
    fields_.push_back(f);   // Row 接管该 Field* 内存
  }
  return p - buf;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  uint32_t size = 4;                              // field_count
  size += (fields_.size() + 7) / 8;               // null bitmap，按 8 向上取整
  for (auto &f : fields_) {
    size += f->GetSerializedSize();
  }
  return size;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
