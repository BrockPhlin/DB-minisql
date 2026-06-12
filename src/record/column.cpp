#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  char *p = buf;
  uint32_t magic = COLUMN_MAGIC_NUM;       // 头文件里已经定义
  uint32_t name_len = name_.size();
  memcpy(p, &magic, 4);                    p += 4;
  memcpy(p, &name_len, 4);                 p += 4;
  memcpy(p, name_.data(), name_len);       p += name_len;
  memcpy(p, &type_, sizeof(TypeId));       p += 4;   // TypeId 是 enum，本质 int
  memcpy(p, &len_, 4);                     p += 4;
  memcpy(p, &table_ind_, 4);               p += 4;
  memcpy(p, &nullable_, 1);                p += 1;
  memcpy(p, &unique_, 1);                  p += 1;
  return p - buf;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  char *p = buf;
  uint32_t magic = COLUMN_MAGIC_NUM;       // 头文件里已经定义
  uint32_t name_len = name_.size();
  memcpy(p, &magic, 4);                    p += 4;
  memcpy(p, &name_len, 4);                 p += 4;
  memcpy(p, name_.data(), name_len);       p += name_len;
  memcpy(p, &type_, sizeof(TypeId));       p += 4;   // TypeId 是 enum，本质 int
  memcpy(p, &len_, 4);                     p += 4;
  memcpy(p, &table_ind_, 4);               p += 4;
  memcpy(p, &nullable_, 1);                p += 1;
  memcpy(p, &unique_, 1);                  p += 1;
  return p - buf;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  char *p = buf;
  uint32_t magic; memcpy(&magic, p, 4);             p += 4;
  ASSERT(magic == COLUMN_MAGIC_NUM, "Column magic mismatch");
  uint32_t name_len; memcpy(&name_len, p, 4);       p += 4;
  std::string name(p, name_len);                    p += name_len;
  TypeId type; memcpy(&type, p, 4);                 p += 4;
  uint32_t len; memcpy(&len, p, 4);                 p += 4;
  uint32_t table_ind; memcpy(&table_ind, p, 4);     p += 4;
  bool nullable, unique;
  memcpy(&nullable, p, 1);                          p += 1;
  memcpy(&unique, p, 1);                            p += 1;

  // 注意：name_ 在 Column 里是 std::string，要用 ctor
  column = new Column(name, type, len, table_ind, nullable, unique);
  return p - buf;
}
