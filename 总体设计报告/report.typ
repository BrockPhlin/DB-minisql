#import "lib.typ": *

#show: bubble.with(
  title: "MiniSQL 总体设计报告",
  subtitle: "DB-minisql",
  author: "程文博 · 彭逸涵 · 胡海洋",
  affiliation: "浙江大学 计算机科学与技术学院",
  year: "数据库系统, 2026春夏",
  date: datetime.today().display("[year] 年 [month padding:none] 月 [day padding:none] 日"),
)


#outline(title: "目录")
#pagebreak()

= 前言

本项目实现了一个基于 C++ 的 MiniSQL 数据库系统。系统以页式存储为底层基础，在其上实现缓冲池、记录管理、B+ 树索引、目录管理、执行引擎、恢复管理和锁管理等模块，最终支持数据库、表、索引的管理，以及 `select`、`insert`、`update`、`delete` 等基本 SQL 操作。

总体设计部分主要围绕系统分层、模块接口和关键执行流程展开。各模块的细节实现已在个人报告中分别说明，这里更关注模块之间如何组织，以及一次 SQL 执行如何从解析结果逐层落到表、索引和磁盘页。

项目采用 CMake 构建，核心代码位于 `src` 目录，测试代码位于 `test` 目录。底层依赖 GoogleTest 完成单元测试，主程序通过命令行交互和 `execfile` 支持端到端验收。

小组按模块推进工作。每个模块设一名主要负责人，其他成员参与接口讨论、联调、测试复核和文档互审。

- 第 1、2 部分：Disk and Buffer Pool Manager、Record Manager，程文博主要负责，彭逸涵、胡海洋协作。
- 第 3、4 部分：Index Manager、Catalog Manager，彭逸涵主要负责，程文博、胡海洋协作。
- 第 5、6 部分：Executor Engine、Recovery Manager，胡海洋主要负责，程文博、彭逸涵协作。
- 第 7 部分：Lock Manager，胡海洋主要负责，程文博、彭逸涵协作。
- 系统联调、验收测试和总体设计报告由三人共同完成，各自重点复核自己主要负责模块与跨模块接口。

第七部分 Lock Manager 也纳入本次实现范围，因此后文单独设置并发控制章节，并保留对应思考题。

= 第一章 MiniSQL 总体架构

== 系统功能

MiniSQL 的目标是实现一个小型关系数据库系统。系统需要支持数据库文件的创建和加载、表和索引的定义、记录的插入删除修改查询，以及必要的持久化和测试能力。相比只在内存中保存数据的简单解释器，本项目要求将表数据、索引结构和目录信息都落到数据库文件中，并在进程重启后重新加载。

从功能上看，系统可以分为三类：

- 存储功能：磁盘页分配、缓冲池换入换出、表页和记录持久化。
- 查询功能：SQL 解析、执行计划生成、火山模型执行器、结果输出。
- 管理功能：Catalog 元信息管理、B+ 树索引管理、恢复和并发控制。

== 分层结构

系统整体采用自下而上的分层结构：

+ Disk Manager 负责数据库文件中的物理读写，并通过位图页管理逻辑页分配状态。
+ Buffer Pool Manager 负责在内存页框和磁盘页之间建立缓存层，向上隐藏磁盘 I/O 细节。
+ Record Manager 在页式存储上实现行记录、Schema 和堆表。
+ Index Manager 在 Buffer Pool 上构建磁盘 B+ 树，为表记录提供快速访问路径。
+ Catalog Manager 维护表和索引元信息，并把上层执行器与底层表、索引对象连接起来。
+ Planner 和 Executor 根据语法树生成并执行计划，完成用户 SQL 的实际语义。
+ Recovery Manager 和 Lock Manager 分别处理恢复和并发控制相关问题。

这种分层的好处是每个模块都只直接依赖相邻的下层接口。比如 Executor 不直接读写磁盘页，而是通过 Catalog 找到 TableHeap 和 Index；Catalog 也不直接操作文件，而是通过 Buffer Pool 读写自己的元信息页。

== SQL 执行主流程

系统对 SQL 语句采用两条执行路径。

DDL 和 Utility 语句的语义较直接，例如 `create database`、`drop table`、`show indexes`、`execfile` 等。这类语句在 `ExecuteEngine::Execute` 中根据 AST 节点类型直接分发到对应处理函数，再调用 Catalog 或数据库实例完成操作。

DML 语句包括 `select`、`insert`、`update`、`delete`。这类语句先由 Planner 将 AST 转换为逻辑执行计划，再由 ExecuteEngine 根据 PlanType 创建对应 Executor。各 Executor 通过统一的 `Init` / `Next` 接口迭代执行，这也是本项目采用的火山模型执行方式。

一次查询大致经过如下路径：

```text
SQL 文本
  -> Parser 生成 AST
  -> ExecuteEngine 判断语句类型
  -> DDL/Utility: 直接调用 Catalog 或数据库接口
  -> DML: Planner 生成 Plan
  -> ExecuteEngine 创建 Executor
  -> Executor 访问 Catalog / TableHeap / Index
  -> Buffer Pool 读写页
  -> Disk Manager 持久化到数据库文件
```

== 跨模块数据流

系统中几个核心对象贯穿了多层模块：

- AST 是 SQL 语句进入执行层后的结构化表示，ExecuteEngine 根据 AST 根节点选择 DDL/Utility 路径或 Planner 路径。
- Schema 描述表和索引的列结构，Catalog、Record、Executor 和 Index 都围绕 Schema 传递列信息。
- RowId 连接表记录和索引项，TableHeap 用它定位记录，B+ 树叶子页用它作为索引值。
- GenericKey 是索引键在 B+ 树页中的二进制表示，索引模块围绕它完成 Row、Schema 与页内键值对之间的转换和比较。
- Txn 在线程执行、锁管理和表记录访问之间传递事务状态，Lock Manager 根据 Txn 的隔离级别和状态授予或拒绝锁请求。

= 第二章 存储与缓冲层设计

== Disk Manager

Disk Manager 是系统中最底层的持久化模块。它负责把上层看到的逻辑页号映射到数据库文件中的物理页，并提供页的分配、释放、读取和写入接口。

本项目没有把所有数据页简单连续编号，而是采用 Meta Page + Bitmap Page + Data Pages 的文件组织方式。物理页 0 是 Disk Meta Page，保存全局已分配页数、Extent 数量以及每个 Extent 的使用情况。每个 Extent 由一个 Bitmap Page 和一组连续的数据页组成，Bitmap Page 中的每一位表示对应数据页是否已经被分配。

逻辑页号到物理页号的映射关系由 `MapPageId` 完成。上层模块只处理连续的逻辑页号，不需要知道中间穿插了 Meta Page 和 Bitmap Page。这样既保留了页分配状态的持久化能力，也让上层接口保持简单。

页分配时，Disk Manager 先根据全局计数判断当前 Extent 是否还有空间。如果现有 Extent 已满，则新建一个 Bitmap Page 作为新的 Extent 起点。随后读取对应 Bitmap Page，在其中找到一个空闲 bit 并置为已分配。释放页时则反向更新对应 bit 和元信息计数。

== Buffer Pool Manager

Buffer Pool Manager 位于 Disk Manager 之上，负责把磁盘页缓存到固定大小的内存页框中。它维护四类核心状态：

- `pages_`：内存中的 Page 数组。
- `page_table_`：逻辑页号到 frame id 的映射。
- `free_list_`：从未使用或已经归还的空闲 frame。
- `replacer_`：记录当前可淘汰的 frame。

`FetchPage` 先查 page table。如果目标页已经在缓冲池中，则增加 pin count 并返回；如果未命中，则从 free list 或 replacer 中选择一个 frame。被替换的 frame 如果是脏页，需要先写回磁盘，再读入新页。

`NewPage` 的流程与 `FetchPage` 类似，不同之处在于它会先向 Disk Manager 申请新的逻辑页号，再把对应 frame 初始化为新页。`UnpinPage` 会减少 pin count，当 pin count 降为 0 时，该 frame 才能进入替换器。`FlushPage` 把脏页写回磁盘，`DeletePage` 在页未被 pin 时释放页并回收 frame。

这一层的关键约束是 pin / unpin 必须成对出现。上层模块每次从 Buffer Pool 取页后，都需要在使用结束时释放，否则页面会一直无法被替换，测试中的 `CheckAllUnpinned` 也会暴露这类问题。

== 页面替换策略

基础替换策略采用 LRU Replacer。它用链表和哈希表维护可淘汰 frame，链表头部表示最近使用，尾部表示最久未使用。`Victim` 从尾部取出 frame，`Pin` 将 frame 从替换器中移除，`Unpin` 将 frame 加入替换器。

作为扩展，本组还实现了 Clock Replacer。Clock Replacer 为每个 frame 维护 reference bit。淘汰时遇到 reference bit 为 1 的 frame，会先将其清零并给予第二次机会；遇到 reference bit 为 0 的 frame 才真正淘汰。该设计牺牲了严格 LRU 顺序，但实现简单，开销稳定，适合作为 Buffer Pool 替换策略的补充实现。

= 第三章 Record Manager 设计

== 记录模型

Record Manager 管理表中的行记录和模式信息。本项目中的记录相关对象包括：

- `Column`：字段定义，保存字段名、类型、长度、列下标、是否可空、是否唯一等信息。
- `Schema`：表或索引的字段集合。
- `Field`：一条记录中某一列的具体值。
- `Row`：由多个 Field 组成的一条记录，同时保存 RowId。

其中 RowId 是连接 Record Manager 和 Index Manager 的关键。表中每条记录的物理位置由 RowId 表示，索引叶子页中保存的 value 也正是 RowId。

== 序列化与反序列化

为了将内存对象持久化到页中，`Column`、`Schema` 和 `Row` 都实现了序列化、反序列化和大小计算。

`Column` 和 `Schema` 序列化时会写入 magic number。反序列化时先校验 magic number，可以避免把错误的字节流解释为合法元信息。字符串采用长度前缀保存，而不是依赖 `\0`，这样可以与后续二进制字段紧密排列。

`Row` 的序列化需要处理空值。设计中先写入字段数量，再写入 null bitmap。bitmap 的每一位对应一列是否为空；非空字段才继续写入 Field 的实际内容。这样既能区分空字符串和 NULL，也能避免为空字段浪费额外空间。

== TablePage

TablePage 采用 Slotted Page 布局。页头保存前后页号、空闲空间指针、tuple 数量以及扩展 hint；slot 区从页头向后增长，实际 tuple 数据从页尾向前增长。中间未使用的区域就是当前页的空闲空间。

每个 slot 保存 tuple 的 offset 和 size。删除分为两个阶段：`MarkDelete` 只设置删除标记，`ApplyDelete` 才真正移动数据并回收空间。这种设计为事务回滚和恢复预留了接口。

更新记录时，如果新记录能够在原页中放下，则在页内调整数据布局；如果空间不足，则由 TableHeap 走删除加重新插入的路径。

== TableHeap 与迭代器

TableHeap 通过页链表组织一张表的全部数据页。插入记录时，TableHeap 从首页开始查找能够容纳该记录的页面；如果所有页面都没有足够空间，则申请新页并链接到表的页链尾部。

查询和删除都通过 RowId 定位到具体 TablePage。TableIterator 则沿着页内 slot 和页间链表顺序遍历表中所有有效记录，为 SeqScanExecutor 和建索引时的已有数据回填提供基础。

== Hint 优化

为了减少 TablePage 中重复线性扫描的开销，本组在页头中加入了两个 hint：第一个空闲 slot 的候选位置，以及第一个有效 tuple 的候选位置。hint 只作为扫描起点，不作为正确性的唯一依据。当 hint 失效时，相关函数仍会回退扫描剩余区域，因此不会影响数据正确性。

= 第四章 Index Manager 设计

== 模块职责

Index Manager 负责在表记录之上提供快速查找能力。本项目实现的是基于磁盘页的 B+ 树索引。索引键由一列或多列 Field 序列化得到，叶子页保存索引键到 RowId 的映射，内部页保存索引键到子页号的映射。

B+ 树中的比较和定位都围绕序列化后的索引键进行。Index Manager 需要在插入、删除、分裂、合并、范围扫描和根页持久化过程中保持树结构不变量。

== B+ 树页结构

B+ 树页分为三类：

- `BPlusTreePage`：公共页头，保存页类型、键长度、当前大小、最大容量、父页号和当前页号。
- `BPlusTreeLeafPage`：叶子页，顺序保存 `(key, RowId)`，并通过 `next_page_id` 连接成叶子链表。
- `BPlusTreeInternalPage`：内部页，顺序保存 `(key, child_page_id)`，其中第 0 个 key 作为占位，查找时从第 1 个有效 key 开始比较。

这些页对象并不是单独分配的 C++ 对象，而是直接存放在 Buffer Pool 的 `Page::data_` 中。使用时先通过 Buffer Pool 取页，再用 `reinterpret_cast` 将数据区解释为对应的 B+ 树页类型。

== 查找、插入与删除

查找从根页开始，内部页根据 key 比较选择子页，直到定位到叶子页。叶子页内部通过有序数组进行二分查找。点查返回对应 RowId，范围扫描则通过迭代器沿叶子链表继续向后遍历。

插入时，如果树为空，则创建一个叶子页作为根。非空树中，先找到目标叶子页并检查唯一键约束；插入后如果页未溢出，流程结束；如果叶子页溢出，则分裂出新叶子页，并把新叶子的最小 key 插入父节点。父节点也可能继续溢出，因此插入过程可能向上递归，直到创建新的根节点。

删除时，先在叶子页中移除目标 key。如果删除后节点仍满足最小大小要求，则无需调整；否则在兄弟节点之间尝试重分配。如果当前节点与兄弟节点合并后不超过最大容量，则执行合并并删除父节点中的分隔 key。根节点需要单独调整：根叶子为空时树变为空；根内部节点只剩一个孩子时降低树高。

== 根页持久化

仅把 root page id 保存在 B+ 树对象成员中是不够的，因为进程重启后该值会丢失。本项目通过固定的 IndexRootsPage 保存 `index_id -> root_page_id` 的映射。每次根页变化时，B+ 树都会更新该页。Catalog 重新加载索引时，B+ 树构造函数再通过该映射找回根页。

== IndexIterator

IndexIterator 持有当前叶子页和元素下标。递增迭代器时，如果当前页还有元素，则只移动下标；如果已经到达页尾，则沿 `next_page_id` 取下一个叶子页。迭代器需要正确处理页的 pin / unpin，避免遍历过程中页面被过早释放或长时间固定在缓冲池中。

= 第五章 Catalog Manager 设计

== 模块职责

Catalog Manager 维护数据库中的所有模式信息，包括表定义、字段定义、索引定义，以及这些元信息所在的页号。它是上层执行器和底层存储结构之间的连接点：Executor 通过 Catalog 找到 TableHeap 和 Index，IndexInfo 也通过 Catalog 中的表 Schema 构造自己的 key schema。

== 元信息持久化

Catalog 的持久化由三类结构完成：

- `CatalogMeta`：固定存放在 `CATALOG_META_PAGE_ID`，记录所有 table id 到表元信息页、index id 到索引元信息页的映射。
- `TableMetadata`：每张表独占一个元信息页，保存表 id、表名、表数据首页页号和 Schema。
- `IndexMetadata`：每个索引独占一个元信息页，保存索引 id、索引名、所属表 id 和索引列映射。

表数据和索引数据本身不直接放在 Catalog 页中。Catalog 只记录如何找到这些对象，以及如何重建内存中的 `TableInfo` 和 `IndexInfo`。

== 内存结构

Catalog Manager 在内存中维护表名、表 id、索引名和索引 id 之间的映射。这样上层按名称查表或查索引时，不需要每次都扫描磁盘元信息页。

新建数据库时，Catalog 初始化空的 CatalogMeta，并初始化 IndexRootsPage。打开已有数据库时，加载顺序是先读 CatalogMeta，再加载所有表，最后加载所有索引。索引必须在表之后加载，因为 `IndexInfo::Init` 需要依赖所属表的 Schema。

== 表管理

建表时，Catalog 先检查表名是否重复，然后分配表元信息页，深拷贝传入 Schema，创建 TableHeap，并把 TableMetadata 写入元信息页。之后更新内存映射和 CatalogMeta。

删表时，需要先删除该表上的所有索引，再释放表数据页和表元信息页。如果先删除表对象，索引中的 key schema 可能还引用表 Schema，容易产生悬空引用。因此删除顺序是 Catalog Manager 中比较重要的生命周期约束。

== 索引管理

建索引时，Catalog 先检查表是否存在、索引名是否重复，并将索引列名转换为表 Schema 中的列下标。随后创建 IndexMetadata 和 IndexInfo。对于已经存在的数据，还需要遍历 TableHeap，把每一行的索引 key 和 RowId 插入 B+ 树。否则新建索引只能服务后续插入的数据，会漏掉已有记录。

删索引时，需要调用 B+ 树索引的 `Destroy` 删除索引数据页，并从 CatalogMeta、内存映射和 IndexRootsPage 中清理相关状态。

== Schema 深浅拷贝

Schema 生命周期是 Catalog Manager 中容易出错的地方。表元信息需要拥有自己的 Schema，因此建表时使用深拷贝。索引 Schema 只引用表 Schema 中的一部分列，因此使用浅拷贝。这样既避免重复保存 Column，又能保证索引列定义与表定义一致。

= 第六章 Executor 与查询执行设计

== 执行入口

Executor Engine 对外提供统一入口 `Execute(pSyntaxNode ast)`。该函数先判断 AST 根节点类型。对于 DDL 和 Utility 语句，直接调用对应的私有执行函数；对于 DML 语句，则创建 Planner，将 AST 转换为执行计划，再调用 `ExecutePlan` 构造具体 Executor。

系统启动时，ExecuteEngine 会扫描 `./databases` 目录，将已有数据库文件加载为 `DBStorageEngine`。测试环境可以通过构造参数关闭自动加载，以避免测试之间受到旧数据库文件影响。

== DDL 与 Utility 语句

DDL 和 Utility 语句主要包括：

- 数据库级操作：`create database`、`drop database`、`show databases`、`use database`。
- 表级操作：`create table`、`drop table`、`show tables`。
- 索引级操作：`create index`、`drop index`、`show indexes`。
- 辅助操作：`execfile` 和 `quit`。

这些语句不需要经过复杂的执行计划。以建表为例，执行器从 AST 中解析表名、列名、类型、长度和约束，构造 Schema 后调用 CatalogManager 创建表。建索引则解析索引名和索引列列表，再交由 CatalogManager 完成元信息创建和索引回填。

`execfile` 的设计是读取文件内容，按分号拆分 SQL 语句，并对每条语句重新初始化 Parser 状态后递归调用 `Execute`。这样可以复用普通交互模式下的执行路径。

== Planner 与执行计划

DML 语句由 Planner 生成执行计划。`insert` 会生成 ValuesPlan 与 InsertPlan；`delete` 和 `update` 会先生成扫描计划，再包裹 DeletePlan 或 UpdatePlan；`select` 根据谓词和可用索引决定使用 SeqScanPlan 或 IndexScanPlan。

这里的设计把“如何执行”从“如何解析”中分离出来。Parser 只负责给出语法树，Planner 负责把语法树转换成计划，Executor 负责按计划访问数据。

== 火山模型 Executor

Executor 统一采用 `Init` / `Next` 接口。`Init` 完成表、索引、子执行器等状态初始化；`Next` 每次返回一条记录，直到返回 false 表示执行结束。

主要执行器包括：

- `ValuesExecutor`：把 insert 语句中的常量值转为 Row。
- `SeqScanExecutor`：遍历 TableHeap，并根据谓词过滤记录。
- `IndexScanExecutor`：利用索引得到候选 RowId，再回表读取完整记录；必要时继续用谓词过滤。
- `InsertExecutor`：从子执行器取 Row，检查唯一索引约束，写入 TableHeap，并同步更新所有索引。
- `DeleteExecutor`：从子执行器取待删除记录，标记删除并移除索引项。
- `UpdateExecutor`：生成更新后的 Row，更新 TableHeap，并先删除旧索引项再插入新索引项。

通过这种接口，插入、删除和更新都可以把扫描逻辑作为子执行器复用，避免每种 DML 语句都重新实现过滤和遍历。

= 第七章 Recovery Manager 设计

== 模块定位

Recovery Manager 在本项目中采用简化的内存级设计，用 `unordered_map` 模拟数据库中的键值数据，将恢复算法本身从页结构、刷盘策略和日志落盘细节中拆出来，便于单元测试验证 Redo 和 Undo 的正确性。

== 日志结构

日志记录 `LogRec` 保存事务恢复需要的信息，包含日志类型（插入、删除、更新、事务开始、提交和中止）、LSN、同事务上一条日志的 `prev_lsn`、事务 id，以及数据操作所需的 key、old value 和 new value。`prev_lsn` 将同一事务的日志串成反向链，Undo 时可以从事务最后一条日志沿链回溯。日志集合使用 `std::map<lsn_t, LogRecPtr>` 保存，Redo 阶段按 LSN 升序重放，Undo 阶段按 LSN 降序扫描。

== CheckPoint

CheckPoint 表示一次恢复开始时的持久化快照，包含检查点 LSN、检查点时刻的活跃事务表和持久化数据。RecoveryManager 的 `Init` 阶段会用 CheckPoint 恢复这些状态。

== Redo 与 Undo

Redo 阶段从检查点 LSN 之后开始，按日志顺序重放所有操作。提交日志会把事务从活跃事务表中删除；开始日志会加入活跃事务；插入、删除、更新分别按照 after image 重做。遇到 Abort 日志时，系统沿该事务的 `prev_lsn` 链反向撤销该事务已经产生的修改，并将该事务从活跃事务表移除。Undo 阶段处理 Redo 后仍留在活跃事务表中的事务，逆序扫描日志，把它们的修改全部撤销。插入的逆操作是删除 key，删除和更新的逆操作是恢复 old value。该实现省略了真实 ARIES 中的 CLR 和页 LSN 判断，但保留了日志驱动恢复、检查点、Redo、Undo 和事务日志链这些核心思想。

== 思考题

本模块中为了简化实验难度，将 Recovery Manager 模块独立出来，用 `unordered_map` 在内存中模拟键值数据，避免涉及页结构、刷盘和日志落盘细节。如果不独立，真正做到数据库在任何时候断电都能恢复，同时支持事务的回滚，Recovery Manager 应按如下方式设计。

=== 1. 日志格式与 WAL 协议

*设计思路*：当前实验中的 `LogRec` 以 `std::string` 为 key、`int32_t` 为 value，这是内存 KV 模拟的简化表示。真实集成到数据库系统中，`LogRec` 必须描述"哪个表的哪个记录被怎样修改"。

日志记录需重新定义字段：

```
log_type: kInsert | kDelete | kUpdate | kBegin | kCommit | kAbort | kCLR
         | kCheckPointBegin | kCheckPointEnd
lsn:     单调递增的全局日志序列号
txn_id:  事务标识
prev_lsn: 同事务上一条日志的 LSN（用于 Undo 链）
table_id: 操作涉及的表
rid:     RowId → (page_id, slot_num)
old_row: 更新前的完整 Row 序列化（Delete/Update 需要）
new_row: 更新后的完整 Row 序列化（Insert/Update 需要）
undo_next: 仅 CLR 日志使用，指向下一条需 Undo 的日志
```

`old_row` 和 `new_row` 采用 `Row::SerializeTo` 的结果，写入日志时带 4 字节长度前缀。对于整条记录 INSERT/DELETE/UPDATE 的日志体积，按每行约 200 字节估计，一条日志约 400-500 字节。CLR 日志的 `undo_next` 指向该 CLR 所补偿的那条原始日志的 `prev_lsn`，这样在重启恢复时遇到 CLR 就不会继续沿着已补偿过的链重复 Undo。

*关键要点*：
- `kCheckPointBegin` 和 `kCheckPointEnd` 用于标记检查点边界。若恢复时只找到 `kCheckPointBegin` 后面没有配对的 `kCheckPointEnd`，说明上次检查点未完成，回退到上一个完整检查点。
- 每条日志的磁盘存储格式：`[length: 4B] [crc: 4B] [LogRec 序列化数据]`。CRC 校验覆盖 `length` + `LogRec` 序列化数据部分，用于检测日志文件在磁盘上的部分写入或静默损坏。计算 CRC 时使用 CRC32C（硬件加速的 iSCSI 变体），其碰撞概率在 32 位长度下已足够低。
- LogManager 在内存中维护预分配日志缓冲区 `log_buffer_`（典型大小为 4MB，即一个磁盘页簇的大小），日志先写入缓冲区。刷盘触发条件：
  (a) `log_buffer_` 剩余空间不足以容纳下一条日志 → 调用 `Flush()` 将缓冲区写入日志文件并从 `flushed_lsn_` 更新；
  (b) 事务提交时，COMMIT 日志必须在 `Commit()` 返回前确保落盘（Group Commit 优化后可批量刷盘以提升吞吐）；
  (c) 后台线程每 10ms 检查缓冲区是否有待刷数据，周期性刷盘降低单条日志延迟。

*与当前实现的关系*：目前 `LogManager` 是一个空类，改造后需承担缓冲、刷盘、序列化和文件管理四个职责。磁盘布局上，日志文件与数据文件分开存储（如 `database.minisql.wal`），避免日志写入与数据页竞争同一文件偏移。恢复时先读取 WAL 文件全部日志，构造内存中的 `std::map<lsn_t, LogRecPtr>` 日志集合，供 Recovery Manager 检索。

=== 2. Page LSN 与 WAL 语义

*设计思路*：ARIES 协议要求每个数据页记录最后一次修改所对应的日志 LSN，以此实现"不重复 Redo、不遗漏 Redo"的判断依据。

当前代码中，`TablePage` 页头已预留 `LSN` 字段（偏移量为 `OFFSET_LSN = 4`），但 `GetLSN()` / `SetLSN()` 方法尚未实现，各修改函数也没有更新 LSN 的逻辑。`BPlusTreePage` 基类中已经提供了 `SetLSN(lsn_t lsn)`，但 B+ 树的插入、删除、分裂、合并函数在修改页数据后并未调用它。

*需要修改的地方*：

- `TablePage::InsertTuple`：在成功写入 slot 后，调用 `SetLSN(log_lsn)`，其中 `log_lsn` 是本次操作所写日志的 LSN。
- `TablePage::MarkDelete`：设置删除标记后更新页 LSN（注意 `MarkDelete` 不产生 Redo 日志，但可以作为 NTA 优化的一部分记录日志，简化起见要求也更新 LSN）。
- `TablePage::ApplyDelete`：`memmove` 回收空间后更新 LSN。
- `TablePage::UpdateTuple`：in-place 更新完成后更新 LSN。
- B+ 树的 `Insert`、`Remove`、`MoveHalfTo`、`Coalesce`：在修改内部页或叶子页的 `array_` 和 `size_` 之后更新页 LSN。

*WAL 核心不变量*：脏页刷回磁盘前，该页的 `page_lsn` 必须 ≤ `flushed_lsn_`（即该页上所有修改对应的日志都已落盘）。这个检查在 `BufferPoolManager::FlushPage` 中实现：

```cpp
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  frame_id_t fid = it->second;
  lsn_t page_lsn = pages_[fid].GetLSN();  // 从页头读取
  if (page_lsn > log_manager_->GetFlushedLSN()) {
    log_manager_->Flush();                // 保证日志先于数据落盘
  }
  if (pages_[fid].IsDirty()) {
    disk_manager_->WritePage(page_id, pages_[fid].GetData());
    pages_[fid].is_dirty_ = false;
  }
  return true;
}
```

这条规则保证了崩溃恢复的基本前提：磁盘上的任何脏数据，其对应的 UNDO 日志一定已经安全落盘，所以 Recovery Manager 总可以看到它需要的所有信息。

=== 3. 日志驱动的磁盘级恢复

集成到真实存储层的 Recovery Manager 不再依赖 `std::map<std::string, int32_t>` 模拟数据，而是直接对 Buffer Pool 中的物理页数据进行 Redo 和 Undo。整体恢复流程遵循 ARIES 框架的三个阶段。

*Analysis 阶段*：从最近一次完整 CheckPoint 中恢复出"崩溃时刻的活跃事务集合（ATT）"和"脏页表（DPT）"，然后正向扫描检查点之后的日志更新这两个集合。

```
ATT := { CheckPoint 记录的活跃事务 id → last_lsn }
DPT := { CheckPoint 记录的脏页 page_id → 最早 rec_lsn }
for log from checkpoint_lsn to end:
  if log.type == kBegin:
    ATT[txn_id] = log.lsn
  elif log.type == kCommit or log.type == kAbort:
    ATT.erase(txn_id)
  elif log is data operation:
    if DPT doesn't contain page_id:
      DPT[page_id] = log.lsn
    set undo_next_lsn for ATT[txn_id]
```

Analysis 结束后，ATT 中包含所有"未提交也未中止"的事务，它们将在 Undo 阶段被回滚。DPT 中包含所有可能被修改过但不一定已落盘的页及其最早相关日志 LSN。

*Redo 阶段*：从 `DPT` 中取最小的 `rec_lsn`，从该位置开始正向重放日志。对每条数据操作日志：

- 从 Buffer Pool 中 `FetchPage(log.page_id)` 获取目标页。
- 比较 `page.GetLSN() < log.lsn`。若为真，说明该日志的修改尚未反映到磁盘页上，将 `log.new_row`（after image）应用到页上，并更新页 LSN 为 `log.lsn`。
- 若 `page.GetLSN() >= log.lsn`，则该修改已在磁盘上，跳过。
- 应用结束后 `UnpinPage`，设为 dirty。

`kCLR` 日志同样需要 Redo，因为 CLR 记录的是 Undo 操作对页的物理修改，这些修改可能在 Undo 过程中因系统再次崩溃而丢失，需要 Redo 来保证其持久性。

*Redo 的根本目的*：将数据库状态恢复到崩溃瞬间的"物理一致性"状态——所有已提交事务的修改都反映在页数据中，即使这些页在崩溃时尚未刷盘。

*Undo 阶段*：对 ATT 中所有事务按 `last_lsn` 降序处理（即最后开始的事务先回滚）。对每个事务，沿 `prev_lsn` 链从最后一条日志逆向扫描：

- 遇到 `kInsert` 日志：Undo 语义是删除插入的行。在 Buffer Pool 中按 `table_id` 和 `rid` 找到目标页，调用 `TablePage::ApplyDelete`，并在 WAL 中写一条 CLR 日志（undo_next 指向 `log.prev_lsn`）。CLR 的作用是把 Undo 操作本身记录下来：如果 Undo 中途系统再次崩溃，重启后 Redo 阶段的 CLR 重放能恢复 Undo 的进度，而再次进入 Undo 时会跳过 CLR 沿 `undo_next` 继续。
- 遇到 `kDelete` 日志：Undo 语义是将 `old_row` 重新插入。在目标页中调用 `TablePage::InsertTuple` 恢复旧记录，写 CLR。
- 遇到 `kUpdate` 日志：Undo 语义是用 `old_row` 覆盖 `new_row`。调用 `TablePage::UpdateTuple` 恢复旧值，写 CLR。
- 遇到 `kCLR` 日志：不执行 Undo 动作，沿 `undo_next` 跳过已补偿的日志，继续逆向。

Undo 完成后，事务状态置为 Aborted，各 Executor 中 `TxnAbortException` 被捕获后由 ExecuteEngine 调用 `TxnManager::Abort`。

=== 4. CheckPoint 机制详细设计

*设计思路*：检查点的作用是在日志链中插入"快照标记"，使得恢复时不需要从日志文件头部开始扫描。本实验当前使用简单的"内存数据快照"模式，完整的 CheckPoint 需要支持模糊检查点（Fuzzy Checkpoint），即检查点过程中系统仍可正常接受读写请求，不阻塞事务执行。

*触发条件*：
- 周期触发：后台线程每 60 秒（或通过配置参数 `checkpoint_interval_sec` 指定）触发一次。
- 日志量阈值：当日志文件大小超过当前数据文件大小 × `log_ratio_threshold`（如 0.5）时触发，避免日志文件无限膨胀。这是因为日志记录了所有修改的历史，若不定期截断，恢复时间会线性增长。
- 正常关闭：`ExecuteQuit` 或 `drop database` 时显式触发一次完整检查点并等其完成，之后可安全删除 WAL 文件。
- 手动触发：支持 `CHECKPOINT` SQL 语句，供管理员在低负载时段主动执行。

*执行流程*：

1. 写 `kCheckPointBegin` 日志并记录其 LSN 为 `ckpt_begin_lsn`。
2. 遍历 `TxnManager` 的活跃事务表，为每个活跃事务记录 `(txn_id, last_lsn, state)` 三元组。这里的关键是"先读事务表再读脏页表"的顺序不需要严格全局锁：事务可能在下一步扫描脏页时变成 inactive，但这只会导致 ATT 中多包含一个已提交事务（Undo 时它会因找不到未提交日志而自然跳过），不会导致正确性问题。
3. 遍历 `BufferPoolManager` 中所有 `pin_count == 0` 的 frame，收集 `(page_id, page_lsn)` 构造脏页表 DPT。注意此处 *不能* 等待 pin count 降为 0，否则长事务可能永远阻塞检查点。解决方案是：对 `pin_count > 0` 的脏页，在 DPT 中将其 `rec_lsn` 设为 `ckpt_begin_lsn`（即保守地认为检查点开始后该页才变脏），这样 Redo 时会从检查点开始重放该页涉及的所有日志，虽然多做了工作但保证正确性。
4. 将 ATT 和 DPT 序列化，连同 `ckpt_begin_lsn` 写入 `kCheckPointEnd` 日志。
5. 调用 `LogManager::Flush()` 确保所有日志（包括两条检查点日志）落盘。
6. 将 `kCheckPointEnd` 日志的 LSN 写入 `DiskFileMetaPage` 的新增字段 `last_checkpoint_lsn`（位于 Meta Page 的固定偏移处），并刷盘。这一步是原子的：只有 `last_checkpoint_lsn` 写入成功，本次检查点才生效。若写入过程中崩溃，元数据页中的 `last_checkpoint_lsn` 仍指向上一个有效检查点。

*故障处理*：
- 若恢复时从 `DiskFileMetaPage` 读到的 `last_checkpoint_lsn` 对应一条 `kCheckPointBegin` 日志，但后续找不到配对的 `kCheckPointEnd`（日志文件在检查点中途被截断），则向前搜索前一个完整的 `kCheckPointEnd` 作为恢复起点。实现上可以维护一个"最近 N 次检查点位置"的环形记录。
- 日志截断：完整检查点完成后，`checkpoint_lsn` 之前的所有日志理论上已经不再需要（对应修改已全部反映到数据页上）。但出于安全考虑，实际保留最近 2 个检查点之间的日志，以便在最后一个检查点损坏时仍可回退。

=== 5. 各模块需要修改的接口与数据结构

*Disk Manager*：
- `DiskFileMetaPage` 新增字段 `last_checkpoint_lsn_`（8 字节，偏移量紧接在 `extent_used_page_` 数组之后）。为确保 Meta Page 不超过 PAGE_SIZE，将 `extent_used_page_` 数组从 `MAX_EXTENTS = (PAGE_SIZE - 8) / 4 ≈ 1022` 缩减为 `(PAGE_SIZE - 16) / 4 ≈ 1020`，腾出 8 字节给 `last_checkpoint_lsn_`。
- 新增 `WritePhysicalPage` / `ReadPhysicalPage` 用于直接读写 Meta Page 的 `last_checkpoint_lsn_`（现有接口仅支持逻辑页映射）。

*Buffer Pool Manager*：
- `FlushPage` 中增加 WAL 前置检查（见上文）。
- 新增 `GetDirtyPages()` 接口，遍历 `pages_` 数组返回所有 `is_dirty_ == true` 且 `pin_count_ == 0` 的 `(page_id, lsn)` 列表，供 CheckPoint 使用。
- `Page` 类新增 `GetLSN()` 方法，从 `data_` 偏移 4 处读取 `lsn_t`。

*TablePage*：
- 实现 `lsn_t GetLSN()` 和 `void SetLSN(lsn_t lsn)`，从页头偏移 `OFFSET_LSN = 4` 处读写 8 字节的 LSN 值。注意 LSN 类型在 `log_manager.h` 中定义为 `int32_t`，需改为 `int64_t` 或 `uint64_t`（32 位 LSN 在 WAL 场景下可能不够：以每秒 10000 条日志计算，约 5 天就会溢出回绕）。
- `InsertTuple`、`MarkDelete`、`ApplyDelete`、`UpdateTuple` 各函数增加 `log_manager_` 参数（当前已有预留），在修改页数据后调用 `SetLSN`。

*B+ 树页*：
- `BPlusTreePage` 基类已有 `SetLSN` 方法声明，确认实现从页头固定位置读写 LSN（B+ 树页与 TablePage 共享页头 LSN 偏移）。
- `BPlusTreeLeafPage::Insert`、`Remove`、`MoveHalfTo`、`Coalesce` 在修改 `array_` / `size_` 后更新 LSN。
- `BPlusTreeInternalPage::InsertNodeAfter`、`Remove`、`MoveHalfTo`、`Coalesce` 同理。
- `AdjustRoot` 在修改 `IndexRootsPage` 上的 `root_page_id` 映射时也需要记录日志并更新 LSN。

*Recovery Manager*：
- 析构掉内存模拟 `data_` map，改为直接操作 Buffer Pool。
- 从日志文件中读入全部日志（构造 `std::map<lsn_t, LogRecPtr>`），并通过 `DiskFileMetaPage` 确定 `last_checkpoint_lsn`。
- `Redo` 阶段不写入内存 map，而是调用 `buffer_pool_manager_->FetchPage` 拿到物理页，比较 LSN 后将 after-image 写入 `page->GetData()` 对应偏移。
- `Undo` 阶段同理，沿 `prev_lsn` 链对物理页执行逆操作。

*TxnManager*：
- Commit 时先写 COMMIT 日志，调用 `LogManager::Flush()` 确保落盘，再释放锁并结束事务。
- Abort 时调用 `RecoveryManager::Undo(txn_id)`（在持锁下执行），再写 ABORT 日志，释放锁。

*Lock Manager*：
- 恢复流程开始时，清空 `lock_table_` 中所有锁请求。崩溃后未提交事务的锁全部无效。
- 死锁检测中 `DeleteNode` 将 victim 事务标记为 Aborted 后，该事务的后续 `CheckAbort` 会抛出异常，Executor 层捕获后沿调用栈回到 `ExecuteEngine`，由后者调用 `TxnManager::Abort`，期间 Undo 操作在持锁下进行。

=== 6. 与 Lock Manager 的协调及关键故障场景覆盖

*恢复第一步*：清空 `LockManager::lock_table_`。此时系统中不存在任何活跃事务，所有表级和行级锁请求、等待队列、条件变量均被重置。

*死锁 Abort 时的 Undo*：死锁检测将 victim 事务的 `state_` 异步置为 `kAborted`，该事务被 `CheckAbort` 检测到后抛出 `TxnAbortException`。异常传播到 `ExecutePlan` 的 catch 块，在 `TxnManager::Abort` 中调用 `RecoveryManager::Undo`。Undo 沿 `prev_lsn` 链撤销修改，*期间保持锁持有*——因为在 Undo 完成之前，该事务仍持有对受影响 RowId 的排他锁，其他事务不应看到部分回滚的数据。Undo 完成后写 ABORT 日志，调用 `Unlock` 释放全部锁。

*故障场景覆盖*：

(a) *事务执行中系统崩溃*：重启时分析日志，未提交事务进入 ATT，Undo 阶段回滚所有未提交修改。已提交但未刷盘的修改由 Redo 阶段恢复。

(b) *CheckPoint 中途崩溃*：检查 `DiskFileMetaPage.last_checkpoint_lsn` 指向的日志是否为有效的 `kCheckPointEnd`。若日志文件在 `kCheckPointBegin` 和 `kCheckPointEnd` 之间截断，恢复到上一个完整检查点即可，本次失败的检查点不会对数据库状态产生任何副作用（因为检查点只写日志和元数据页，不修改数据页）。

(c) *WAL 不变量保证*：`BufferPoolManager::FlushPage` 在写脏页之前检查页 LSN 是否已安全落盘。若系统在 Flush 中途崩溃（例如只写了半个页），重启后的 Redo 阶段会因为 `page_lsn < log_lsn` 而重放该页的所有修改，自然修复不完整的页写入。

(d) *CRC 校验检测日志损坏*：每条日志带有独立的 CRC32C 校验和。读取日志时若 CRC 不匹配，认为该日志及其后续日志已损坏。保守策略是截断到尾，将损坏点之后的所有日志视为不存在（相当于只恢复到损坏点之前的最后一个完整操作）。这可能导致少量已提交事务"丢失"，但不会破坏数据库的物理一致性。

(e) *Undo 中途再次崩溃*：此时页上同时存在"部分被 Undo 的数据"和"尚未被 Undo 的数据"。重启后 Analysis 阶段发现该事务仍在 ATT 中，Redo 阶段会把上次 Undo 写的 CLR 日志重放（恢复已完成的 Undo 步骤），Undo 阶段从 CLR 的 `undo_next` 继续逆向，不会重复执行已 Undo 的操作。这正是 CLR 和 `undo_next` 字段存在的意义。
= 第八章 并发控制与 Lock Manager

== 实验概述

本模块实现基于严格两阶段锁（Strict 2PL）的行级并发控制。系统支持 `READ_UNCOMMITTED`、`READ_COMMITTED`、`REPEATABLE_READ` 三种隔离级别，提供共享锁（S）和独占锁（X）两种锁模式。锁冲突通过 `std::condition_variable` 实现阻塞等待，后台周期性地运行基于等待图 DFS 的死锁检测。

实验中 Lock Manager 被独立出来，与 Executor 通过 `Txn` 对象交互而非直接集成在 `ExecuteEngine` 的执行路径中。这使得我们可以在单元测试中直接构造事务对象和锁请求序列来验证 2PL 语义与死锁检测逻辑，而不需要走完整的 Parser→Planner→Executor 流程。单元测试文件 `lock_manager_test.cpp` 包含 10 个测试用例，覆盖了锁类型、2PL 状态推进、锁升级互斥和死锁检测场景。

本模块由胡海洋主要负责实现。以下各节按照头文件 `lock_manager.h`、`txn.h` 和实现文件 `lock_manager.cpp`、`txn_manager.cpp` 中的实际结构展开，所有引用的代码片段均来自 `src/concurrency/` 目录。

== Lock Manager 数据结构

=== 锁模式与请求

Lock Manager 内部定义了三种锁模式 `LockMode`：

```cpp
// src/include/concurrency/lock_manager.h:28
enum class LockMode { kNone, kShared, kExclusive };
```

其中 `kNone` 表示请求尚未被授予，仅作为 `LockRequest::granted_` 的初始值。每个锁请求由 `LockRequest` 结构体表示：

```cpp
// src/include/concurrency/lock_manager.h:36
class LockRequest {
public:
    LockRequest(txn_id_t txn_id, LockMode lock_mode)
        : txn_id_(txn_id), lock_mode_(lock_mode), granted_(LockMode::kNone) {}

    txn_id_t txn_id_{0};
    LockMode lock_mode_{LockMode::kShared};
    LockMode granted_{LockMode::kNone};
};
```

注意 `lock_mode_` 与 `granted_` 是两个不同的字段。`lock_mode_` 表示事务*请求*的锁类型（共享或独占），而 `granted_` 表示*实际授予*的锁类型。请求初始为 `kNone`，在加锁函数中条件满足后才被设置为 `kShared` 或 `kExclusive`。这种分离是必须的：一个等待中的排他锁请求在队列中可能迟迟得不到满足，它的 `lock_mode_` 是 `kExclusive` 但 `granted_` 保持 `kNone`。死锁检测的等待图构建正是依赖 `granted_ == kNone` 来判断哪些请求是等待者。

=== 锁请求队列

每个行标识 `RowId` 在 `lock_table_` 中对应一个 `LockRequestQueue`。该类封装了请求列表 `req_list_`（`std::list<LockRequest>`）以及一个 `req_list_iter_map_` 哈希表，提供 O(1) 按 `txn_id` 查找请求迭代器的能力：

```cpp
// src/include/concurrency/lock_manager.h:52
class LockRequestQueue {
public:
    using ReqListType = std::list<LockRequest>;

    void EmplaceLockRequest(txn_id_t txn_id, LockMode lock_mode) {
        req_list_.emplace_front(txn_id, lock_mode);
        bool res = req_list_iter_map_.emplace(txn_id, req_list_.begin()).second;
        assert(res);
    }

    bool EraseLockRequest(txn_id_t txn_id) {
        auto iter = req_list_iter_map_.find(txn_id);
        if (iter == req_list_iter_map_.end()) return false;
        req_list_.erase(iter->second);
        req_list_iter_map_.erase(iter);
        return true;
    }

    ReqListType::iterator GetLockRequestIter(txn_id_t txn_id) {
        auto iter = req_list_iter_map_.find(txn_id);
        assert(iter != req_list_iter_map_.end());
        return iter->second;
    }

    ReqListType req_list_{};
    std::unordered_map<txn_id_t, ReqListType::iterator> req_list_iter_map_{};
    std::condition_variable cv_{};
    bool is_writing_{false};
    bool is_upgrading_{false};
    int32_t sharing_cnt_{0};
};
```

三个状态标志的语义是设计中需要特别注意的地方：

- `is_writing_`：当前是否有事务持有该记录上的排他锁。排他锁与任何其他锁都不兼容，因此它是判断共享锁能否授予的唯一障碍。
- `sharing_cnt_`：当前持有共享锁的事务数量。这个计数只包括 `granted_ == kShared` 的请求，等待中的共享锁请求不计入。排他锁等待条件 `!is_writing_ && sharing_cnt_ == 0` 同时要求无写锁且无读锁。
- `is_upgrading_`：锁升级互斥标志。同一时刻一条记录上只允许一个锁升级操作。这在 `LockUpgrade` 的入口处就被检查，而非在等待条件中，避免了多个升级请求之间的复杂互推。

请求插入采用 `emplace_front`，这意味 `req_list_` 按逆时间序排列（最新请求在队首）。由于授予顺序是 FCFS，判断是否应该授予某个等待请求时，需遍历列表从尾部向头部检查——但实际操作中我们并不需要显式遍历：授予条件只用状态标志（`is_writing_`、`sharing_cnt_`）判断，这些标志本身已经汇总了所有已授权请求的信息。

=== 锁表与顶层结构

```cpp
// src/include/concurrency/lock_manager.h:179
std::unordered_map<RowId, LockRequestQueue> lock_table_{};
std::mutex latch_{};
```

`lock_table_` 直接以 `LockRequestQueue` 为值类型（而非 `shared_ptr`），因为队列的生命周期完全由 `lock_table_` 管理：队列在首次有事务请求对应 `RowId` 的锁时由 `LockPrepare` 惰性创建，在 `Unlock` 中当队列变为空时不删除（保留空队列的代价很小，且下一次请求该行时可以重用）。全局 `latch_`（`std::mutex`）保护整个 `lock_table_`，所有 `LockShared`、`LockExclusive`、`LockUpgrade`、`Unlock` 以及死锁检测 `RunCycleDetection` 都需要先持有 `latch_`。

=== Txn 与 TxnManager

每个事务由 `Txn` 对象表示：

```cpp
// src/include/concurrency/txn.h:53
class Txn {
public:
    explicit Txn(txn_id_t txn_id, IsolationLevel iso_level = IsolationLevel::kRepeatedRead)
        : txn_id_(txn_id), iso_level_(iso_level), thread_id_(std::this_thread::get_id()) {}

    inline std::unordered_set<RowId> &GetSharedLockSet() { return shared_lock_set_; }
    inline std::unordered_set<RowId> &GetExclusiveLockSet() { return exclusive_lock_set_; }

private:
    txn_id_t txn_id_;
    IsolationLevel iso_level_;
    TxnState state_{TxnState::kGrowing};
    std::thread::id thread_id_;
    std::unordered_set<RowId> shared_lock_set_;
    std::unordered_set<RowId> exclusive_lock_set_;
};
```

`SharedLockSet` 和 `ExclusiveLockSet` 分别记录事务持有的共享锁和排他锁对应的 `RowId`。它们在三个场景下被使用：(1) `Unlock` 时从对应集合中删除 `RowId`；(2) 死锁检测 `DeleteNode` 时遍历两个集合来移除 victim 事务的出边；(3) `TxnManager::ReleaseLocks` 在 Commit/Abort 时遍历两个集合并调用 `Unlock` 批量释放所有锁。

`TxnManager` 负责事务的生命周期管理：

```cpp
// src/concurrency/txn_manager.cpp:7
Txn *TxnManager::Begin(Txn *txn, IsolationLevel isolationLevel) {
    if (nullptr == txn) txn = new Txn(next_txn_id_++, isolationLevel);
    std::unique_lock<std::shared_mutex> lock(rw_latch_);
    txn_map_[txn->GetTxnId()] = txn;
    return txn;
}

void TxnManager::Commit(Txn *txn) {
    txn->SetState(TxnState::kCommitted);
    ReleaseLocks(txn);
}

void TxnManager::Abort(Txn *txn) {
    txn->SetState(TxnState::kAborted);
    ReleaseLocks(txn);
}
```

`ReleaseLocks` 先收集两个锁集合的所有 `RowId`（合并到一个 `unordered_set`），然后对每个 `RowId` 调用 `lock_mgr_->Unlock`。使用合并的临时集合是必要的：`Unlock` 内部会修改 `Txn` 的锁集合，如果在遍历过程中直接操作可能导致迭代器失效。

== 两阶段锁与锁操作

=== 2PL 状态机

Txn 的状态按如下状态机流转：

```
GROWING ──→ SHRINKING ──→ COMMITTED
   │                         │
   └────────→ ABORTED ←──────┘
```

- `kGrowing`：事务可以获取新锁（`LockShared`、`LockExclusive`、`LockUpgrade`），但不可释放任何锁（2PL 协议在 Growing 阶段禁止释放）。
- `kShrinking`：事务已释放了至少一把锁（具体触发条件由隔离级别决定），此后不能获取新锁。
- `kCommitted` / `kAborted`：终态，不再允许任何锁操作。

Strict 2PL 的体现是：排他锁在 `Commit` 或 `Abort` 时才释放（由 `TxnManager::ReleaseLocks` 批量完成），而不是在 `Unlock` 中释放。实际代码中 `Unlock` 可能释放共享锁也可能释放排他锁，但隔离级别的 2PL 行为控制着"何时允许进入 Shrinking 阶段"。

=== LockPrepare 与 CheckAbort

```cpp
// src/concurrency/lock_manager.cpp:159
void LockManager::LockPrepare(Txn *txn, const RowId &rid) {
    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }
    if (lock_table_.find(rid) == lock_table_.end()) {
        lock_table_.emplace(std::piecewise_construct,
                            std::forward_as_tuple(rid), std::forward_as_tuple());
    }
}
```

`LockPrepare` 是所有锁获取操作（`LockShared`、`LockExclusive`）的前置公共逻辑。两步检查：(1) 如果事务已经进入 Shrinking 阶段，说明它已经释放过锁，根据 2PL 不得再获取新锁，直接置为 `kAborted` 并抛出 `TxnAbortException`；(2) 为当前 `RowId` 惰性创建 `LockRequestQueue`——使用 `emplace` 的 `piecewise_construct` 形式避免不必要的拷贝。

`LockUpgrade` 不做 `LockPrepare`（它内部有自己的 `kShrinking` 检查），因为升级针对已经持有的锁，不需要再创建队列。

```cpp
// src/concurrency/lock_manager.cpp:174
void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
    if (txn->GetState() == TxnState::kAborted) {
        req_queue.EraseLockRequest(txn->GetTxnId());
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
    }
}
```

`CheckAbort` 在 `cv_.wait` 返回后立即调用，检查事务是否在等待期间被死锁检测异步标记为 `kAborted`。关键点：即使当前等待条件（如 `!is_writing_`）已经满足，如果事务本身已是 Aborted 状态，它也不能继续获取锁。`EraseLockRequest` 从队列中移除该事务的等待请求，避免留下孤儿请求干扰后续事务。

=== LockShared

```cpp
// src/concurrency/lock_manager.cpp:19
bool LockManager::LockShared(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    if (txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockSharedOnReadUncommitted);
    }

    LockPrepare(txn, rid);

    LockRequestQueue &req_queue = lock_table_[rid];
    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);

    if (req_queue.is_writing_) {
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted || !req_queue.is_writing_;
        });
    }

    CheckAbort(txn, req_queue);

    txn->GetSharedLockSet().emplace(rid);
    req_queue.sharing_cnt_++;
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kShared;
    return true;
}
```

执行流程：
1. 隔离级别检查：`READ_UNCOMMITTED` 下不允许获取共享锁（该隔离级别要求读取脏数据，加锁反而不正确）。直接 Abort 事务。
2. `LockPrepare` 惰性创建队列。
3. 在当前 `RowId` 的队列中插入共享锁请求。
4. 冲突判断：共享锁只与排他锁冲突。因此等待条件为 `req_queue.is_writing_ == false`（或事务已 Abort）。如果当前有排他锁持有者，线程在 `cv_` 上阻塞。
5. 授予锁：更新 `SharedLockSet`、递增 `sharing_cnt_`、设置 `granted_ = kShared`。

注意第 3 步的 `EmplaceLockRequest` 发生在冲突检查*之前*。这意味着即使事务需要等待，它的请求也已经在队列中了。这对死锁检测很重要：等待图构建依赖所有等待请求都已在 `req_list_` 中注册。

=== LockExclusive

```cpp
// src/concurrency/lock_manager.cpp:53
bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    LockPrepare(txn, rid);

    LockRequestQueue &req_queue = lock_table_[rid];
    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);

    if (req_queue.is_writing_ || req_queue.sharing_cnt_ > 0) {
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted ||
                   (!req_queue.is_writing_ && req_queue.sharing_cnt_ == 0);
        });
    }

    CheckAbort(txn, req_queue);

    txn->GetExclusiveLockSet().emplace(rid);
    req_queue.is_writing_ = true;
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kExclusive;
    return true;
}
```

排他锁的冲突条件比共享锁严格：必须 `!is_writing_ && sharing_cnt_ == 0`，即当前记录上既无写锁也无任何读锁。授予后设置 `is_writing_ = true`，写入 `ExclusiveLockSet`。

`LockExclusive` 没有额外的隔离级别检查：`READ_UNCOMMITTED` 下写锁仍然需要（否则并发写会破坏数据完整性）。

=== LockUpgrade

```cpp
// src/concurrency/lock_manager.cpp:80
bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }

    LockRequestQueue &req_queue = lock_table_[rid];

    if (req_queue.is_upgrading_) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
    }

    req_queue.GetLockRequestIter(txn->GetTxnId())->lock_mode_ = LockMode::kExclusive;

    if (req_queue.is_writing_ || req_queue.sharing_cnt_ > 1) {
        req_queue.is_upgrading_ = true;
        req_queue.cv_.wait(lock, [&req_queue, txn]() {
            return txn->GetState() == TxnState::kAborted ||
                   (!req_queue.is_writing_ && req_queue.sharing_cnt_ == 1);
        });
        req_queue.is_upgrading_ = false;
    }

    CheckAbort(txn, req_queue);

    req_queue.sharing_cnt_--;
    req_queue.is_writing_ = true;
    txn->GetSharedLockSet().erase(rid);
    txn->GetExclusiveLockSet().emplace(rid);
    req_queue.GetLockRequestIter(txn->GetTxnId())->granted_ = LockMode::kExclusive;
    return true;
}
```

锁升级的执行流程与普通加锁有几个关键区别：

1. *不调用 `LockPrepare`*：升级的前置条件是事务已经持有共享锁，队列必然存在。`kShrinking` 的检查手动内联。
2. *`is_upgrading_` 互斥*：第 91-94 行立即拒绝第二个升级请求。注意不是等待而是直接 Abort。这样设计的理由是：如果两个事务同时试图升级，它们的等待条件（`sharing_cnt_ == 1`）互相矛盾，必然形成死锁。与其增加复杂的等待逻辑，不如直接拒绝并让上层重试。
3. *请求类型就地修改*：不插入新请求，而是直接修改已有请求的 `lock_mode_ = kExclusive`。这保持了事务在队列中的原有位置。
4. *升级后状态迁移*：`sharing_cnt_--`（减少一个共享锁计数），`is_writing_ = true`（标记为写锁持有），从 `SharedLockSet` 迁移到 `ExclusiveLockSet`。

锁升级等待条件为 `sharing_cnt_ == 1`，即自己是唯一共享锁持有者。因为排他锁与共享锁不兼容，必须等所有其他读者释放后才能升级。升级完成后 `is_upgrading_ = false` 恢复为可接受新的升级请求。

=== Unlock 与 2PL 状态推进

```cpp
// src/concurrency/lock_manager.cpp:123
bool LockManager::Unlock(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);

    bool was_shared = (txn->GetSharedLockSet().erase(rid) > 0);
    bool was_exclusive = (txn->GetExclusiveLockSet().erase(rid) > 0);

    if (txn->GetState() == TxnState::kGrowing &&
        (txn->GetIsolationLevel() == IsolationLevel::kRepeatedRead || was_exclusive)) {
        txn->SetState(TxnState::kShrinking);
    }

    auto table_iter = lock_table_.find(rid);
    if (table_iter == lock_table_.end()) return false;

    LockRequestQueue &req_queue = table_iter->second;
    req_queue.EraseLockRequest(txn->GetTxnId());

    if (was_exclusive) {
        req_queue.is_writing_ = false;
    } else if (was_shared && req_queue.sharing_cnt_ > 0) {
        req_queue.sharing_cnt_--;
    }

    req_queue.cv_.notify_all();
    return true;
}
```

2PL 状态推进的逻辑在第 133-136 行：Growing 阶段的事务在*首次释放锁*时进入 Shrinking，但具体触发条件取决于隔离级别。`REPEATABLE_READ` 下释放*任何*锁（共享或排他）都进入 Shrinking；`READ_COMMITTED` 下只有释放排他锁才进入 Shrinking（允许在 Growing 期间释放共享锁并随后重新获取，这是 Read Committed 语义所要求的：每次读取需要新的快照）。`READ_UNCOMMITTED` 下事务不会获取共享锁（见 `LockShared` 的隔离级别检查），因此只有排他锁释放才可能触发 Shrinking，行为等价于 `READ_COMMITTED`。

释放锁后调用 `cv_.notify_all()` 唤醒所有在 `LockShared`、`LockExclusive` 或 `LockUpgrade` 中等待的线程。被唤醒的线程重新检查各自的等待条件，条件满足的获取锁，不满足的继续等待或因 Abort 而抛出异常。

== 死锁检测

=== 等待图与 DFS 环检测

```cpp
// src/include/concurrency/lock_manager.h:183
std::unordered_map<txn_id_t, std::set<txn_id_t>> waits_for_{};
std::unordered_set<txn_id_t> visited_set_{};
std::stack<txn_id_t> visited_path_{};
txn_id_t revisited_node_{INVALID_TXN_ID};
```

等待图 `waits_for_` 使用邻接表表示：key 是等待者事务 id，value 是一个 `std::set<txn_id_t>`（被等待的事务集合）。选择 `std::set` 而非 `std::vector` 是因为 DFS 遍历时需要按升序遍历邻居，以保证环检测结果的确定性。

```cpp
// src/concurrency/lock_manager.cpp:202
bool LockManager::DFS(txn_id_t txn_id) {
    visited_set_.insert(txn_id);
    visited_path_.push(txn_id);

    auto iter = waits_for_.find(txn_id);
    if (iter != waits_for_.end()) {
        for (const txn_id_t next : iter->second) {
            if (visited_set_.find(next) != visited_set_.end()) {
                revisited_node_ = next;
                return true;
            }
            if (DFS(next)) return true;
        }
    }

    visited_path_.pop();
    visited_set_.erase(txn_id);
    return false;
}
```

DFS 实现使用两个结构协同工作：`visited_set_` 标记当前递归栈中访问过的所有节点（用于快速判断 "next 是否在路径上"），`visited_path_` 栈记录从搜索起点到当前节点的完整路径。当 `visited_set_` 中发现重复节点时，`revisited_node_` 被设置为该节点（即环中"回边"指向的节点），DFS 返回 `true`。

DFS 返回后的回溯（`pop` + `erase`）恢复状态到进入该节点之前，保证下一次从不同起点开始的搜索能重新使用这些状态。

=== HasCycle：提取环中最年轻事务

```cpp
// src/concurrency/lock_manager.cpp:229
bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
    visited_set_.clear();
    while (!visited_path_.empty()) visited_path_.pop();
    revisited_node_ = INVALID_TXN_ID;

    std::set<txn_id_t> nodes;
    for (const auto &entry : waits_for_) {
        nodes.insert(entry.first);
    }

    for (const txn_id_t start : nodes) {
        if (DFS(start)) {
            newest_tid_in_cycle = revisited_node_;
            while (!visited_path_.empty()) {
                txn_id_t node = visited_path_.top();
                visited_path_.pop();
                newest_tid_in_cycle = std::max(newest_tid_in_cycle, node);
                if (node == revisited_node_) break;
            }
            return true;
        }
    }
    return false;
}
```

环检测从最小 `txn_id` 开始搜索（`std::set` 有序遍历）。发现环后，`visited_path_` 中保存的是从 DFS 起点到 `revisited_node_` 的路径，而环的另一半就是从 `revisited_node_` 回到路径上某节点的回边。因此环由路径上 `revisited_node_` 及其后缀组成。`HasCycle` 逐 pop 路径并取 `std::max` 来找出环中 id 最大的事务。

选择 *id 最大* 的事务作为 victim 是常见的 deadlock victim selection 策略，基于"年轻事务（更大的 id 意味着更晚创建）的工作量通常较少，回滚代价更低"的假设。注意这里并没有用时间戳或实际工作量衡量，`txn_id` 作为单调递增的数值是一个足够好的代理。

=== DeleteNode

```cpp
// src/concurrency/lock_manager.cpp:261
void LockManager::DeleteNode(txn_id_t txn_id) {
    waits_for_.erase(txn_id);

    auto *txn = txn_mgr_->GetTransaction(txn_id);

    for (const auto &row_id : txn->GetSharedLockSet()) {
        for (const auto &lock_req : lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }

    for (const auto &row_id : txn->GetExclusiveLockSet()) {
        for (const auto &lock_req : lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }
}
```

`DeleteNode` 做了两件事：(1) 从等待图中删除 victim 的所有出边（`waits_for_.erase(txn_id)`）；(2) 从等待图中删除所有指向 victim 的入边：遍历 victim 持有的所有锁（共享和排他），对每个锁对应的 `LockRequestQueue`，找到所有在等待的请求（`granted_ == kNone`），移除它们指向 victim 的边。

=== RunCycleDetection：后台检测循环

```cpp
// src/concurrency/lock_manager.cpp:286
void LockManager::RunCycleDetection() {
    while (enable_cycle_detection_) {
        std::this_thread::sleep_for(cycle_detection_interval_);
        {
            std::unique_lock<std::mutex> lock(latch_);

            waits_for_.clear();
            for (const auto &entry : lock_table_) {
                const LockRequestQueue &req_queue = entry.second;
                std::vector<txn_id_t> granted;
                std::vector<txn_id_t> waiting;
                for (const auto &req : req_queue.req_list_) {
                    Txn *t = txn_mgr_->GetTransaction(req.txn_id_);
                    if (t == nullptr || t->GetState() == TxnState::kAborted) continue;
                    if (req.granted_ == LockMode::kNone)
                        waiting.push_back(req.txn_id_);
                    else
                        granted.push_back(req.txn_id_);
                }
                for (const txn_id_t w : waiting) {
                    for (const txn_id_t g : granted) {
                        if (w != g) AddEdge(w, g);
                    }
                }
            }

            txn_id_t youngest = INVALID_TXN_ID;
            while (HasCycle(youngest)) {
                Txn *victim = txn_mgr_->GetTransaction(youngest);
                if (victim != nullptr) victim->SetState(TxnState::kAborted);
                DeleteNode(youngest);
            }

            for (auto &entry : lock_table_) {
                entry.second.cv_.notify_all();
            }

            waits_for_.clear();
        }
    }
}
```

关键的实现细节：

1. *每次检测重建完整等待图*：`waits_for_.clear()` 后全部重新扫描 `lock_table_`。O(|queue|×|request|) 的代价在死锁检测间隔（默认 100ms）下是可控的。相比增量维护等待图（每次加锁/释放时更新边），全量重建避免了边维护代码中的 bug，代码更简单可靠。

2. *过滤 Aborted 事务*：第 301-303 行跳过已 Aborted 的事务，不将其加入等待图。这是因为 Aborted 事务即将释放所有锁，不应该再参与死锁检测。

3. *循环击破*：while 循环持续检测并击破环——每轮找到最年轻 victim，标记 Aborted，调用 `DeleteNode`。循环直到 `HasCycle` 返回 false。这样处理了"环中环"的情况：击破一个环后可能还有另一个环残留在图中。

4. *唤醒所有等待者*：`cv_.notify_all()` 遍历 `lock_table_` 中每个队列，确保被标记为 Aborted 的 victim 线程能被唤醒。这些线程在 `CheckAbort` 中检测到 `kAborted` 状态后抛出 `TxnAbortException` 退出等待。

`cycle_detection_interval_` 默认值为 100 毫秒，由调用方通过 `EnableCycleDetection` 设置。测试中通常使用 500 毫秒以便观察时序。

== 测试覆盖

`lock_manager_test.cpp` 包含 10 个完整测试：

- *`SLockInReadUncommittedTest`*：验证 `READ_UNCOMMITTED` 下获取共享锁被拒绝，事务状态变为 `kAborted`，`shared_lock_set_` 保持为空。
- *`TwoPhaseLockingTest`*：验证标准 2PL 流程——Growing 阶段可加 S 锁和 X 锁，释放锁后进入 Shrinking，在 Shrinking 阶段再加锁触发 `kLockOnShrinking` 异常。
- *`UpgradeLockInShrinkingPhase`*：验证在 Shrinking 阶段调用 `LockUpgrade` 被拒绝，事务 Abort。
- *`UpgradeConflictTest`*：两个事务都持有共享锁，t0 先发起升级（等待），t1 随后也试图升级，后者因 `is_upgrading_` 互斥收到 `kUpgradeConflict` 并 Abort，t1 的释放使得 `sharing_cnt_` 降到 1，t0 成功升级。
- *`UpgradeTest`*：基本的锁升级流程——S → Upgrade → X，验证锁集合从 Shared 迁移到 Exclusive，2PL 状态保持 Growing 直至 Unlock。
- *`UpgradeAfterAbortTest`*：t0 升级等待中，外部线程通过 `txn_mgr_->Abort(t0)` 中止事务，t0 被唤醒后捕获 `kDeadlock` 异常。
- *`BasicCycleTest1 + 2`*：直接通过 `AddEdge` / `RemoveEdge` 构造等待图，验证 `HasCycle` 找到最大 id 的环成员，以及排除边后环的消失。
- *`DeadlockDetectionTest1`*：经典的 AB-BA 死锁——t0 持有 r0 等 r1，t1 持有 r1 等 r0，死锁检测 500ms 后 t1 被标记 Aborted（t1 的 id 更大），t0 获得 r1 提交。
- *`DeadlockDetectionTest2`*：4 个事务的复杂依赖——t0 等 r1、t1 等 r3、t2 等 r0、t3 等 r0，结合已有共享锁形成交错等待，死锁检测找到环并击破。

== 思考题

=== 问题一：Lock Manager 不独立时，并发查询期间的隔离级别与事务边界控制

本实验中 Lock Manager 与 Executor 通过 `Txn` 对象解耦，测试中直接调用 `LockShared` / `LockExclusive` 等手段验证锁语义。如果不独立，正确接入 Executor 和 ExecuteEngine 需要完成以下工作。

*ExecuteEngine 改造*：当前 `ExecuteTrxBegin`、`ExecuteCommit`、`ExecuteRollback` 均返回 `DB_FAILED`，需要分别改为调用 `TxnManager::Begin`、`Commit`、`Abort`。`ExecutePlan` 中所有 Executor 创建时传入的 `Txn*` 参数目前为 `nullptr`，需要从 `ExecuteContext` 中获取当前会话关联的事务指针。需要支持两种事务模式：

- 隐式事务（auto-commit）：每条 DML 语句自动包裹在独立事务中。`ExecutePlan` 开始时创建一个新的 `Txn`，执行完成后 Commit，捕获 `TxnAbortException` 时 Abort。
- 显式事务：用户通过 `begin transaction` 开始，随后多条 DML 共享同一个 `Txn`，最后 `commit` 或 `rollback` 结束。

*各 Executor 的加锁点*：

- `SeqScanExecutor::Next`：逐行返回记录。在将 Row 返回给上层之前调用 `LockShared(txn, row->GetRowId())`。如果隔离级别是 `READ_UNCOMMITTED`，跳过加锁（`LockShared` 内部会直接 Abort 事务，因此 `SeqScanExecutor` 需要在调用前检查隔离级别并跳过）。
- `IndexScanExecutor`：通过索引获取 `RowId` 后，先 `LockShared` 再回表读取完整记录。顺序不能颠倒：先回表再加锁会导致读到未提交的数据，即使后续发现锁冲突也来不及纠正。
- `InsertExecutor`：先 `InsertTuple` 写入表页获得 `RowId`，然后在返回的 `RowId` 上 `LockExclusive`。如果加锁失败（异常），需要 `ApplyDelete` 回滚刚插入的记录。
- `DeleteExecutor` 和 `UpdateExecutor`：在从子执行器（通常是 `SeqScanExecutor`）获取记录后，操作前对记录的 `RowId` 调用 `LockExclusive`。注意 `UpdateExecutor` 的 `UpdateTuple` 可能在原页空间不足时走 `ApplyDelete + InsertTuple` 路径，新记录的 `RowId` 与旧记录不同，需在两个 `RowId` 上分别持锁。

*异常处理*：所有 Executor 的 `Init` 和 `Next` 都需要捕获 `TxnAbortException`。该异常应在 Executor 层停止迭代（`Next` 返回 `false`），同时设置 Executor 的内部 abort 标志。`ExecuteEngine::ExecutePlan` 在计划执行完成后检查事务状态：若为 `kAborted` 则调用 `txn_mgr_->Abort`；若正常结束则调用 `txn_mgr_->Commit`。

=== 问题二：考虑模块 3 中 B+ 树并发修改的完整设计

模块 3（Index Manager）中的 B+ 树实现目前是单线程的，所有页操作没有考虑并发安全。当 Lock Manager 不再独立且执行器并发运行后，多个事务可能同时通过索引访问数据，B+ 树的结构修改（插入导致节点分裂、删除导致节点合并）与并发读取之间必须协调。完整的并发控制需要引入页面级 latch，并与行级锁配合。

*页级 Latch 与 Crabbing 协议*：在 `BPlusTreePage` 基类中增加 `RLatch()` / `RUnlatch()` / `WLatch()` / `WUnlatch()` 四个方法，使用 `std::shared_mutex` 实现读写锁语义（多个读者可并发，写者独占）。

Crabbing 协议的核心思想是：*在向下的查找路径中，当前持有子页的 latch 后才释放父页的 latch，条件是在确定子页的修改不会向上传播时即可释放祖先*。

查找（只读）路径：
```
根: RLatch → 确定子页 → 子页: RLatch → 释放根: RUnlatch → ... → 叶子: RLatch
```
读操作天然安全，一步到位即可释放祖先。

插入路径（可能触发分裂）：
```
根: WLatch → 子页: WLatch。若子页安全（size < max_size - 1，插入不会溢出），释放根: WUnlatch。
否则保留根上的 latch，继续向下。到达叶子时，如果叶子分裂，将新键插入父节点。
因为父节点仍持有写 latch，分裂向上传播不受并发干扰。
```

删除路径（可能触发合并/重分配）：
```
安全条件为 size > min_size（删除后不会低于最小占用率）。
子页安全时释放祖先的 latch，不安全时保留。
```

*安全条件的形式化定义*：
- 插入安全：`GetSize() < GetMaxSize() - 1`（插入一条记录不会溢出）
- 删除安全：`GetSize() > GetMinSize()`（删除一条记录不会触发 underflow）

*AdjustRoot 的全局保护*：修改 `root_page_id_` 时（创建新根或降低树高），需要 `tree_mutex_` 级别的全局写锁。因为根页号被多个线程共享读取（每次查找都从根开始），直接在原对象上原子修改可能被其他线程读到中间状态。

*叶子链表遍历*：`IndexIterator` 沿叶子页的 `next_page_id` 链表前进时，需要保持对当前页的读 latch，获取下一页后先 RLatch 下一页再 RUnlatch 当前页。这保证了在跨页边界时不会因并发分裂而读到不一致的链表。

*页面 latch 与行级锁的获取顺序*：为避免跨层死锁，统一约定*先获取行级锁再获取页面 latch*。如果顺序颠倒（先持有页 latch 再等行锁），事务 A 持有页 latch 等待事务 B 的行锁，事务 B 可能正在等待事务 A 的行锁或同一页的 latch，形成循环等待。先获取行级锁的方案中，行锁由 Deadlock Detector 处理（超时或等待图），页 latch 的持有时间很短（仅用于对页内数据的物理读写保护），不会在持有页 latch 期间阻塞在另一个行锁上。

*Executor 与 B+ 树索引的交互*：`IndexScanExecutor` 的流程变为：
1. `index_->ScanKey(key, &rids)` 获取候选 `RowId` 列表（在 B+ 树内部已完成查找路径的 latch 获取/释放）
2. 对每个候选 `RowId`：先 `LockShared(txn, rid)` 获取行级读锁，再回表读记录
3. 用谓词过滤记录

插入时，`InsertExecutor` 先获取表上的行级排他锁，写入 TableHeap，再遍历索引列表，对每个索引调用 `index_->InsertEntry(key, row_id, txn)`。索引内部会走 Crabbing 协议获取需要的页 latch，写完键后释放所有页 latch。如果某个索引写入失败（比如唯一键冲突），需要回滚已写入的其他索引。此时事务仍持有该行的排他锁，Undo 在持锁下执行，保证其他事务看不到部分写入的状态。

*唯一约束检查与锁的交互*：唯一索引的插入必须在排他锁持有下完成。流程是：(1) `LockExclusive(txn, rid)` 获取行锁；(2) B+ 树 `InsertEntry` 内部查找键是否存在；(3) 不存在则插入键和 RowId。步骤 (2) 和 (3) 需要在一个页面写 latch 的保护下完成（因为查找和插入访问同一个叶子页），否则可能出现"检测不存在 → 另一事务插入相同键 → 本事务也插入"的幻读冲突。当前 B+ 树的 `Insert` 函数在 `InsertIntoLeaf` 内部已有页面 latch 保护查找和插入的原子性。

=== 问题三：索引与表数据在并发下的一致性保证

并发执行时，表和索引在多条 DML 的穿插下需要保持一致性。这要求操作顺序在并发语义上的正确性。

*Insert*：先写表再写索引。TableHeap 插入带锁保护，获取 `RowId` 后在此 `RowId` 上加排他锁。然后再向所有索引回填。如果索引写入过程中抛出异常（如 `TxnAbortException`），需要在异常处理中 `MarkDelete` 表记录并释放锁。先写表再写索引的顺序保证：如果崩溃发生在索引写入完成前，恢复时可以通过扫描表记录重建索引。反之（先写索引再写表），恢复时索引指向不存在的记录，更难修复。

*Delete*：先删索引项再 MarkDelete 表记录。先删索引可以避免其他事务通过索引找到一条即将被删除的记录。索引删除由 `index_->RemoveEntry` 完成，表删除通过 `TableHeap::ApplyDelete` 完成。两者都在行级排他锁持有下执行。

*Update*：分为两种情况。如果新记录在原页放得下：`TableHeap::UpdateTuple` in-place 更新，然后 `index_->RemoveEntry(旧key)` + `index_->InsertEntry(新key)`。如果放不下：`TableHeap::ApplyDelete(旧RowId)` → `TableHeap::InsertTuple(新Row)` 返回新 `RowId`。此时旧 `RowId` 和新 `RowId` 上都要持有排他锁，索引更新也是先删旧键再插新键。两个 RowId 上的锁通过 `LockExclusive` 分别获取，注意获取顺序要一致（比如先小 page_id 后大 page_id）避免 AB-BA 死锁。

*唯一约束检查与幽灵冲突*：`InsertExecutor` 在写入表数据和索引数据之间，如果有其他事务并发插入相同的唯一键，可能产生幽灵冲突。解决方案是：唯一索引的键检查（是否存在相同键）必须在行级排他锁的持有下完成。当前 `IndexInfo` 中的 `GetIndex` 返回的 `BPlusTreeIndex` 在 `InsertEntry` 时内部调用 B+ 树的 `Insert`，该函数内部先查找目标叶子页再插入，查找和插入处于同一个页面写 latch 下，因此对于本事务是原子的。但如果有其他事务在同一个键上并发插入，当前 Lock Manager 的行锁粒度是 `RowId` 级别，无法阻止"同键不同行"的并发插入。解决这一问题需要 next-key locking 或间隙锁（gap lock），本实验未要求实现。

= 第九章 系统测试与自设计测试

== 测试原则

本项目测试分为单元测试和端到端测试两类。单元测试用于验证单个模块或相邻模块组合的正确性；端到端测试通过 MiniSQL 主程序执行 SQL 脚本，验证 Parser、Executor、Catalog、Record、Index 和存储层能够联合工作。

测试结果以截图形式放入报告。每个截图位置会标明对应命令，截图中应包含运行命令、测试名称和测试完成状态。

== 存储与缓冲层测试

Disk Manager 测试关注位图页分配、空闲页判断、跨页读写和释放后再分配。Buffer Pool Manager 测试关注页面换入换出、dirty page 刷盘、pin count 维护和替换策略。

// TODO: 截图位置 — Disk Manager、LRU Replacer、Buffer Pool Manager 测试结果
// 保存为: 总体设计报告/images/9_1_disk_buffer_test.png
运行命令:
```bash
cd build && ./test/minisql_test --gtest_filter="DiskManagerTest.*:LRUReplacerTest.*:BufferPoolManagerTest.*"
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Disk / Buffer 测试成功通过:
  #figure(image("images/9_1_disk_buffer_test.png", width: 80%), caption: [Disk / Buffer 测试结果])
]

Clock Replacer 的自设计测试覆盖空替换器、重复 Unpin、Pin 后不可淘汰、全员 reference bit 为 1 时的第二次机会，以及连续淘汰不重复等场景。

// TODO: 截图位置 — Clock Replacer 测试结果
// 保存为: 总体设计报告/images/9_2_clock_test.png
运行命令:
```bash
cd build && ./test/minisql_test --gtest_filter="CLOCKReplacerTest.*"
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Clock Replacer 测试成功通过:
  #figure(image("images/9_2_clock_test.png", width: 80%), caption: [Clock Replacer 测试结果])
]

== Record Manager 测试

Record Manager 测试覆盖 Field、Row、Column、Schema 的序列化和反序列化，以及 TableHeap 中大量记录插入、遍历、删除和更新流程。

// TODO: 截图位置 — TupleTest + TableHeapTest 测试结果
// 保存为: 总体设计报告/images/9_3_record_test.png
运行命令:
```bash
cd build && ./test/minisql_test --gtest_filter="TupleTest.*:TableHeapTest.*"
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Record / TableHeap 测试成功通过:
  #figure(image("images/9_3_record_test.png", width: 80%), caption: [Record / TableHeap 测试结果])
]

== Index 与 Catalog 测试

Index Manager 测试覆盖 B+ 树插入、查找、删除、迭代器顺序扫描和重复键处理。Catalog Manager 测试覆盖表元信息和索引元信息序列化、创建、删除和重启加载。

// TODO: 截图位置 — BPlusTree + Catalog 测试结果
// 保存为: 总体设计报告/images/9_4_index_catalog_test.png
运行命令:
```bash
cd build && ./test/minisql_test --gtest_filter="BPlusTreeTests.*:CatalogTest.*:PageTests.IndexRootsPageTest"
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Index / Catalog 测试成功通过:
  #figure(image("images/9_4_index_catalog_test.png", width: 80%), caption: [Index / Catalog 测试结果])
]

本组还补充了 Index 与 Catalog 的组合测试，重点验证已有数据建索引时的回填、索引范围扫描、重复键拒绝、Catalog 重启加载和 DropIndex / DropTable 后的持久化状态。

// TODO: 截图位置 — custom_index_catalog_test 测试结果
// 保存为: 总体设计报告/images/9_5_custom_index.png
运行命令:
```bash
cd build && cmake --build . --target custom_index_catalog_test && ./test/custom_index_catalog_test
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Index + Catalog 组合测试成功通过:
  #figure(image("images/9_5_custom_index.png", width: 80%), caption: [Index + Catalog 组合测试])
]

== Executor 与端到端测试

Executor 测试覆盖 SeqScan、IndexScan、Insert、Delete、Update 等执行器。端到端测试通过 `execfile` 执行 SQL 脚本，验证从 SQL 文本到最终数据结果的完整路径。

// TODO: 截图位置 — ExecutorTest + execfile 端到端测试结果
// 保存为: 总体设计报告/images/9_6_executor_test.png (单元测试) + images/9_7_acceptance.png (端到端)
方式一 (单元测试):
```bash
cd build && ./test/minisql_test --gtest_filter="ExecutorTest.*"
```
方式二 (端到端):
```bash
cd build && ./bin/main
execfile test_acceptance.sql;
quit;
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Executor 与验收 SQL 测试成功通过:
  #figure(image("images/9_6_executor_test.png", width: 80%), caption: [Executor 测试结果])
]

== Recovery 与 Lock Manager 测试

Recovery 测试通过构造检查点和多事务日志，验证 Redo / Undo 后的数据状态符合预期。Lock Manager 测试覆盖共享锁、独占锁、锁升级、解锁、事务状态变化和死锁检测。

// TODO: 截图位置 — RecoveryManagerTest + LockManagerTest 测试结果
// 保存为: 总体设计报告/images/9_7_recovery_lock_test.png
运行命令:
```bash
cd build && ./test/recovery_manager_test && ./test/lock_manager_test
```
#block(
  fill: luma(240), inset: 1em, radius: 0.3em, width: 100%,
)[
  *测试结果* — Recovery / Lock Manager 测试成功通过:
  #figure(image("images/9_7_recovery_lock_test.png", width: 80%), caption: [Recovery / Lock Manager 测试结果])
]

= 第十章 总结

== 分工与协作

本组的实现和文档工作按模块划分主要负责人，但每个模块都经过组内讨论和交叉联调。程文博主要负责第 1、2 部分，即 Disk and Buffer Pool Manager 与 Record Manager；彭逸涵主要负责第 3、4 部分，即 Index Manager 与 Catalog Manager；胡海洋主要负责第 5、6 部分，即 Executor Engine 与 Recovery Manager。第 7 部分 Lock Manager 由胡海洋主要负责，程文博和彭逸涵参与并发控制设计讨论、测试复核和报告整理。

在系统联调阶段，三人共同检查了 Catalog 与 Index、Executor 与 Record、Buffer Pool 与各上层模块之间的接口一致性，并补充运行单元测试、自设计测试和端到端 SQL 验收脚本。总体设计报告由三人共同整理，各成员重点校对自己主要负责模块的实现描述和跨模块调用关系。

== 完成情况

本项目最终形成了一个能够联合运行的 MiniSQL 系统。底层通过 Disk Manager 和 Buffer Pool Manager 提供页式存储；Record Manager 在页上组织表数据；Index Manager 提供 B+ 树索引；Catalog Manager 维护所有表和索引元信息；Executor Engine 将 SQL 语句转换为对表和索引的实际操作；Recovery Manager 和 Lock Manager 分别补充恢复和并发控制能力。

从整体设计看，系统中最关键的是模块之间的边界：上层不直接读写磁盘页，而是通过 Catalog、TableHeap 和 Index 逐层访问；底层只提供页接口，不理解 SQL 语义。这样的分层使每个模块可以单独测试，也能在端到端执行中组合起来。

当前实现仍有一些限制。Recovery Manager 采用内存级 KV 模拟，尚未接入真实磁盘页日志；Lock Manager 主要实现 RowId 粒度锁，B+ 树结构修改的完整并发控制仍需要进一步引入页面 latch 和事务回滚机制；SQL 支持范围也以实验要求为主，没有覆盖完整 SQL 标准。但在 MiniSQL 实验目标范围内，系统已经实现了主要功能，并通过单元测试和自设计测试验证了关键路径。
