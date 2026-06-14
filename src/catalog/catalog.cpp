#include "catalog/catalog.h"

#include "page/index_roots_page.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

/**
 * TODO: Student Implement
 */
uint32_t CatalogMeta::GetSerializedSize() const {
  // CatalogMeta 的布局是：
  // 4 字节魔数 + 4 字节表数量 + 4 字节索引数量 +
  // 每个表 8 字节(table_id + page_id) + 每个索引 8 字节(index_id + page_id)。
  return 4 + 4 + 4 + 8 * (table_meta_pages_.size() + index_meta_pages_.size());
}

CatalogMeta::CatalogMeta() {}

/**
 * TODO: Student Implement
 */
CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  // init 为 true 表示创建一个全新的数据库文件，此时 catalog 里还没有任何表或索引。
  if (init) {
    // 新数据库需要一份空的 CatalogMeta，用来记录“表/索引元信息存在哪个页”。
    catalog_meta_ = CatalogMeta::NewInstance();
    // 空 CatalogMeta 的下一个 table_id 从 0 开始；这里用 store 写入 atomic 变量。
    next_table_id_.store(catalog_meta_->GetNextTableId());
    // 空 CatalogMeta 的下一个 index_id 也从 0 开始。
    next_index_id_.store(catalog_meta_->GetNextIndexId());

    // INDEX_ROOTS_PAGE_ID 是 B+Tree 用来保存 index_id -> root_page_id 的固定页。
    auto page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
    // 如果能取到这个固定页，就把它初始化为空的索引根页表。
    if (page != nullptr) {
      // Page::GetData() 才是真正的页内容区域，需要 reinterpret 成 IndexRootsPage 使用。
      auto roots_page = reinterpret_cast<IndexRootsPage *>(page->GetData());
      // 新数据库没有任何索引，所以根页映射数量从 0 开始。
      roots_page->Init();
      // 初始化修改了页内容，所以 unpin 时 dirty 传 true，让 BufferPool 知道要写回。
      buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
    }
    // 把空的 CatalogMeta 写到 CATALOG_META_PAGE_ID，保证数据库关闭后还能识别 catalog 页。
    FlushCatalogMetaPage();
    // 新数据库初始化完成后直接返回，不需要走下面“从磁盘加载”的分支。
    return;
  }

  // init 为 false 表示打开已有数据库，需要先读固定的 catalog meta 页。
  auto page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  // 已有数据库必须存在 catalog meta 页；否则文件不是一个合法的 MiniSQL 数据库。
  ASSERT(page != nullptr, "Failed to load catalog meta page.");
  // 从 catalog meta 页反序列化出 table_id/index_id 到元信息页号的映射。
  catalog_meta_ = CatalogMeta::DeserializeFrom(page->GetData());
  // 这里只读取 catalog meta 页，没有改内容，所以 dirty 传 false。
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);

  // catalog_meta_ 只保存“表元信息在哪个页”，这里逐个把 TableInfo 真正加载到内存。
  for (auto table_meta_page : catalog_meta_->table_meta_pages_) {
    // first 是 table_id，second 是存放 TableMetadata 的 page_id。
    LoadTable(table_meta_page.first, table_meta_page.second);
  }
  // 表必须先加载完，因为索引加载时需要找到它所属的 TableInfo 和表 Schema。
  for (auto index_meta_page : catalog_meta_->index_meta_pages_) {
    // first 是 index_id，second 是存放 IndexMetadata 的 page_id。
    LoadIndex(index_meta_page.first, index_meta_page.second);
  }

  // 重新打开数据库后，下一个 table_id 应该接在已有最大 table_id 后面。
  next_table_id_.store(catalog_meta_->GetNextTableId());
  // 重新打开数据库后，下一个 index_id 也应该接在已有最大 index_id 后面。
  next_index_id_.store(catalog_meta_->GetNextIndexId());
}

CatalogManager::~CatalogManager() {
  // 析构前先刷 catalog meta，避免内存里的最新映射没有落盘。
  FlushCatalogMetaPage();
  // catalog_meta_ 是构造函数里 new 出来的，CatalogManager 负责释放。
  delete catalog_meta_;
  // IndexInfo 的 key_schema_ 是表 Schema 的浅拷贝，所以要先删索引，再删表。
  for (auto iter : indexes_) {
    // iter.second 是 IndexInfo*，析构时会释放 IndexMetadata、Index 和浅拷贝 Schema。
    delete iter.second;
  }
  // 索引释放完后，再释放 TableInfo；TableInfo 会释放 TableMetadata 和 TableHeap。
  for (auto iter : tables_) {
    // TableMetadata 持有真正的表 Schema，因此最后释放表信息。
    delete iter.second;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
  // table_names_ 是“表名 -> table_id”的内存索引；如果能找到，说明表已经存在。
  if (table_names_.find(table_name) != table_names_.end()) {
    // 按接口约定，重复建表要返回 DB_TABLE_ALREADY_EXIST。
    return DB_TABLE_ALREADY_EXIST;
  }

  // 每张表的 TableMetadata 单独占用一个数据页，这里先准备接收新页号。
  page_id_t meta_page_id = INVALID_PAGE_ID;
  // NewPage 会从 BufferPool/DiskManager 申请一个新的逻辑页，并把页 pin 在内存中。
  auto page = buffer_pool_manager_->NewPage(meta_page_id);
  // 如果 BufferPool 找不到可用 frame，或者磁盘页分配失败，NewPage 会返回 nullptr。
  if (page == nullptr) {
    return DB_FAILED;
  }

  // next_table_id_ 是 atomic，后置 ++ 会取当前 id，并把下一个 id 自动加 1。
  auto table_id = next_table_id_++;
  // 外部传入的 schema 可能由调用者管理；表元信息必须拥有自己的深拷贝，防止二次释放。
  auto table_schema = Schema::DeepCopySchema(schema);
  // TableHeap::Create 会为真实表数据分配第一页 TablePage，并把第一页页号记录在 TableHeap 中。
  auto table_heap = TableHeap::Create(buffer_pool_manager_, table_schema, txn, log_manager_, lock_manager_);
  // TableMetadata 记录表 id、表名、表数据第一页页号和表结构 Schema。
  auto table_meta = TableMetadata::Create(table_id, table_name, table_heap->GetFirstPageId(), table_schema);
  // 把 TableMetadata 序列化到刚才分配的 meta page 中，这样重启后可以恢复表定义。
  table_meta->SerializeTo(page->GetData());
  // meta page 已经被写入新内容，所以 dirty 传 true；同时释放 NewPage 带来的 pin。
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  // TableInfo 是 catalog 暴露给 executor/planner 使用的内存对象。
  table_info = TableInfo::Create();
  // TableInfo 同时持有表元信息和表数据操作对象 TableHeap。
  table_info->Init(table_meta, table_heap);
  // CatalogMeta 只关心“这个 table_id 的元信息保存在哪个页”。
  catalog_meta_->table_meta_pages_.emplace(table_id, meta_page_id);
  // 维护表名到 table_id 的快速查询映射。
  table_names_.emplace(table_name, table_id);
  // 维护 table_id 到 TableInfo* 的快速查询映射。
  tables_.emplace(table_id, table_info);
  // 表元信息映射发生变化，需要把 CatalogMeta 刷回固定 catalog 页。
  FlushCatalogMetaPage();
  // 所有内存对象和持久化元信息都创建成功。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  // 先通过表名查 table_id；这里不直接遍历 tables_，因为表名映射更快也更明确。
  auto name_iter = table_names_.find(table_name);
  // 找不到表名时，把输出指针置空，避免调用方误用旧指针。
  if (name_iter == table_names_.end()) {
    table_info = nullptr;
    return DB_TABLE_NOT_EXIST;
  }
  // name_iter->second 是 table_id，再用 table_id 从 tables_ 取出真正的 TableInfo*。
  table_info = tables_[name_iter->second];
  // 找到表后返回成功，调用方可以通过 table_info 操作表数据或表 Schema。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  // 输出参数由调用方传入，先清空，避免把本次结果追加到旧结果后面。
  tables.clear();
  // tables_ 保存了所有 table_id -> TableInfo*，遍历它即可收集当前所有表。
  for (auto table : tables_) {
    // table.second 才是 TableInfo*；table.first 是 table_id。
    tables.emplace_back(table.second);
  }
  // 这里沿用原测试/执行器语义：没有任何表时返回 DB_FAILED，非空时返回 DB_SUCCESS。
  return tables.empty() ? DB_FAILED : DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
  // 当前项目只实现了 B+Tree 索引；其他 index_type 没有对应的 Index 实现。
  if (index_type != "bptree") {
    return DB_FAILED;
  }

  // 建索引必须先找到所属表，因为索引列来自表 Schema，索引数据也来自表数据。
  TableInfo *table_info = nullptr;
  // GetTable 会同时检查表是否存在，并把 TableInfo* 写到 table_info。
  auto table_result = GetTable(table_name, table_info);
  // 表不存在时直接把错误码往上返回，典型值是 DB_TABLE_NOT_EXIST。
  if (table_result != DB_SUCCESS) {
    return table_result;
  }

  // index_names_ 是“表名 -> (索引名 -> index_id)”的二级映射。
  auto index_name_iter = index_names_.find(table_name);
  // 同一张表上不能创建同名索引；不同表上的同名索引允许共存。
  if (index_name_iter != index_names_.end() && index_name_iter->second.find(index_name) != index_name_iter->second.end()) {
    return DB_INDEX_ALREADY_EXIST;
  }

  // key_map 记录“索引第 i 列对应表中的第几列”，例如 index(name,id) 可能是 {1,0}。
  std::vector<uint32_t> key_map;
  // 提前 reserve，避免 push/emplace 时多次扩容；容量正好等于索引列数。
  key_map.reserve(index_keys.size());
  // index_keys 里存的是用户给出的列名，需要逐个转成表 Schema 中的列下标。
  for (auto &key : index_keys) {
    // key_index 用来接收当前列名在表 Schema 中的下标。
    uint32_t key_index;
    // GetColumnIndex 会在线性扫描 Schema 的列名，找到后写入 key_index。
    auto key_result = table_info->GetSchema()->GetColumnIndex(key, key_index);
    // 如果用户指定的列名不存在，不能创建索引，直接返回列不存在错误。
    if (key_result != DB_SUCCESS) {
      return key_result;
    }
    // 保存这个列下标；后续 ShallowCopySchema 和 Row::GetKeyFromRow 都依赖它。
    key_map.emplace_back(key_index);
  }

  // 每个 IndexMetadata 也单独占用一个数据页，这里准备接收新页号。
  page_id_t meta_page_id = INVALID_PAGE_ID;
  // 为索引元信息申请一个新页；这个页不是 B+Tree 数据页，只保存 IndexMetadata。
  auto page = buffer_pool_manager_->NewPage(meta_page_id);
  // 分配失败时还没有创建任何索引对象，直接返回 DB_FAILED。
  if (page == nullptr) {
    return DB_FAILED;
  }

  // 取当前可用 index_id，并让 next_index_id_ 指向下一个 id。
  auto index_id = next_index_id_++;
  // IndexMetadata 保存 index_id、索引名、所属 table_id 和 key_map。
  auto index_meta = IndexMetadata::Create(index_id, index_name, table_info->GetTableId(), key_map);
  // 把 IndexMetadata 写入刚分配的元信息页，供数据库重启时恢复索引定义。
  index_meta->SerializeTo(page->GetData());
  // 元信息页已经写入，需要 dirty=true；释放 NewPage 默认持有的 pin。
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  // IndexInfo 是内存中的索引描述对象，它会持有 IndexMetadata、key_schema_ 和 Index。
  index_info = IndexInfo::Create();
  // Init 会用 key_map 从表 Schema 浅拷贝出 key_schema_，再创建 BPlusTreeIndex。
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
  // 如果索引类型或 key 大小不被支持，CreateIndex 可能返回 nullptr。
  if (index_info->GetIndex() == nullptr) {
    // IndexInfo 析构会释放 index_meta 和 key_schema_，避免内存泄漏。
    delete index_info;
    // 失败时把输出指针置空，避免调用方拿到无效对象。
    index_info = nullptr;
    // 已经申请的索引元信息页不再需要，删除它避免磁盘页泄漏。
    buffer_pool_manager_->DeletePage(meta_page_id);
    return DB_FAILED;
  }

  // 如果表里已经有数据，新建索引必须把已有行逐条插入 B+Tree，否则索引会漏数据。
  for (auto iter = table_info->GetTableHeap()->Begin(txn); iter != table_info->GetTableHeap()->End(); ++iter) {
    // key_row 只包含索引列，不包含整行的所有列。
    Row key_row;
    // 根据表 Schema 和索引 key_schema_，从当前整行中抽取索引键。
    iter->GetKeyFromRow(table_info->GetSchema(), index_info->GetIndexKeySchema(), key_row);
    // 把索引键和原始行的 RowId 插入 B+Tree，这样之后可以通过 key 找回行位置。
    if (index_info->GetIndex()->InsertEntry(key_row, iter->GetRowId(), txn) != DB_SUCCESS) {
      // 如果中途插入失败，先销毁已经创建出来的 B+Tree 页面。
      index_info->GetIndex()->Destroy();
      // 再释放 IndexInfo 及其持有的元数据对象。
      delete index_info;
      // 输出指针清空，表示创建失败。
      index_info = nullptr;
      // 元信息页也要删除，因为这个索引最终没有成功创建。
      buffer_pool_manager_->DeletePage(meta_page_id);
      return DB_FAILED;
    }
  }

  // 到这里说明索引对象和已有数据都创建成功，开始维护 catalog 的持久化映射。
  catalog_meta_->index_meta_pages_.emplace(index_id, meta_page_id);
  // 维护“表名 -> 索引名 -> index_id”的查询映射，供 GetIndex/GetTableIndexes 使用。
  index_names_[table_name].emplace(index_name, index_id);
  // 维护“index_id -> IndexInfo*”的查询映射，真正保存索引内存对象。
  indexes_.emplace(index_id, index_info);
  // 索引元信息页映射发生变化，需要把 CatalogMeta 刷回磁盘。
  FlushCatalogMetaPage();
  // 全部步骤成功完成。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  // 查索引前先确认表存在；索引一定挂在某张表下面。
  if (table_names_.find(table_name) == table_names_.end()) {
    // 表不存在时，输出指针置空，避免返回调用方传入的旧值。
    index_info = nullptr;
    return DB_TABLE_NOT_EXIST;
  }
  // 找到这张表对应的“索引名 -> index_id”映射。
  auto table_index_iter = index_names_.find(table_name);
  // 如果这张表没有任何索引，自然找不到目标索引。
  if (table_index_iter == index_names_.end()) {
    index_info = nullptr;
    return DB_INDEX_NOT_FOUND;
  }
  // 在该表的索引名映射中查找具体索引名。
  auto index_iter = table_index_iter->second.find(index_name);
  // 表上有索引，但没有这个名字的索引。
  if (index_iter == table_index_iter->second.end()) {
    index_info = nullptr;
    return DB_INDEX_NOT_FOUND;
  }
  // index_iter->second 是 index_id，再用 indexes_ 找到真正的 IndexInfo*。
  index_info = indexes_.at(index_iter->second);
  // 找到索引后返回成功。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  // 输出参数先清空，保证本次返回的全是当前表上的索引。
  indexes.clear();
  // 表不存在时，索引列表也没有意义，返回表不存在错误。
  if (table_names_.find(table_name) == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  // 查找这张表的索引名映射。
  auto table_index_iter = index_names_.find(table_name);
  // 表存在但没有索引不是错误，返回空 vector + DB_SUCCESS。
  if (table_index_iter == index_names_.end()) {
    return DB_SUCCESS;
  }
  // 遍历这张表的所有索引名/id 记录。
  for (auto index : table_index_iter->second) {
    // index.second 是 index_id，用它从 indexes_ 拿到 IndexInfo* 后放入输出列表。
    indexes.emplace_back(indexes_.at(index.second));
  }
  // 成功返回；调用方可以检查输出 vector 是否为空。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(table_id_t table_id) {
  // 通过 table_id 找 TableInfo；这个私有重载假设调用方已经处理了表名相关逻辑。
  auto table_iter = tables_.find(table_id);
  // 如果内存表映射里没有该 id，说明表不存在或 catalog 状态不一致。
  if (table_iter == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  // 保存 TableInfo*，后面需要释放它。
  auto table_info = table_iter->second;
  // 保存表名，删除 table_names_ 映射时要用。
  auto table_name = table_info->GetTableName();
  // 删除真实表数据占用的所有 TablePage；这里只删除表数据页，不删 TableMetadata 页。
  table_info->GetTableHeap()->FreeTableHeap();

  // 找到该表对应的 TableMetadata 页号。
  auto meta_page_iter = catalog_meta_->table_meta_pages_.find(table_id);
  // 正常情况下一定能找到；这里仍然判断，避免异常状态下访问 end 迭代器。
  if (meta_page_iter != catalog_meta_->table_meta_pages_.end()) {
    // meta_page_id 是保存 TableMetadata 的逻辑页号。
    auto meta_page_id = meta_page_iter->second;
    // 当前 BufferPoolManager 的 DeletePage 只会释放在 page_table_ 中的页；
    // 因此先 Fetch 一下，让该页进入 buffer，再 unpin 到 pin_count=0。
    auto page = buffer_pool_manager_->FetchPage(meta_page_id);
    // Fetch 成功才需要 unpin；这里没有改页内容，所以 dirty=false。
    if (page != nullptr) {
      buffer_pool_manager_->UnpinPage(meta_page_id, false);
    }
    // 删除 TableMetadata 所在页，释放磁盘空间和 buffer frame。
    buffer_pool_manager_->DeletePage(meta_page_id);
    // CatalogMeta 中也要删掉 table_id -> meta_page_id 的持久化映射。
    catalog_meta_->table_meta_pages_.erase(meta_page_iter);
  }

  // 删除“表名 -> table_id”的内存映射。
  table_names_.erase(table_name);
  // 删除“table_id -> TableInfo*”的内存映射；此时 table_info 指针仍保存在局部变量里。
  tables_.erase(table_iter);
  // 释放 TableInfo；它会级联释放 TableMetadata、TableHeap 和表 Schema。
  delete table_info;
  // CatalogMeta 发生变化，需要刷回固定 catalog 页。
  FlushCatalogMetaPage();
  // 表删除完成。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(const string &table_name) {
  // 公共接口按表名删除，所以先查“表名 -> table_id”。
  auto table_iter = table_names_.find(table_name);
  // 表名不存在时直接返回 DB_TABLE_NOT_EXIST。
  if (table_iter == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  // 删除表前必须先删除表上的所有索引，否则索引对象会引用即将释放的表 Schema。
  auto index_iter = index_names_.find(table_name);
  // 如果这张表建过索引，就逐个删除。
  if (index_iter != index_names_.end()) {
    // 不能一边遍历 unordered_map 一边 DropIndex，因为 DropIndex 会修改这个 map。
    std::vector<std::string> index_names;
    // 预留空间等于当前索引数量，避免 vector 扩容。
    index_names.reserve(index_iter->second.size());
    // 先把所有索引名拷贝出来。
    for (auto index : index_iter->second) {
      index_names.emplace_back(index.first);
    }
    // 再按拷贝出来的索引名逐个删除索引。
    for (auto &index_name : index_names) {
      DropIndex(table_name, index_name);
    }
  }

  // 表上的索引已经删除完，再调用按 table_id 删除的私有重载删除表本体。
  return DropTable(table_iter->second);
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  // 索引必须属于某张表，所以先确认表是否存在。
  if (table_names_.find(table_name) == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  // 找到这张表对应的索引名映射。
  auto table_index_iter = index_names_.find(table_name);
  // 表存在但没有任何索引时，目标索引一定不存在。
  if (table_index_iter == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  // 在该表的索引集合中查找指定索引名。
  auto index_iter = table_index_iter->second.find(index_name);
  // 没找到该索引名，返回 DB_INDEX_NOT_FOUND。
  if (index_iter == table_index_iter->second.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  // 保存 index_id，后面要用它删除 indexes_ 和 CatalogMeta 里的记录。
  auto index_id = index_iter->second;
  // indexes_ 是“index_id -> IndexInfo*”的内存对象表。
  auto info_iter = indexes_.find(index_id);
  // 正常情况下一定能找到；这里判断是为了在异常状态下更稳。
  if (info_iter != indexes_.end()) {
    // 先销毁 B+Tree 里的所有索引数据页和 IndexRootsPage 中的 root 记录。
    info_iter->second->GetIndex()->Destroy();
    // 再释放 IndexInfo；它会释放 IndexMetadata、Index 和 key_schema_。
    delete info_iter->second;
    // 从内存映射中删除 index_id -> IndexInfo*。
    indexes_.erase(info_iter);
  }

  // 找到保存该索引 IndexMetadata 的页号。
  auto meta_page_iter = catalog_meta_->index_meta_pages_.find(index_id);
  // 正常情况下一定能找到；仍然判断，避免状态不一致时崩溃。
  if (meta_page_iter != catalog_meta_->index_meta_pages_.end()) {
    // meta_page_id 是保存 IndexMetadata 的逻辑页号。
    auto meta_page_id = meta_page_iter->second;
    // 和 DropTable 一样，先 Fetch 再 Unpin，让当前 DeletePage 实现能够释放这个页。
    auto page = buffer_pool_manager_->FetchPage(meta_page_id);
    // 只是为了让页进入 buffer，没有修改页内容。
    if (page != nullptr) {
      buffer_pool_manager_->UnpinPage(meta_page_id, false);
    }
    // 删除 IndexMetadata 所在页。
    buffer_pool_manager_->DeletePage(meta_page_id);
    // 删除 CatalogMeta 中的 index_id -> meta_page_id 映射。
    catalog_meta_->index_meta_pages_.erase(meta_page_iter);
  }

  // 从该表的“索引名 -> index_id”映射中删除这个索引名。
  table_index_iter->second.erase(index_iter);
  // 如果这张表已经没有任何索引，就把外层 table_name 记录也删掉。
  if (table_index_iter->second.empty()) {
    index_names_.erase(table_index_iter);
  }
  // CatalogMeta 发生变化，需要持久化到 catalog meta 页。
  FlushCatalogMetaPage();
  // 索引删除完成。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::FlushCatalogMetaPage() const {
  // CatalogMeta 固定序列化到 CATALOG_META_PAGE_ID 这一页。
  auto page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  // 如果固定页取不到，说明 buffer/disk 状态异常，刷盘失败。
  if (page == nullptr) {
    return DB_FAILED;
  }
  // 把当前内存里的 table_meta_pages_ 和 index_meta_pages_ 写入页内容。
  catalog_meta_->SerializeTo(page->GetData());
  // 序列化修改了页内容，所以 dirty=true；释放 FetchPage 带来的 pin。
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  // FlushPage 立即把该页写回磁盘；成功则返回 DB_SUCCESS。
  return buffer_pool_manager_->FlushPage(CATALOG_META_PAGE_ID) ? DB_SUCCESS : DB_FAILED;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  // page_id 是 CatalogMeta 中记录的 TableMetadata 所在页。
  auto page = buffer_pool_manager_->FetchPage(page_id);
  // 取不到元信息页，就无法恢复这张表。
  if (page == nullptr) {
    return DB_FAILED;
  }

  // table_meta 用输出参数接收反序列化出来的新 TableMetadata 对象。
  TableMetadata *table_meta = nullptr;
  // 从页内容中恢复表名、表 id、表数据第一页页号和表 Schema。
  TableMetadata::DeserializeFrom(page->GetData(), table_meta);
  // 加载过程只读 TableMetadata 页，没有修改页内容。
  buffer_pool_manager_->UnpinPage(page_id, false);

  // CatalogMeta 里的 table_id 应该和 TableMetadata 自己记录的 table_id 一致。
  ASSERT(table_meta->GetTableId() == table_id, "Unexpected table id in catalog metadata.");
  // 根据 TableMetadata 里的 first_page_id 创建 TableHeap；这里不会分配新表页，只是恢复操作对象。
  auto table_heap = TableHeap::Create(buffer_pool_manager_, table_meta->GetFirstPageId(), table_meta->GetSchema(),
                                      log_manager_, lock_manager_);
  // 创建内存 TableInfo，用来把元信息和 TableHeap 绑在一起。
  auto table_info = TableInfo::Create();
  // TableInfo 接管 table_meta 和 table_heap 的生命周期。
  table_info->Init(table_meta, table_heap);
  // 恢复“表名 -> table_id”的内存映射，供按名字查表使用。
  table_names_.emplace(table_info->GetTableName(), table_id);
  // 恢复“table_id -> TableInfo*”的内存映射，供按 id 查表和执行器使用。
  tables_.emplace(table_id, table_info);
  // 这张表加载完成。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  // page_id 是 CatalogMeta 中记录的 IndexMetadata 所在页。
  auto page = buffer_pool_manager_->FetchPage(page_id);
  // 取不到索引元信息页，就无法恢复该索引。
  if (page == nullptr) {
    return DB_FAILED;
  }

  // index_meta 用输出参数接收反序列化出来的新 IndexMetadata 对象。
  IndexMetadata *index_meta = nullptr;
  // 从页内容中恢复索引名、index_id、所属 table_id 和 key_map。
  IndexMetadata::DeserializeFrom(page->GetData(), index_meta);
  // 加载过程只读 IndexMetadata 页，没有修改页内容。
  buffer_pool_manager_->UnpinPage(page_id, false);

  // CatalogMeta 里的 index_id 应该和 IndexMetadata 自己记录的 index_id 一致。
  ASSERT(index_meta->GetIndexId() == index_id, "Unexpected index id in catalog metadata.");
  // 索引依赖所属表的 Schema，因此必须先通过 table_id 找到 TableInfo。
  TableInfo *table_info = nullptr;
  // index_meta->GetTableId() 是创建索引时保存的所属表 id。
  auto table_result = GetTable(index_meta->GetTableId(), table_info);
  // 如果表没加载成功，索引也无法加载；此时要释放刚反序列化的 index_meta。
  if (table_result != DB_SUCCESS) {
    delete index_meta;
    return table_result;
  }

  // 创建内存 IndexInfo，用来持有 IndexMetadata、key_schema_ 和真正的 Index 对象。
  auto index_info = IndexInfo::Create();
  // Init 会根据表 Schema 和 key_map 恢复 key_schema_，并打开/创建 B+TreeIndex 对象。
  index_info->Init(index_meta, table_info, buffer_pool_manager_);
  // 如果 B+TreeIndex 创建失败，释放 IndexInfo 并报告失败。
  if (index_info->GetIndex() == nullptr) {
    delete index_info;
    return DB_FAILED;
  }

  // 恢复“表名 -> 索引名 -> index_id”的内存映射。
  index_names_[table_info->GetTableName()].emplace(index_info->GetIndexName(), index_id);
  // 恢复“index_id -> IndexInfo*”的内存映射。
  indexes_.emplace(index_id, index_info);
  // 这个索引加载完成。
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  // 按 table_id 查找 TableInfo，主要供 LoadIndex 这种已知 table_id 的内部逻辑使用。
  auto table_iter = tables_.find(table_id);
  // 找不到 id 时，把输出指针置空，并返回表不存在。
  if (table_iter == tables_.end()) {
    table_info = nullptr;
    return DB_TABLE_NOT_EXIST;
  }
  // 找到后把 TableInfo* 写入输出参数。
  table_info = table_iter->second;
  // 查询成功。
  return DB_SUCCESS;
}
