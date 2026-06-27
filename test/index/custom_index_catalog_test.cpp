#include "catalog/catalog.h"
#include "common/instance.h"
#include "gtest/gtest.h"
#include "index/b_plus_tree_index.h"

#include <cstring>
#include <memory>
#include <vector>

namespace {

Row MakeIntKey(int value) {
  std::vector<Field> fields{Field(TypeId::kTypeInt, value)};
  return Row(fields);
}

Row MakeUserRow(int id, const char *name) {
  std::vector<Field> fields{Field(TypeId::kTypeInt, id), Field(TypeId::kTypeChar, const_cast<char *>(name),
                                                               static_cast<uint32_t>(strlen(name)), true)};
  return Row(fields);
}

}  // namespace

TEST(CustomIndexCatalogTest, IndexRangeOperatorsAndDuplicateKey) {
  DBStorageEngine engine("custom_index_range_test.db", true);

  std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, true)};
  TableSchema table_schema(columns);
  std::vector<uint32_t> key_map{0};
  auto *index_schema = Schema::ShallowCopySchema(&table_schema, key_map);
  BPlusTreeIndex index(101, index_schema, 16, engine.bpm_);

  for (int i = 1; i <= 20; i++) {
    Row key = MakeIntKey(i);
    ASSERT_EQ(DB_SUCCESS, index.InsertEntry(key, RowId(7, i), nullptr));
  }

  Row duplicated_key = MakeIntKey(10);
  ASSERT_EQ(DB_FAILED, index.InsertEntry(duplicated_key, RowId(7, 100), nullptr));

  std::vector<RowId> result;
  Row equal_key = MakeIntKey(10);
  ASSERT_EQ(DB_SUCCESS, index.ScanKey(equal_key, result, nullptr, "="));
  ASSERT_EQ(1, result.size());
  EXPECT_EQ(RowId(7, 10), result[0]);

  result.clear();
  Row ge_key = MakeIntKey(18);
  ASSERT_EQ(DB_SUCCESS, index.ScanKey(ge_key, result, nullptr, ">="));
  ASSERT_EQ(3, result.size());
  EXPECT_EQ(RowId(7, 18), result[0]);
  EXPECT_EQ(RowId(7, 19), result[1]);
  EXPECT_EQ(RowId(7, 20), result[2]);

  result.clear();
  Row lt_key = MakeIntKey(4);
  ASSERT_EQ(DB_SUCCESS, index.ScanKey(lt_key, result, nullptr, "<"));
  ASSERT_EQ(3, result.size());
  EXPECT_EQ(RowId(7, 1), result[0]);
  EXPECT_EQ(RowId(7, 2), result[1]);
  EXPECT_EQ(RowId(7, 3), result[2]);

  result.clear();
  Row ne_key = MakeIntKey(10);
  ASSERT_EQ(DB_SUCCESS, index.ScanKey(ne_key, result, nullptr, "<>"));
  ASSERT_EQ(19, result.size());
  for (const auto &rid : result) {
    EXPECT_NE(RowId(7, 10).Get(), rid.Get());
  }

  index.Destroy();
  delete index_schema;
}

TEST(CustomIndexCatalogTest, CatalogReloadDropAndExistingRowsIndexBackfill) {
  const std::string db_name = "custom_catalog_lifecycle_test.db";
  std::vector<RowId> inserted_rids;

  {
    auto *db = new DBStorageEngine(db_name, true);
    auto &catalog = db->catalog_mgr_;
    Txn txn;

    std::vector<Column *> columns = {new Column("id", TypeId::kTypeInt, 0, false, true),
                                     new Column("name", TypeId::kTypeChar, 32, 1, false, false)};
    auto schema = std::make_shared<TableSchema>(columns);
    TableInfo *table_info = nullptr;
    ASSERT_EQ(DB_SUCCESS, catalog->CreateTable("users", schema.get(), &txn, table_info));
    ASSERT_NE(nullptr, table_info);

    const char *names[] = {"user1", "user2", "user3", "user4", "user5"};
    for (int i = 1; i <= 5; i++) {
      Row row = MakeUserRow(i, names[i - 1]);
      ASSERT_TRUE(table_info->GetTableHeap()->InsertTuple(row, &txn));
      inserted_rids.emplace_back(row.GetRowId());
    }

    IndexInfo *index_info = nullptr;
    ASSERT_EQ(DB_SUCCESS, catalog->CreateIndex("users", "idx_users_id", {"id"}, &txn, index_info, "bptree"));
    ASSERT_NE(nullptr, index_info);
    delete db;
  }

  {
    auto *db = new DBStorageEngine(db_name, false);
    auto &catalog = db->catalog_mgr_;
    Txn txn;

    TableInfo *table_info = nullptr;
    ASSERT_EQ(DB_SUCCESS, catalog->GetTable("users", table_info));
    ASSERT_NE(nullptr, table_info);

    IndexInfo *index_info = nullptr;
    ASSERT_EQ(DB_SUCCESS, catalog->GetIndex("users", "idx_users_id", index_info));
    ASSERT_NE(nullptr, index_info);

    std::vector<RowId> result;
    Row key = MakeIntKey(3);
    ASSERT_EQ(DB_SUCCESS, index_info->GetIndex()->ScanKey(key, result, &txn, "="));
    ASSERT_EQ(1, result.size());
    EXPECT_EQ(inserted_rids[2], result[0]);

    std::vector<IndexInfo *> indexes;
    ASSERT_EQ(DB_SUCCESS, catalog->GetTableIndexes("users", indexes));
    ASSERT_EQ(1, indexes.size());

    ASSERT_EQ(DB_SUCCESS, catalog->DropIndex("users", "idx_users_id"));
    ASSERT_EQ(DB_INDEX_NOT_FOUND, catalog->GetIndex("users", "idx_users_id", index_info));
    ASSERT_EQ(DB_SUCCESS, catalog->DropTable("users"));
    ASSERT_EQ(DB_TABLE_NOT_EXIST, catalog->GetTable("users", table_info));
    delete db;
  }

  {
    auto *db = new DBStorageEngine(db_name, false);
    TableInfo *table_info = nullptr;
    ASSERT_EQ(DB_TABLE_NOT_EXIST, db->catalog_mgr_->GetTable("users", table_info));
    delete db;
  }
}
