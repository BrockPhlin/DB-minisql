#include "executor/execute_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <tuple>

#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "planner/planner.h"
#include "utils/utils.h"

extern "C" {
int yyparse(void);
#include "parser/minisql_lex.h"
}

ExecuteEngine::ExecuteEngine(bool auto_load) {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }
  if (!auto_load) {
    // 测试用：跳过目录扫描，避免撞未 flush 的 db 文件
    closedir(dir);
    return;
  }
  // 启动时扫描 ./databases 目录，把所有数据库加载到内存
  // 这样 main 进程重启后，已经存在的数据库（含表、索引、数据）能从磁盘恢复
  struct dirent *stdir;
  while ((stdir = readdir(dir)) != nullptr) {
    if (strcmp(stdir->d_name, ".") == 0 ||
        strcmp(stdir->d_name, "..") == 0 ||
        stdir->d_name[0] == '.')
      continue;
    dbs_[stdir->d_name] = new DBStorageEngine(stdir->d_name, false);  // false = 从磁盘恢复
  }
  closedir(dir);
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    // Create a new sequential scan executor
    case PlanType::SeqScan: {
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    }
    // Create a new index scan executor
    case PlanType::IndexScan: {
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    }
    // Create a new update executor
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
      // Create a new delete executor
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values: {
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    }
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  // Construct the executor for the abstract plan node
  auto executor = CreateExecutor(exec_ctx, plan);

  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) {
        result_set->push_back(row);
      }
    }
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) {
      result_set->clear();
    }
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) {
    return DB_FAILED;
  }
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!current_db_.empty()) context = dbs_[current_db_]->MakeExecuteContext(nullptr);
  switch (ast->type_) {
    case kNodeCreateDB:
      return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:
      return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:
      return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:
      return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:
      return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable:
      return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:
      return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes:
      return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex:
      return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:
      return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:
      return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:
      return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback:
      return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:
      return ExecuteExecfile(ast, context.get());
    case kNodeQuit:
      return ExecuteQuit(ast, context.get());
    default:
      break;
  }
  // Plan the query.
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    // Execute the query.
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    std::cout << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }
  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time)).count());
  // Return the result set as string.
  std::stringstream ss;
  ResultWriter writer(ss);

  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      // find the max width for each column
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set) {
        for (uint32_t i = 0; i < num_of_columns; i++) {
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
        }
      }
      int k = 0;
      for (const auto &column : schema->GetColumns()) {
        data_width[k] = max(data_width[k], int(column->GetName().length()));
        k++;
      }
      // Generate header for the result set.
      writer.Divider(data_width);
      k = 0;
      writer.BeginRow();
      for (const auto &column : schema->GetColumns()) {
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      }
      writer.EndRow();
      writer.Divider(data_width);

      // Transforming result set into strings.
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        }
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  std::cout << writer.stream_.rdbuf();
  // todo:: use shared_ptr for schema
  if (ast->type_ == kNodeSelect)
      delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:
      cout << "Database already exists." << endl;
      break;
    case DB_NOT_EXIST:
      cout << "Database not exists." << endl;
      break;
    case DB_TABLE_ALREADY_EXIST:
      cout << "Table already exists." << endl;
      break;
    case DB_TABLE_NOT_EXIST:
      cout << "Table not exists." << endl;
      break;
    case DB_INDEX_ALREADY_EXIST:
      cout << "Index already exists." << endl;
      break;
    case DB_INDEX_NOT_FOUND:
      cout << "Index not exists." << endl;
      break;
    case DB_COLUMN_NAME_NOT_EXIST:
      cout << "Column not exists." << endl;
      break;
    case DB_KEY_NOT_FOUND:
      cout << "Key not exists." << endl;
      break;
    case DB_QUIT:
      cout << "Bye." << endl;
      break;
    default:
      break;
  }
}

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    return DB_ALREADY_EXIST;
  }
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) {
    return DB_NOT_EXIST;
  }
  remove(("./databases/" + db_name).c_str());
  delete dbs_[db_name];
  dbs_.erase(db_name);
  if (db_name == current_db_)
    current_db_ = "";
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowDatabases" << std::endl;
#endif
  if (dbs_.empty()) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width) max_width = itr.first.length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteUseDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    current_db_ = db_name;
    cout << "Database changed" << endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowTables" << std::endl;
#endif
  if (current_db_.empty()) {
    cout << "No database selected" << endl;
    return DB_FAILED;
  }
  vector<TableInfo *> tables;
  if (dbs_[current_db_]->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    cout << "Empty set (0.00 sec)" << endl;
    return DB_FAILED;
  }
  string table_in_db("Tables_in_" + current_db_);
  uint max_width = table_in_db.length();
  for (const auto &itr : tables) {
    if (itr->GetTableName().length() > max_width) max_width = itr->GetTableName().length();
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  cout << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << endl;
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : tables) {
    cout << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << endl;
  }
  cout << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateTable" << std::endl;
#endif
  if (current_db_.empty()) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  // 解析表名: ast->child_ = kNodeIdentifier (table_name)
  std::string table_name = ast->child_->val_;

  // 解析列定义列表: ast->child_->next_ = kNodeColumnDefinitionList
  pSyntaxNode col_def_list = ast->child_->next_;
  if (col_def_list == nullptr || col_def_list->type_ != kNodeColumnDefinitionList) {
    return DB_FAILED;
  }

  std::vector<Column *> columns;
  uint32_t col_idx = 0;

  // 遍历每个列定义 (通过 next_ 链表)
  for (pSyntaxNode col_def = col_def_list->child_; col_def != nullptr; col_def = col_def->next_) {
    if (col_def->type_ != kNodeColumnDefinition) {
      continue;
    }

    // col_def->child_ = kNodeIdentifier (column name)
    pSyntaxNode col_name_node = col_def->child_;
    std::string col_name = col_name_node->val_;

    // col_name_node->next_ = kNodeColumnType (type: "int", "char", "float")
    pSyntaxNode col_type_node = col_name_node->next_;
    std::string col_type = col_type_node->val_;

    // UNIQUE constraint is stored in the val_ field of kNodeColumnDefinition
    bool is_unique = (col_def->val_ != nullptr && strcmp(col_def->val_, "unique") == 0);

    if (col_type == "int") {
      columns.push_back(new Column(col_name, kTypeInt, col_idx++, true, is_unique));
    } else if (col_type == "float") {
      columns.push_back(new Column(col_name, kTypeFloat, col_idx++, true, is_unique));
    } else if (col_type == "char") {
      // CHAR length is stored in col_type_node->child_ (kNodeNumber)
      pSyntaxNode length_node = col_type_node->child_;
      uint32_t length = 0;
      if (length_node != nullptr && length_node->type_ == kNodeNumber) {
        length = static_cast<uint32_t>(atoi(length_node->val_));
      }
      columns.push_back(new Column(col_name, kTypeChar, length, col_idx++, true, is_unique));
    } else {
      // 未知类型
      for (auto col : columns) {
        delete col;
      }
      return DB_FAILED;
    }
  }

  // 创建 Schema 并调用 CatalogManager
  Schema *schema = new Schema(columns);
  TableInfo *table_info = nullptr;
  dberr_t result =
      dbs_[current_db_]->catalog_mgr_->CreateTable(table_name, schema, nullptr, table_info);
  delete schema;

  if (result == DB_SUCCESS) {
    std::cout << "Table " << table_name << " created." << std::endl;
  }
  return result;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropTable" << std::endl;
#endif
  if (current_db_.empty()) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  // 解析表名: ast->child_ = kNodeIdentifier (table_name)
  std::string table_name = ast->child_->val_;

  dberr_t result = dbs_[current_db_]->catalog_mgr_->DropTable(table_name);

  if (result == DB_SUCCESS) {
    std::cout << "Table " << table_name << " dropped." << std::endl;
  } else if (result == DB_TABLE_NOT_EXIST) {
    std::cout << "Table " << table_name << " does not exist." << std::endl;
  }
  return result;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowIndexes" << std::endl;
#endif
  if (current_db_.empty()) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  // 获取当前数据库中所有的表
  std::vector<TableInfo *> tables;
  dbs_[current_db_]->catalog_mgr_->GetTables(tables);

  if (tables.empty()) {
    std::cout << "Empty set (0.00 sec)" << std::endl;
    return DB_SUCCESS;
  }

  // 计算列宽
  uint max_table_width = 5;     // "Table"
  uint max_index_width = 5;     // "Index"
  uint max_column_width = 7;    // "Columns"
  std::vector<std::tuple<std::string, std::string, std::string>> index_rows;

  for (const auto &table : tables) {
    std::vector<IndexInfo *> indexes;
    dbs_[current_db_]->catalog_mgr_->GetTableIndexes(table->GetTableName(), indexes);
    for (const auto &index : indexes) {
      std::string table_name = table->GetTableName();
      std::string index_name = index->GetIndexName();

      // 构建索引列名字符串
      std::string col_names;
      auto key_schema = index->GetIndexKeySchema();
      for (uint32_t i = 0; i < key_schema->GetColumnCount(); i++) {
        if (i > 0) col_names += ", ";
        col_names += key_schema->GetColumn(i)->GetName();
      }

      if (table_name.length() > max_table_width) max_table_width = table_name.length();
      if (index_name.length() > max_index_width) max_index_width = index_name.length();
      if (col_names.length() > max_column_width) max_column_width = col_names.length();

      index_rows.emplace_back(table_name, index_name, col_names);
    }
  }

  if (index_rows.empty()) {
    std::cout << "Empty set (0.00 sec)" << std::endl;
    return DB_SUCCESS;
  }

  // 输出表格
  std::cout << "+" << std::setfill('-') << std::setw(max_table_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_index_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_column_width + 2) << ""
            << "+" << std::endl;
  std::cout << "| " << std::left << std::setfill(' ') << std::setw(max_table_width) << "Table"
            << " | " << std::setw(max_index_width) << "Index"
            << " | " << std::setw(max_column_width) << "Columns"
            << " |" << std::endl;
  std::cout << "+" << std::setfill('-') << std::setw(max_table_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_index_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_column_width + 2) << ""
            << "+" << std::endl;

  for (const auto &row : index_rows) {
    std::cout << "| " << std::left << std::setfill(' ') << std::setw(max_table_width) << std::get<0>(row)
              << " | " << std::setw(max_index_width) << std::get<1>(row)
              << " | " << std::setw(max_column_width) << std::get<2>(row)
              << " |" << std::endl;
  }

  std::cout << "+" << std::setfill('-') << std::setw(max_table_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_index_width + 2) << ""
            << "+" << std::setfill('-') << std::setw(max_column_width + 2) << ""
            << "+" << std::endl;

  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateIndex" << std::endl;
#endif
  if (current_db_.empty()) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  // 解析: CREATE INDEX index_name ON table_name (col1, col2, ...)
  // ast->child_ = kNodeIdentifier (index_name)
  std::string index_name = ast->child_->val_;

  // ast->child_->next_ = kNodeIdentifier (table_name)
  pSyntaxNode table_name_node = ast->child_->next_;
  if (table_name_node == nullptr || table_name_node->type_ != kNodeIdentifier) {
    return DB_FAILED;
  }
  std::string table_name = table_name_node->val_;

  // ast->child_->next_->next_ = kNodeColumnList
  pSyntaxNode column_list = table_name_node->next_;
  if (column_list == nullptr || column_list->type_ != kNodeColumnList) {
    return DB_FAILED;
  }

  // 解析列名列表
  std::vector<std::string> index_keys;
  for (pSyntaxNode col_node = column_list->child_; col_node != nullptr; col_node = col_node->next_) {
    if (col_node->type_ == kNodeIdentifier) {
      index_keys.push_back(col_node->val_);
    }
  }

  // 索引类型 (可选): ast->child_->next_->next_->next_ = kNodeIndexType
  std::string index_type = "bptree";  // 默认使用 B+ Tree
  pSyntaxNode type_node = column_list->next_;
  if (type_node != nullptr && type_node->type_ == kNodeIndexType) {
    index_type = type_node->child_->val_;
  }

  IndexInfo *index_info = nullptr;
  dberr_t result = dbs_[current_db_]->catalog_mgr_->CreateIndex(table_name, index_name, index_keys, nullptr,
                                                                 index_info, index_type);

  if (result == DB_SUCCESS) {
    std::cout << "Index " << index_name << " created on table " << table_name << "." << std::endl;
  } else if (result == DB_TABLE_NOT_EXIST) {
    std::cout << "Table " << table_name << " does not exist." << std::endl;
  } else if (result == DB_INDEX_ALREADY_EXIST) {
    std::cout << "Index " << index_name << " already exists." << std::endl;
  }
  return result;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropIndex" << std::endl;
#endif
  if (current_db_.empty()) {
    std::cout << "No database selected" << std::endl;
    return DB_FAILED;
  }

  // DROP INDEX 语法不含表名，需要遍历所有表查找该索引
  // ast->child_ = kNodeIdentifier (index_name)
  std::string index_name = ast->child_->val_;

  std::string table_name;
  std::vector<TableInfo *> tables;
  dbs_[current_db_]->catalog_mgr_->GetTables(tables);
  for (const auto &table : tables) {
    IndexInfo *index_info = nullptr;
    if (dbs_[current_db_]->catalog_mgr_->GetIndex(table->GetTableName(), index_name, index_info) == DB_SUCCESS) {
      table_name = table->GetTableName();
      break;
    }
  }
  if (table_name.empty()) {
    std::cout << "Index " << index_name << " not found." << std::endl;
    return DB_INDEX_NOT_FOUND;
  }

  dberr_t result = dbs_[current_db_]->catalog_mgr_->DropIndex(table_name, index_name);

  if (result == DB_SUCCESS) {
    std::cout << "Index " << index_name << " dropped." << std::endl;
  } else if (result == DB_INDEX_NOT_FOUND) {
    std::cout << "Index " << index_name << " not found." << std::endl;
  }
  return result;
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxBegin" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxCommit" << std::endl;
#endif
  return DB_FAILED;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxRollback" << std::endl;
#endif
  return DB_FAILED;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteExecfile" << std::endl;
#endif
  // 解析文件名: ast->child_ = kNodeString 或 kNodeIdentifier
  if (ast->child_ == nullptr) {
    std::cout << "No file specified." << std::endl;
    return DB_FAILED;
  }
  std::string file_name = ast->child_->val_;

  // 打开文件
  std::ifstream file(file_name);
  if (!file.is_open()) {
    std::cout << "Failed to open file: " << file_name << std::endl;
    return DB_FAILED;
  }

  // 读取整个文件内容并按分号分割语句
  std::stringstream buffer;
  buffer << file.rdbuf();
  file.close();
  std::string content = buffer.str();

  // 记录 execfile 批次开始时间（验收要求：批量执行时显示总执行时间）
  auto batch_start = std::chrono::system_clock::now();
  // 统计本批次成功执行的 SQL 数量
  size_t statement_count = 0;

  // 按分号分割并逐条执行
  std::string statement;
  for (size_t i = 0; i < content.size(); i++) {
    if (content[i] == ';') {
      // 执行一条语句
      if (!statement.empty()) {
        // 去除前导空白
        size_t start = statement.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
          std::string sql = statement.substr(start) + ";";
          YY_BUFFER_STATE bp = yy_scan_string(sql.c_str());
          if (bp != nullptr) {
            yy_switch_to_buffer(bp);
            MinisqlParserInit();
            yyparse();
            if (!MinisqlParserGetError()) {
              Execute(MinisqlGetParserRootNode());
              statement_count++;
            } else {
              std::cout << "Error in SQL: " << sql << std::endl;
              std::cout << MinisqlParserGetErrorMessage() << std::endl;
            }
            MinisqlParserFinish();
            yy_delete_buffer(bp);
            yylex_destroy();
          }
        }
        statement.clear();
      }
    } else {
      statement += content[i];
    }
  }

  // 统计本批次总耗时：用 microseconds 精度更细，避免毫秒被截到 0
  auto batch_end = std::chrono::system_clock::now();
  double batch_ms = std::chrono::duration_cast<std::chrono::microseconds>(batch_end - batch_start).count() / 1000.0;
  std::cout << "Execfile [" << file_name << "] total: " << statement_count
            << " statement(s), " << std::fixed << std::setprecision(4)
            << batch_ms / 1000.0 << " sec." << std::endl;

  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteQuit" << std::endl;
#endif
  return DB_QUIT;
}
