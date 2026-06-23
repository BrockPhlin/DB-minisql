# DB-minisql 验收速记 / Q&A 备查

> 浙大 2026 春 DB-minisql（参考 CMU-15445 BusTub 改写）。所有要点均来自仓库源码逐行阅读（2026-06-17）。

---

## 0. 一句话项目流

**SQL 文本 → lex/yacc → AST → Planner (AST→Statement→Plan) → Volcano 迭代器模型 Executor → BufferPool → DiskManager → 磁盘文件。**

---

## 1. 入口与构建

- `src/main.cpp`：REPL 主循环
  - `InputCommand` 读一行直到 `;`
  - `yy_scan_string` + `yyparse`（lex/yacc）→ AST
  - `SyntaxTreePrinter` 把 AST 输出到 `syntax_tree_<id>.txt`（graphviz dot 格式）
  - `engine.Execute(root)` 分派
  - 每个查询结束 `MinisqlParserFinish()` + `yylex_destroy()` 释放语法树
- 构建：`mkdir build && cd build && cmake .. && make -j`
- `CMakeLists.txt`：C++17，glog + googletest 子模块，`zSql` 共享库 + `main` 可执行
- 编译选项 `-DENABLE_OUTPUT_DBG_INFO` 默认开启，DBMS_DEBUG 由 `ENABLE_BPM_DEBUG` 等宏控制

---

## 2. SQL → AST（Parser 层）

文件：`src/include/parser/minisql.l`（lex）、`minisql.y`（yacc）、`parser.c` / `syntax_tree.c`（运行时 C 代码）

- **AST 节点**：`SyntaxNode { id_, type_, line_no_, col_no_, child_, next_, val_ }`
  - `child_` = 第一个孩子；`next_` = 兄弟（单链表）
  - `val_` = deep-copy 的字符串；`kNodeString` 自动去掉两端引号
  - 节点类型枚举见 `SyntaxNodeType`（kNodeCreateTable, kNodeSelect, kNodeConditions, kNodeCompareOperator 等共 35+ 种）
- 解析结果通过 `MinisqlParserSetRoot` 存到 `minisql_parser_root_node_`，由 `MinisqlGetParserRootNode()` 读出
- 错误处理：`MinisqlParserSetError(msg)` 标记错误并记错误信息
- 销毁：每条语句结束调 `MinisqlParserFinish()` 遍历 `minisql_parser_syntax_node_list_` 释放

**常用 AST 结构**（老师最爱问的）：
- `kNodeSelect` → child: `kNodeColumnList` / `kNodeAllColumns` → next: `kNodeIdentifier`(table) → next: `kNodeConditions`
- `kNodeConditions` → child: 条件表达式（Connector 或 CompareOperator）
- `kNodeConnector` → child: 左条件 → next: 右条件
- `kNodeCompareOperator` → child: column → next: value
- `kNodeCreateTable` → child: 表名 → next: `kNodeColumnDefinitionList`
- `kNodeColumnDefinition` → child: 列名 → next: `kNodeColumnType`
- `kNodeColumnType` (char) → child: `kNodeNumber` (长度)

---

## 3. AST → Plan（Planner 层）

文件：`src/planner/planner.cpp` + `include/planner/statement/*` + `include/planner/expressions/*`

### 3.1 Statement 绑定
- `Planner::PlanQuery(ast)` 按 `ast->type_` switch 到 Select/Insert/Delete/Update
- 每种 statement 调自己的 `SyntaxTree2Statement` 把 AST 走一遍，构造 Statement 对象：
  - `SelectStatement`：含 `table_name_`、`column_list_`（`vector<pair<string, ColumnValueExpr>>`）、`where_`（抽象表达式树）、`column_in_condition_`（出现在 where 中的列下标）、`has_or`
  - `InsertStatement`：`raw_values_` (vector of expressions)
  - `UpdateStatement`：`update_attrs` (col_idx → const expr)
  - `DeleteStatement`：`where_`
- `AbstractStatement::MakePredicate(ast, table_name, &column_in_condition, &has_or)` 递归构造表达式树
  - `kNodeConnector` → `LogicExpression`（AND/OR），若 `val_=="or"` 则 `*has_or = true`
  - `kNodeCompareOperator` → `ComparisonExpression(colExpr, constExpr, op)`

### 3.2 表达式体系
- `AbstractExpression { ret_type_, type_, children_ }` 树
- 子类：
  - `ColumnValueExpression(row_idx, col_idx, ret_type)` → `Evaluate(row)` 取 `row->GetField(col_idx)`
  - `ConstantValueExpression(val)` → 永远返回 val
  - `ComparisonExpression(left, right, comp_type)` → 返回 `kTypeInt` 0/1
  - `LogicExpression(left, right, LogicType)` → 三值逻辑 True/False/Null
- 三值逻辑 (`CmpBool`): kFalse=0, kTrue, kNull

### 3.3 PlanNode
- `AbstractPlanNode { Schema *output_schema_, vector<AbstractPlanNodeRef> children_ }`
- `PlanType` 枚举：SeqScan, IndexScan, Insert, Update, Delete, Values, Aggregation, Limit, Distinct, NestedLoopJoin
- **火山模型**：每个算子实现 `Init()` + `Next(Row*, RowId*)` + `GetOutputSchema()`

### 3.4 Planner 关键决策
```cpp
// PlanSelect
if (available_index.empty() || statement->has_or) {
    return SeqScanPlanNode
} else {
    return IndexScanPlanNode(indexes, need_filter = (avail != cond_cols.size()))
}
```
- "has_or" 一定走 SeqScan（IndexScan 不擅长 OR 合并）
- "need_filter" = true 表示索引只覆盖了部分 where 列，仍需回表再过滤

### 3.5 几个 PlanNode 的字段
- `SeqScanPlanNode { table_name_, filter_predicate_ }`
- `IndexScanPlanNode { table_name_, indexes_, need_filter_, filter_predicate_ }`
- `InsertPlanNode { table_name_, child: ValuesPlanNode }`
- `UpdatePlanNode { table_name_, child: SeqScan, update_attrs: map<col_idx, expr> }`
- `DeletePlanNode { table_name_, child: SeqScan }`
- `ValuesPlanNode { vector<vector<AbstractExpressionRef>> }`

### 3.6 OutputSchema
- `Planner::MakeOutputSchema` 为 Select 构造输出 schema，CHAR 类型用 `MAX_VARCHAR_SIZE=128` 作 length
- 注意：execute_engine.cpp 第 211-212 行 `if (ast->type_ == kNodeSelect) delete planner.plan_->OutputSchema();` —— 手工释放以避免 shared_ptr 循环

---

## 4. 执行引擎 Executor

文件：`src/executor/*.cpp` + `execute_engine.cpp`

### 4.1 CreateExecutor 工厂
按 `plan->GetType()` switch：
- SeqScan / IndexScan / Values → 直接构造
- Update / Delete / Insert → 递归先建 child，再包一层

### 4.2 各算子关键点
- **SeqScanExecutor**：通过 `TableIterator` 顺序扫，`SchemaEqual` 判断是否需要 TupleTransfer
- **IndexScanExecutor**：递归遍历 LogicExpression 树：
  - LogicExpression：左右子表达式结果 `set_intersection`（按 RowId 升序）
  - ComparisonExpression：取右值作 key，调 `index->ScanKey(key, ret, txn, comp_type)`
  - ⚠️ `lhs.empty()` 短路返回 rhs 这段写法有 bug 风险（注释里也承认了）：无法区分"0 行匹配"与"该列未建索引"
- **InsertExecutor**：
  1. 先对每个索引的 key 做 `ScanKey`，如果已有 → 报 "key already exists"（UNIQUE 约束）
  2. `TableHeap::InsertTuple`
  3. 遍历索引插 entry
- **UpdateExecutor**：
  1. child 出 (src_row, src_rid)
  2. `GenerateUpdatedTuple` 按 update_attrs 重算
  3. `TableHeap::UpdateTuple`（可能走 delete+insert）
  4. 对每个索引：先 `RemoveEntry(src_key, src_rid)` 再 `InsertEntry(dest_key, src_rid)`
- **DeleteExecutor**：
  1. child 出 row
  2. `TableHeap::MarkDelete`（不是物理删除！）
  3. 遍历索引 `RemoveEntry`
  4. 真删要靠 `ApplyDelete`（commit 时调），本作业没实现事务，所以 MarkDelete 后元数据就丢了——但 `TablePage::GetNextTupleRid` 会过滤被 mark 的（`IsDeleted` 检查 size 的高 bit），所以下次 Scan 看不到 ✓
- **ValuesExecutor**：单纯按 cursor 推 values

### 4.3 ExecuteEngine 顶层
- `Execute(ast)`：
  - CreateDB / DropDB / ShowDB / UseDB / ShowTables / CreateTable / DropTable / ShowIndexes / CreateIndex / DropIndex / TrxBegin/Commit/Rollback / ExecFile / Quit → 各 ExecuteXxx
  - Select/Insert/Delete/Update → 进 Planner
- 输出表格用 `ResultWriter`，宽度自动算

### 4.4 ExecuteXxx 注意点
- **CreateTable**：解析 column defs 列表（注意遍历用 `child_->next_`），区分 int/float/char 调不同 Column 构造
- **CreateIndex**：遍历所有表找索引（drop index 没有 ON 子句）
- **TrxBegin/Commit/Rollback**：现在都直接 return `DB_FAILED`（没实现事务）
- **ExecFile**：读文件，按 `;` 切分，每段重新 `yy_scan_string` + `yyparse` + `Execute`

---

## 5. 存储层 Page / Buffer / Storage

### 5.1 Page（src/include/page/page.h）
- `data_[PAGE_SIZE]` = 4KB 裸 buffer
- 成员：page_id_, pin_count_, is_dirty_, rwlatch_（reader-writer latch）
- 头部 8 字节：PageId(4) + LSN(4)

### 5.2 Page 类型
- **TablePage**（`page/table_page.h`）：slotted page 格式
  - Header 24 字节：PageId(4) LSN(4) PrevPageId(4) NextPageId(4) FreeSpacePointer(4) TupleCount(4)
  - Tuple 目录：每个 slot 8 字节 = offset(4) + size(4)
  - 删除标记：size 的最高位 `DELETE_MASK = 1<<31`
  - `InsertTuple`：先找空 slot，否则从 free space 顶部向下增长
  - `MarkDelete`：置 DELETE_MASK
  - `UpdateTuple`：若空间够直接 memmove；不够则交给 TableHeap 走 delete+insert
  - `GetNextTupleRid`：跳过已删的
- **HeaderPage**：通用 name → root_id 映射
- **IndexRootsPage**：index_id → root_page_id 映射（layout: count(4) + entries）
- **BitmapPage<PAGE_SIZE>**：4096 - 8 = 4088 字节 → 32704 bit → 一个 extent 32704 页
  - AllocatePage: 找下一个 free bit 并标 1
  - DeAllocatePage: 把对应位置 0
- **DiskFileMetaPage**：存在 page 0，记录 num_extents_、num_allocated_pages_、extent_used_page_[]
- **BPlusTreePage / BPlusTreeInternalPage / BPlusTreeLeafPage**：见 §6

### 5.3 BufferPoolManager（src/buffer/buffer_pool_manager.cpp）
- 数据结构：`Page[] pages_`（默认 20480 frame）、`unordered_map<page_id, frame_id> page_table_`、`LRUReplacer *replacer_`、`list<frame_id> free_list_`、`recursive_mutex latch_`
- **FetchPage(pid)**：
  1. 查 page_table_ 命中 → pin_count++ + Pin
  2. 选 victim（先 free_list，后 LRU）
  3. 脏页先写回
  4. 旧 page_id 从 page_table_ 删除
  5. 装入新 page，从磁盘 ReadPage
- **NewPage(pid_out)**：类似但调 `AllocatePage()`（递增 id）
- **UnpinPage(pid, is_dirty)**：dirty 只能 false→true，pin_count 减到 0 时 LRU.Unpin
- **DeletePage(pid)**：pin_count 必须为 0 → 写回（若脏）→ DeallocatePage → frame 重置 → push 到 free_list

### 5.4 LRUReplacer
- `list<frame_id_t> lru_list_` + `unordered_map<frame_id, list::iterator> map_`
- Victim = pop_back，Pin = erase，Unpin = push_front

### 5.5 DiskManager
- 文件格式：`[MetaPage][Bitmap1][data1..dataN][Bitmap2][dataN+1..][...]`
- `MapPageId(logical_id)` = `1 + extent_id*(BITMAP_SIZE+1) + offset_in_extent + 1`
- `AllocatePage`：取 `num_allocated_pages_` 作新 id；如 extent 用尽就开新 extent（写一个空 bitmap 页）
- `DeAllocatePage`：把 extent bitmap 那一位置 0，`num_allocated_pages_--`

### 5.6 TableHeap
- `first_page_id_` + `schema_` + BPM
- 链式 table pages（通过 `NextPageId` 串起来）
- `InsertTuple` first-fit：沿链找有空 slot 的页
- `UpdateTuple`：调 `TablePage::UpdateTuple`；空间不够就 MarkDelete+InsertTuple
- `Begin()` / `End()` 走 `TableIterator`

### 5.7 TableIterator
- 构造时 fetch 当前 rid 的行
- `++` 先在本页找下一个，否则翻到下一页
- End() 用 `RowId()` 作哨兵

---

## 6. 索引 Index / B+ 树

### 6.1 GenericKey & KeyManager
- `GenericKey` 仅是 `char data[0]`（柔性数组），真实大小由 `key_size_` 决定
- `KeyManager` 持有 `Schema *key_schema_` 和 `int key_size_`
- `CompareKeys`：反序列化两个 GenericKey 成 Row，逐字段 CompareLessThan / CompareGreaterThan
- IndexInfo 计算 key_size：根据索引列类型与 len 做对齐（8/16/24/32/64/128/256 向上取整）

### 6.2 B+ 树页
- **BPlusTreePage**：28 字节 header：`PageType KeySize LSN CurrentSize MaxSize ParentPageId PageId`（共 28 字节）
- **LeafPage**：32 字节 header + pairs(key+RowId)，有 `next_page_id_` 串成有序链表
- **InternalPage**：28 字节 header + pairs(key+page_id)，第 0 个 key 是占位（无效）
- `GetMinSize()`：根叶子允许 1；根内部允许 2；其余 `(max+1)/2`

### 6.3 B+ 树操作
- **Insert**：空树 → `StartNewTree`；否则 `InsertIntoLeaf`，溢出 → `Split` → `InsertIntoParent`（可能递归到根）
- **Remove**：`FindLeafPage` → 删记录 → 若 < min_size → `CoalesceOrRedistribute` → 可能向上递归 → `AdjustRoot`
- **Coalesce**：兄弟元素不够借，合并（删本节点、调父节点）
- **Redistribute**：兄弟够借，借一个
- **AdjustRoot**：根为空 → 清空；根只剩 1 个孩子 → 该孩子升为新根
- **UpdateRootPageId(insert=1)**：写 IndexRootsPage（持久化 index_id→root_page_id）

### 6.4 BPlusTreeIndex
- 包装 BPlusTree，对外暴露 `InsertEntry / RemoveEntry / ScanKey / Destroy / GetBeginIterator / GetEndIterator`
- **ScanKey** 支持 `=, >, >=, <, <=, <>`：用 IndexIterator 范围扫
- `<>`：先收集所有，再 erase 匹配 key 的

---

## 7. 记录 Record 层

### 7.1 Type
- `Type` 单例：`TypeInt / TypeFloat / TypeChar`
- `GetTypeSize`：int=4, float=4, char=0（变长）
- 序列化：int/float 直接写裸字节；char 写 `len(4) + bytes`
- 比较：3 值（True/False/Null），任一为 null → kNull

### 7.2 Field
- 联合体 `Val { int32_t integer_, float float_, char *chars_ }`
- 析构：char 类型且 `manage_data_=true` 时 `delete[] value_.chars_`
- 拷贝构造：char 类型 + manage_data 时深拷贝

### 7.3 Schema / Column
- **Column** 序列化：magic(4) + name_len(4) + name + TypeId(4) + len(4) + table_ind(4) + nullable(1) + unique(1)
- `ShallowCopySchema(table_schema, attrs)`：挑出索引列，**不复制 Column**，所以 delete 不会重复
- `DeepCopySchema`：逐列 new Column
- Schema `magic = 200715`，Column `magic = 210928`

### 7.4 Row
- `fields_` 是 `vector<Field*>`，由 Row 析构统一 delete
- 序列化：`field_count(4) + null_bitmap + each field`（null 的 field 不占字节）
- `GetKeyFromRow`：按 key_schema 列名，从 row 中抽出对应列组成新 Row

---

## 8. Catalog 层

### 8.1 CatalogMeta（catalog.cpp）
- 持久化在 `CATALOG_META_PAGE_ID = 0`（也是 META_PAGE_ID）
- 布局：magic(4) + table_n(4) + index_n(4) + 8B/项
- `GetNextTableId/IndexId` = `rbegin()->first + 1`

### 8.2 CatalogManager
- 内存结构：`table_names_`（name→id）、`tables_`（id→TableInfo*）、`index_names_`（table_name→(name→id)）、`indexes_`（id→IndexInfo*）
- **CreateTable**：
  1. 检查重名
  2. NewPage 给 TableMetadata
  3. next_table_id_++
  4. Schema::DeepCopySchema
  5. TableHeap::Create
  6. TableMetadata::SerializeTo(page)
  7. UnpinPage(..., true)
  8. TableInfo::Init
  9. 维护映射，FlushCatalogMetaPage
- **CreateIndex**：
  1. 查表 + 重名
  2. 把 key 列名 → 表列下标 key_map
  3. NewPage 给 IndexMetadata
  4. IndexInfo::Init（自动用 ShallowCopySchema 构造 key_schema）
  5. **如果表已有数据，要把已有行 InsertEntry 进 B+ 树**（建索引不能漏数据）
- **DropTable**：先 DropTable 上所有索引（避免 dangling index schema），再 free table heap
- **DropIndex**：先 Destroy B+ 树，再 DeletePage IndexMetadata 页
- **FlushCatalogMetaPage**：FetchPage(0) + Serialize + Unpin + FlushPage
- **LoadTable / LoadIndex**：根据 CatalogMeta 里的 (id, meta_page_id) 逐个反序列化 + 建 TableHeap / BPlusTree

### 8.3 TableMetadata / IndexMetadata
- TableMetadata：`magic(4) + table_id(4) + name_len(4) + name + root_page_id(4) + schema`
- IndexMetadata：`magic(4) + index_id(4) + name_len(4) + name + table_id(4) + key_n(4) + key_n*4B`

### 8.4 IndexInfo
- 持有 `meta_data_` / `key_schema_` (ShallowCopy) / `index_` (BPlusTreeIndex*)
- `CreateIndex(bpm, "bptree")` 根据 key 列总大小选 16/32/64/128/256 字节

---

## 9. 持久化文件布局（一张图）

```
物理页号 0  ─→ DiskFileMetaPage（meta + num_allocated + extent_used[]）
物理页号 1  ─→ CatalogMeta（表/索引 meta 页号）
物理页号 2  ─→ IndexRootsPage（index_id → root_page_id）
物理页号 3  ─→ 业务页 or bitmap
...
extent 结构：[BitmapN] [DataN*BMP_SIZE]
   ↑MapPageId(logical) = 1 + extent_id*(BMP_SIZE+1) + offset + 1
```

---

## 10. 老师可能问的 30 个问题

### 基础
1. **架构**：SQL → Parser → AST → Planner → Volcano Executor → BufferPool → DiskManager
2. **构建系统**：CMake 3.16+，C++17，glog+googletest 第三方
3. **入口**：`main.cpp` 的 REPL 循环
4. **为什么参考 BusTub**：Volcano 模型 + 火山迭代器 + 共享内存管理思想

### Parser
5. **AST 节点结构**：`SyntaxNode { id, type, line, col, child, next, val }`，child/next 是链表
6. **lex / yacc 怎么配合**：lex 产 token，yacc 用 Bison 语法动作构造 SyntaxNode
7. **如何处理错误**：`MinisqlParserSetError` 设置标记 + 错误信息
8. **字符串去引号**：`syntax_tree.c::CreateSyntaxNode` kNodeString 特殊处理
9. **语法树如何打印**：`SyntaxTreePrinter` 输出 graphviz dot 格式到 syntax_tree_<id>.txt
10. **Bison 的 %union**：把语法值统一成 `pSyntaxNode`，所有 token 都用这一种

### Planner
11. **AST → Plan 怎么走**：`SyntaxTree2Statement` 递归 → `PlanSelect/Insert/...` 转 plan
12. **为什么 IndexScan 需要 SeqScan 兜底**：has_or 时 OR 合并困难，索引不能完全覆盖
13. **planner.plan_->OutputSchema() 何时 delete**：仅 Select 时手工 delete（避免 shared_ptr 循环）
14. **三值逻辑**：`LogicExpression::PerformComputation` 处理 True/False/Null
15. **抽象表达式树**：`AbstractExpression` 的 `Evaluate` 把 Row 算成 Field

### Executor
16. **Volcano 模型**：每个算子 Init + Next + GetOutputSchema
17. **InsertExecutor 如何保证 UNIQUE**：对每个 index 的 key 先 ScanKey 查重
18. **UpdateExecutor 怎么更新索引**：先 RemoveEntry(src_key, src_rid) 再 InsertEntry(dest_key, src_rid)
19. **DeleteExecutor 为什么不物理删除**：事务里要等 commit 才真删；本作业用 MarkDelete，scan 时通过 IsDeleted 过滤
20. **IndexScan 如何处理 AND/OR**：递归遍历 LogicExpression；AND → set_intersection；OR → 顺序合并

### Page / Buffer
21. **Slotted page 格式**：header + free space pointer + tuple array
22. **删除标记怎么存**：`tuple_size | (1<<31)`，高位表示已删
23. **free_list 和 LRU 怎么配合**：优先 free_list，没有再 Victim
24. **pin_count 和 dirty 的语义**：pin 阻止 victim；dirty 决定是否写回
25. **并发控制**：recursive_mutex 保护 page_table_、latch_、free_list_；每页独立 rwlatch_
26. **什么是 extent**：一组共享一个 bitmap 页的数据页，BITMAP_SIZE=32704

### B+ 树
27. **为什么用 B+ 树**：磁盘友好、范围查询 O(log n)
28. **B+ 树分裂阈值**：> max_size 才分裂；max_size 算 `(PAGE_SIZE - HEADER) / pair_size - 1`
29. **Coalesce vs Redistribute**：兄弟够借就 Redistribute，否则 Coalesce
30. **B+ 树根的特殊性**：根可 < min_size；根只剩 1 个孩子时该孩子升为新根
31. **GenericKey 大小如何决定**：IndexInfo::CreateIndex 按 8/16/24/32/64/128/256 向上对齐
32. **InternalPage 第 0 个 key 为什么无效**：占位用，方便 ValueAt(i) 与 KeyAt(i) 对齐

### Catalog
33. **CatalogMeta 怎么持久化**：固定写 page 0（CATALOG_META_PAGE_ID = 0 = META_PAGE_ID）
34. **IndexRootsPage 存什么**：index_id → root_page_id 的映射（不存表名）
35. **建索引时若表已有数据怎么办**：遍历 TableHeap，把每行 InsertEntry 进 B+ 树
36. **DropTable 为什么先 DropIndex**：避免 IndexInfo 持有失效的 TableSchema
37. **DeepCopySchema vs ShallowCopySchema**：建表用 Deep（独立管理），建索引用 Shallow（共享表的 Column）
38. **next_table_id_ 是 atomic**：支持并发；LoadIndex 后从 CatalogMeta 恢复
39. **IndexInfo::CreateIndex key_size 上限**：超 248 字节报错

### 事务/恢复
40. **为什么 ExecuteTrxBegin 直接 return DB_FAILED**：本作业不实现事务
41. **Txn 接口**：txn_id、iso_level、state、shared_lock_set、exclusive_lock_set

---

## 11. trace 一条 SQL 的完整流程

`select id, name from account where balance >= 100 and balance < 200;`

1. **lex** 读 `select id , name from account where balance >= 100 and balance < 200 ;`
2. **yacc** 用 Bison 规约，最后调用 `sql_select` 的 action：
   - 创 `kNodeSelect`，挂上 `kNodeColumnList(id, name)` + `kNodeIdentifier(account)` + `kNodeConditions`
3. **SyntaxTreePrinter** 输出 syntax_tree_24.txt
4. **ExecuteEngine::Execute** 走 default 分支
5. **Planner::PlanQuery** → `SelectStatement::SyntaxTree2Statement`：
   - `kNodeIdentifier` → `table_name_ = "account"`
   - `kNodeColumnList` → 解析 id, name 进 `column_list_`
   - `kNodeConditions` → `MakePredicate`：
     - 顶层是 `kNodeConnector(and)` → MakeLogicExpression
     - 左：`kNodeCompareOperator(>=)` → MakeComparisonExpression(col=balance, val=100)
     - 右：`<` → 同理
6. **PlanSelect**：
   - 查 indexes_：假设没有 → 走 SeqScanPlanNode
7. **ExecutePlan** → `CreateExecutor` → `SeqScanExecutor`
8. **Init**：GetTable("account") + TableHeap::Begin
9. **Next** 循环：
   - iterator 走每行
   - `where_->Evaluate(row)`：递归比较，返回 kTypeInt 0/1
   - 命中 1 → TupleTransfer (因为 select * schema 跟 table 不同) → push 到 result_set
10. **打印**：用 ResultWriter 输出表

---

## 12. 已知设计取舍 / TODO

- 事务（TrxBegin/Commit/Rollback）未实现
- 唯一性检查只在 InsertExecutor 做，Delete 留下的"墓碑"靠 IsDeleted 过滤
- IndexScan 中 `lhs.empty()` 短路那段：分不清"零匹配"和"列未建索引"
- `LogicExpression::Char2Type` 接受的是 `char*` 而 `MakePredicate` 传 `ast->val_`
- `CatalogMeta::GetNextTableId/IndexId` 用 `rbegin()->first+1` —— 删中间后 id 不复用 ✓
- DeletePage 需要 pin_count=0 且要先 FetchPage 一次让 page 进 page_table_
- 读 `INDEX_ROOTS_PAGE_ID` 在 BPlusTree 构造时一定要先 Fetch + Unpin

---

## 13. 文件速查表

| 模块 | 头文件 | cpp |
|---|---|---|
| Common | `include/common/*` | `common/instance.cpp` |
| Parser | `include/parser/minisql.l/y/syntax_tree.h` | `parser/syntax_tree.c parser.c syntax_tree_printer.cpp` |
| Planner | `include/planner/planner.h` `expressions/*` `statement/*` | `planner/planner.cpp` |
| Executor | `include/executor/execute_engine.h` `executors/*` `plans/*` | `executor/*.cpp` |
| Page | `include/page/*` | `page/*.cpp` |
| Buffer | `include/buffer/*` | `buffer/*.cpp` |
| Storage | `include/storage/*` | `storage/*.cpp` |
| Record | `include/record/*` | `record/*.cpp` |
| Index | `include/index/*` | `index/*.cpp` |
| Catalog | `include/catalog/*` | `catalog/*.cpp` |
| Concurrency | `include/concurrency/*` | `concurrency/*.cpp` |
| Recovery | `include/recovery/*` | (no cpp) |

