#import "../个人设计报告34/lib.typ": *

#show: bubble.with(
  title: "MiniSQL 总体设计报告",
  subtitle: "DB-minisql",
  author: "程文博 · 彭逸涵 · 胡海洋",
  affiliation: "浙江大学 计算机科学与技术学院",
  year: "数据库系统, 2026春夏",
  date: datetime.today().display("[year] 年 [month padding:none] 月 [day padding:none] 日"),
)

#let screenshot-box(title, body) = block(
  fill: luma(245),
  stroke: 0.8pt + luma(170),
  inset: 1em,
  radius: 0.3em,
  width: 100%,
)[
  #strong(title)

  #body
]

#outline(title: "目录")
#pagebreak()

= 前言

本项目实现了一个基于 C++ 的 MiniSQL 数据库系统。系统以页式存储为底层基础，在其上实现缓冲池、记录管理、B+ 树索引、目录管理、执行引擎、恢复管理和锁管理等模块，最终支持数据库、表、索引的管理，以及 `select`、`insert`、`update`、`delete` 等基本 SQL 操作。

本报告是小组总体设计报告，重点说明整个系统的模块分层、模块间接口关系和关键流程。个人报告已经分别对各自负责模块中的代码细节、关键函数和调试过程进行了展开，因此总体报告不再逐段重复代码，而是从系统角度说明为什么采用这样的组织方式，以及各模块如何配合完成一次完整的 SQL 执行。

项目采用 CMake 构建，核心代码位于 `src` 目录，测试代码位于 `test` 目录。底层依赖 GoogleTest 完成单元测试，主程序通过命令行交互和 `execfile` 支持端到端验收。

小组分工如下：

- 程文博：Disk Manager、Buffer Pool Manager、Record Manager，以及 Clock Replacer、TablePage Hint 等扩展。
- 彭逸涵：Index Manager、Catalog Manager，以及 Index 与 Catalog 组合场景的自设计测试。
- 胡海洋：Executor Engine、Recovery Manager，以及 SQL 端到端执行流程相关功能。

此外，本组完成了第七部分 Lock Manager 的设计与实现，因此总体设计报告中单独保留第七章说明并发控制模块和思考题。

需要说明的是，MiniSQL 框架中已经提供了一部分基础设施，例如 Parser 生成的语法树结构、`Field` 类型比较、`GenericKey` / `KeyManager` 的序列化比较逻辑，以及 `TxnManager` 的事务边界控制。报告中会把这些内容作为依赖说明，不将其写成本组的主要实现。

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

== 框架与实现边界

为了避免把框架已有函数误写成本组实现，本报告按以下边界组织内容：

- Parser 相关的词法语法生成文件、AST 基本结构作为输入基础，不作为报告重点展开。
- `Field` 的类型比较、`GenericKey` 的序列化比较和 `KeyManager` 的基本逻辑作为索引模块依赖。
- `TxnManager` 已提供 `Begin`、`Commit`、`Abort` 和锁释放框架；本组实现重点是 `LockManager` 中的锁授予、等待和死锁检测。
- `ResultWriter` 等输出辅助类只作为执行层结果展示工具，不单独作为模块说明。

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

实验框架已经提供了 `GenericKey`、`KeyManager` 和比较逻辑。Index Manager 的重点不在于重新实现字段比较，而在于维护 B+ 树在插入、删除、分裂、合并、范围扫描和根页持久化过程中的结构不变量。

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

= 第七章 并发控制与 Lock Manager

== 模块定位

Lock Manager 负责管理事务在记录 RowId 上的共享锁和独占锁。它根据事务隔离级别决定是否允许加锁，并在锁冲突时阻塞事务，直到锁可用或事务被死锁检测线程中止。

`TxnManager` 中的事务创建、提交和中止逻辑由框架提供。提交或中止时，TxnManager 会释放事务持有的锁。本组实现重点在 Lock Manager 内部的锁请求队列、锁升级、2PL 状态迁移和死锁检测。

== 事务状态与隔离级别

系统支持三种隔离级别：

- `READ_UNCOMMITTED`：不需要共享锁读取，因此对共享锁请求直接中止。
- `READ_COMMITTED`：允许读锁较早释放，但写锁释放后事务进入收缩阶段。
- `REPEATABLE_READ`：遵循两阶段锁协议，事务释放任意锁后进入收缩阶段，之后不能再申请新锁。

事务状态主要包括 Growing、Shrinking、Committed 和 Aborted。Lock Manager 在加锁前检查事务状态，如果事务已经进入 Shrinking 阶段仍尝试申请新锁，则将其置为 Aborted 并抛出异常。

== 锁请求队列

每个 RowId 对应一个 `LockRequestQueue`。队列中保存该记录上的所有锁请求，并维护以下状态：

- 当前是否有事务持有独占锁。
- 当前共享锁持有者数量。
- 当前是否存在锁升级请求。
- 条件变量，用于唤醒等待中的事务。

共享锁只与已经授予的独占锁冲突。独占锁与任何已经授予的共享锁或独占锁冲突。锁升级需要从共享锁转换为独占锁，并且同一条记录上同一时刻只允许一个事务执行升级，否则会产生升级冲突。

== 加锁与释放流程

`LockShared` 在隔离级别允许的情况下插入共享锁请求。如果当前存在写锁，则通过条件变量等待。被唤醒后会检查事务是否已经被死锁检测线程中止。

`LockExclusive` 插入独占锁请求后，需要等待当前没有写锁且共享锁数量为 0。成功后将该锁加入事务的 exclusive lock set。

`LockUpgrade` 要求事务已经持有共享锁。升级时先把请求模式改为独占锁，再等待自己成为唯一共享锁持有者，同时没有其他写锁。成功后从 shared lock set 移到 exclusive lock set。

`Unlock` 会从事务的锁集合和对应 RowId 的请求队列中移除锁，并根据隔离级别更新事务状态。释放锁后调用 `notify_all`，让等待事务重新检查条件。

== 死锁检测

Lock Manager 后台周期性构建等待图。图中的边 `T1 -> T2` 表示事务 T1 正在等待事务 T2 持有的锁。构图时跳过已经中止的事务，避免无效节点影响检测结果。

环检测使用 DFS。为了保证测试结果确定，检测时总是从较小事务 id 开始遍历，邻接点也按从小到大的顺序探索。一旦发现环，选择环中事务 id 最大的事务作为 victim，将其状态置为 Aborted，并通知等待队列。等待中的事务被唤醒后会检测到自身已中止并抛出异常。

== 思考题

目前 Lock Manager 主要在 RowId 粒度上提供记录锁。如果要把它完整接入并发查询，尤其是 B+ 树并发修改，还需要在多个模块上补充设计。

首先，Executor 层需要在所有访问记录的位置显式传递事务对象。SeqScan 和 IndexScan 在读取记录前申请共享锁，Insert、Delete、Update 在修改记录前申请独占锁。提交和中止时由 TxnManager 统一释放锁，同时执行器需要根据异常正确终止当前语句。

其次，TableHeap 和 TablePage 中的读写接口需要和锁管理保持一致。当前 TablePage 已有读写 latch，但 latch 保护的是内存页结构，Lock Manager 保护的是事务之间的数据访问语义。两者不能混用：页 latch 应短时间持有，用于保护页内结构；记录锁可以跨越更长的事务阶段，用于保证隔离级别。

再次，B+ 树并发修改需要引入页面级 latch 和 crabbing 策略。查找路径可以从根向下加读 latch，并在确认子节点安全后释放父节点；插入和删除路径需要根据节点是否会分裂或合并决定是否继续持有祖先 latch。叶子页链表范围扫描还需要处理扫描过程中叶子页被分裂或合并的情况。

最后，索引和表的修改必须保持一致。Insert 需要先写表再写索引，Delete 和 Update 需要同时维护旧索引项和新索引项。如果中间失败，需要配合 Recovery Manager 的日志或事务写集合回滚。否则在并发执行时，可能出现表中存在记录但索引缺失，或索引指向已经删除记录的问题。

= 第八章 Recovery Manager 设计

== 模块定位

Recovery Manager 在本项目中采用简化的内存级设计。它不直接接入真实 Disk Manager 和 Buffer Pool，而是用 `unordered_map` 模拟数据库中的键值数据。这样做可以把恢复算法本身从页结构、刷盘策略和日志落盘细节中拆出来，便于单元测试验证 Redo 和 Undo 的正确性。

== 日志结构

日志记录 `LogRec` 保存事务恢复需要的信息。每条日志包含日志类型、LSN、同事务上一条日志的 prev_lsn、事务 id，以及数据操作所需的 key、old value 和 new value。

日志类型包括插入、删除、更新、事务开始、提交和中止。`prev_lsn` 将同一事务的日志串成反向链，Undo 时可以从事务最后一条日志沿链回溯。

日志集合使用 `std::map<lsn_t, LogRecPtr>` 保存。这样 Redo 阶段可以按 LSN 升序重放，Undo 阶段可以按 LSN 降序扫描，避免无序容器导致恢复顺序不确定。

== CheckPoint

CheckPoint 表示一次恢复开始时的持久化快照，包含检查点 LSN、检查点时刻的活跃事务表和持久化数据。RecoveryManager 的 `Init` 阶段会用 CheckPoint 恢复这些状态。

== Redo 与 Undo

Redo 阶段从检查点 LSN 之后开始，按日志顺序重放所有操作。提交日志会把事务从活跃事务表中删除；开始日志会加入活跃事务；插入、删除、更新分别按照 after image 重做。遇到 Abort 日志时，系统沿该事务的 prev_lsn 链反向撤销该事务已经产生的修改，并将该事务从活跃事务表移除。

Undo 阶段处理 Redo 后仍留在活跃事务表中的事务。这些事务在崩溃前未提交，因此需要逆序扫描日志，把它们的修改全部撤销。插入的逆操作是删除 key，删除和更新的逆操作是恢复 old value。

该实现省略了真实 ARIES 中的 CLR 和页 LSN 判断，但保留了日志驱动恢复、检查点、Redo、Undo 和事务日志链这些核心思想。

= 第九章 系统测试与自设计测试

== 测试原则

本项目测试分为单元测试和端到端测试两类。单元测试用于验证单个模块或相邻模块组合的正确性；端到端测试通过 MiniSQL 主程序执行 SQL 脚本，验证 Parser、Executor、Catalog、Record、Index 和存储层能够联合工作。

报告中不直接粘贴测试输出文本。需要证明测试通过时，保留截图位置，并在占位框中说明应放置的截图内容。截图应包含运行命令、测试名称和测试完成状态。

== 存储与缓冲层测试

Disk Manager 测试关注位图页分配、空闲页判断、跨页读写和释放后再分配。Buffer Pool Manager 测试关注页面换入换出、dirty page 刷盘、pin count 维护和替换策略。

#screenshot-box([截图占位：Disk / Buffer 测试结果], [
  此处放置 Disk Manager、LRU Replacer、Buffer Pool Manager 相关测试的终端截图。截图应包含实际执行的测试命令、测试套件名称和最终通过状态。不要在正文中手工粘贴测试输出文本。

  获取方式：在项目根目录执行以下命令，并截取命令和测试结果同屏的终端画面。

  ```bash
  cd build
  ./test/minisql_test --gtest_filter="DiskManagerTest.*:LRUReplacerTest.*:BufferPoolManagerTest.*"
  ```
])

Clock Replacer 的自设计测试覆盖空替换器、重复 Unpin、Pin 后不可淘汰、全员 reference bit 为 1 时的第二次机会，以及连续淘汰不重复等场景。

#screenshot-box([截图占位：Clock Replacer 自设计测试], [
  此处放置 Clock Replacer 自设计测试运行截图。截图应能体现所有 Clock 相关用例均已运行，并显示最终通过状态。

  获取方式：在项目根目录执行以下命令，并截取包含完整命令和最终结果的终端画面。

  ```bash
  cd build
  ./test/minisql_test --gtest_filter="CLOCKReplacerTest.*"
  ```
])

== Record Manager 测试

Record Manager 测试覆盖 Field、Row、Column、Schema 的序列化和反序列化，以及 TableHeap 中大量记录插入、遍历、删除和更新流程。

#screenshot-box([截图占位：Record / TableHeap 测试结果], [
  此处放置 TupleTest 和 TableHeapTest 的测试截图。截图应包含命令、测试套件名称和最终通过状态。

  获取方式：在项目根目录执行以下命令，并截取终端截图。

  ```bash
  cd build
  ./test/minisql_test --gtest_filter="TupleTest.*:TableHeapTest.*"
  ```
])

== Index 与 Catalog 测试

Index Manager 测试覆盖 B+ 树插入、查找、删除、迭代器顺序扫描和重复键处理。Catalog Manager 测试覆盖表元信息和索引元信息序列化、创建、删除和重启加载。

#screenshot-box([截图占位：Index / Catalog 测试结果], [
  此处放置 BPlusTreeTests、IndexIteratorTest、CatalogTest 等相关测试截图。截图应显示测试命令、测试用例集合和最终通过状态。

  获取方式：在项目根目录执行以下命令，并截取终端截图。

  ```bash
  cd build
  ./test/minisql_test --gtest_filter="BPlusTreeTests.*:CatalogTest.*:PageTests.IndexRootsPageTest"
  ```
])

本组还补充了 Index 与 Catalog 的组合测试，重点验证已有数据建索引时的回填、索引范围扫描、重复键拒绝、Catalog 重启加载和 DropIndex / DropTable 后的持久化状态。

#screenshot-box([截图占位：Index + Catalog 自设计测试], [
  此处放置 `custom_index_catalog_test` 的运行截图。截图应体现自设计测试文件被执行，并显示全部用例通过。

  获取方式：在项目根目录执行以下命令，并截取终端截图。

  ```bash
  cd build
  cmake --build . --target custom_index_catalog_test
  ./test/custom_index_catalog_test
  ```
])

== Executor 与端到端测试

Executor 测试覆盖 SeqScan、IndexScan、Insert、Delete、Update 等执行器。端到端测试通过 `execfile` 执行 SQL 脚本，验证从 SQL 文本到最终数据结果的完整路径。

#screenshot-box([截图占位：Executor 与验收 SQL 测试], [
  此处放置 ExecutorTest 运行截图，以及通过 MiniSQL 主程序执行验收 SQL 脚本的截图。截图应包含输入命令、脚本名或测试名，以及最终执行成功状态。

  获取方式一：执行 Executor 单元测试并截图。

  ```bash
  cd build
  ./test/minisql_test --gtest_filter="ExecutorTest.*"
  ```

  获取方式二：从项目根目录启动 MiniSQL 主程序，输入验收脚本命令后截图。

  ```bash
  ./build/bin/main
  ```

  进入交互界面后输入：

  ```sql
  execfile test_acceptance.sql;
  quit;
  ```
])

== Recovery 与 Lock Manager 测试

Recovery 测试通过构造检查点和多事务日志，验证 Redo / Undo 后的数据状态符合预期。Lock Manager 测试覆盖共享锁、独占锁、锁升级、解锁、事务状态变化和死锁检测。

#screenshot-box([截图占位：Recovery / Lock Manager 测试结果], [
  此处放置 RecoveryManagerTest 和 LockManagerTest 的运行截图。截图应包含实际执行命令、测试套件名称和最终通过状态。

  获取方式：在项目根目录执行以下命令，并截取终端截图。

  ```bash
  cd build
  ./test/recovery_manager_test
  ./test/lock_manager_test
  ```
])

= 第十章 总结

本项目最终形成了一个能够联合运行的 MiniSQL 系统。底层通过 Disk Manager 和 Buffer Pool Manager 提供页式存储；Record Manager 在页上组织表数据；Index Manager 提供 B+ 树索引；Catalog Manager 维护所有表和索引元信息；Executor Engine 将 SQL 语句转换为对表和索引的实际操作；Recovery Manager 和 Lock Manager 分别补充恢复和并发控制能力。

从整体设计看，系统中最关键的是模块之间的边界：上层不直接读写磁盘页，而是通过 Catalog、TableHeap 和 Index 逐层访问；底层只提供页接口，不理解 SQL 语义。这样的分层使每个模块可以单独测试，也能在端到端执行中组合起来。

当前实现仍有一些限制。Recovery Manager 采用内存级 KV 模拟，尚未接入真实磁盘页日志；Lock Manager 主要实现 RowId 粒度锁，B+ 树结构修改的完整并发控制仍需要进一步引入页面 latch 和事务回滚机制；SQL 支持范围也以实验要求为主，没有覆盖完整 SQL 标准。但在 MiniSQL 实验目标范围内，系统已经实现了主要功能，并通过单元测试和自设计测试验证了关键路径。
