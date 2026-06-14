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
  return 4        // magic
       + 4        // name_len
       + name_.size()
       + 4        // type
       + 4        // len
       + 4        // table_ind
       + 1        // nullable
       + 1;       // unique
}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  // p 是当前读取位置；保留 buf 原值，最后用 p - buf 计算一共读了多少字节。
  char *p = buf;
  // 先读 4 字节魔数，用来确认这段数据确实是 Column 序列化出来的。
  uint32_t magic; memcpy(&magic, p, 4);             p += 4;
  // 魔数不匹配说明读错页、读错偏移，或者文件内容已经损坏。
  ASSERT(magic == COLUMN_MAGIC_NUM, "Column magic mismatch");
  // 读取列名长度。列名是变长字符串，所以必须先知道长度。
  uint32_t name_len; memcpy(&name_len, p, 4);       p += 4;
  // 根据 name_len 从 buffer 中构造 std::string；构造后 p 跳过列名内容。
  std::string name(p, name_len);                    p += name_len;
  // 读取列类型，例如 int、float、char。
  TypeId type; memcpy(&type, p, 4);                 p += 4;
  // 读取序列化时保存的 len_。CHAR 的 len_ 是最大字符长度，固定类型的 len_ 可由类型推导。
  uint32_t len; memcpy(&len, p, 4);                 p += 4;
  // 读取该列在表 Schema 中的下标。
  uint32_t table_ind; memcpy(&table_ind, p, 4);     p += 4;
  // nullable 和 unique 都是 bool，各占 1 字节。
  bool nullable, unique;
  // 读取是否允许为空。
  memcpy(&nullable, p, 1);                          p += 1;
  // 读取是否唯一。
  memcpy(&unique, p, 1);                            p += 1;

  // Catalog 加载表 Schema 时会走到这里。CHAR 类型必须使用带 length 的构造函数，
  // 因为它的长度来自建表语句；INT/FLOAT 等固定长度类型必须使用不带 length 的构造函数，
  // 让构造函数按类型设置 len_，否则会触发“非 CHAR 类型不能用 CHAR 构造函数”的断言。
  if (type == TypeId::kTypeChar) {
    // CHAR 列需要保留序列化出来的最大长度 len。
    column = new Column(name, type, len, table_ind, nullable, unique);
  } else {
    // 固定长度列的长度由构造函数根据 type 自动设置，例如 int 是 sizeof(int32_t)。
    column = new Column(name, type, table_ind, nullable, unique);
  }
  // 返回本次反序列化消耗的字节数，方便上层继续读取下一个对象。
  return p - buf;
}
