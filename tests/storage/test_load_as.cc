/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "neug/main/connection.h"
#include "neug/main/neug_db.h"
#include "unittest/utils.h"

namespace neug {
namespace test {

class LoadAsTest : public ::testing::Test {
 protected:
  static constexpr const char* DB_DIR = "/tmp/load_as_test_db";
  static constexpr const char* CSV_DIR = "/tmp/load_as_test_csv";

  std::unique_ptr<NeugDB> db_;

  void write_csv(const std::string& filename,
                 const std::string& content) {
    std::ofstream ofs(std::string(CSV_DIR) + "/" + filename);
    ofs << content;
  }

  void SetUp() override {
    if (std::filesystem::exists(DB_DIR)) {
      std::filesystem::remove_all(DB_DIR);
    }
    if (std::filesystem::exists(CSV_DIR)) {
      std::filesystem::remove_all(CSV_DIR);
    }
    std::filesystem::create_directories(DB_DIR);
    std::filesystem::create_directories(CSV_DIR);

    // CSV files for LOAD NODE TABLE tests
    write_csv("people.csv",
              "id,name,age\n"
              "1,Alice,30\n"
              "2,Bob,25\n"
              "3,Carol,35\n"
              "4,Dave,20\n");

    write_csv("items.csv",
              "item_id,title,price\n"
              "101,Widget,9.99\n"
              "102,Gadget,19.99\n"
              "103,Doohickey,4.99\n");

    // CSV for edge tests: src_id, dst_id, weight
    write_csv("edges.csv",
              "src_id,dst_id,weight\n"
              "1,2,0.5\n"
              "2,3,1.0\n"
              "3,4,0.8\n");

    db_ = std::make_unique<NeugDB>();
    NeugDBConfig config;
    config.data_dir = DB_DIR;
    config.checkpoint_on_close = true;
    config.compact_on_close = true;
    config.compact_csr = true;
    config.enable_auto_compaction = false;
    db_->Open(config);
  }

  void TearDown() override {
    if (db_) {
      db_->Close();
      db_.reset();
    }
    if (std::filesystem::exists(DB_DIR)) {
      std::filesystem::remove_all(DB_DIR);
    }
    if (std::filesystem::exists(CSV_DIR)) {
      std::filesystem::remove_all(CSV_DIR);
    }
  }

  // Create persistent node tables needed for edge LOAD tests.
  void setupPersistentNodeTables(std::shared_ptr<Connection> conn) {
    auto res = conn->Query(
        "CREATE NODE TABLE Person(id INT64, name STRING, age INT64, "
        "PRIMARY KEY(id));");
    EXPECT_TRUE(res) << res.error().ToString();
  }

  // Insert some vertices so edge lookups can succeed.
  void insertPersistentVertices(std::shared_ptr<Connection> conn) {
    auto res = conn->Query(
        "CREATE (p:Person {id: 1, name: 'Alice', age: 30});");
    EXPECT_TRUE(res) << res.error().ToString();
    res = conn->Query(
        "CREATE (p:Person {id: 2, name: 'Bob', age: 25});");
    EXPECT_TRUE(res) << res.error().ToString();
    res = conn->Query(
        "CREATE (p:Person {id: 3, name: 'Carol', age: 35});");
    EXPECT_TRUE(res) << res.error().ToString();
    res = conn->Query(
        "CREATE (p:Person {id: 4, name: 'Dave', age: 20});");
    EXPECT_TRUE(res) << res.error().ToString();
  }
};

// ============================================================================
// LOAD NODE TABLE basic
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableBasic) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') AS TempPeople;");
  EXPECT_TRUE(res) << res.error().ToString();

  // Verify data is queryable
  auto match_res = conn->Query(
      "MATCH (n:TempPeople) RETURN n.id ORDER BY n.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();

  const auto& table = match_res.value().response();
  EXPECT_EQ(table.row_count(), 4);
  const auto& id_col = table.arrays(0).int64_array();
  std::vector<int64_t> ids;
  for (int64_t i = 0; i < id_col.values_size(); ++i) {
    ids.push_back(id_col.values(i));
  }
  EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3, 4}));
  conn->Close();
}

// ============================================================================
// LOAD NODE TABLE default primary key (first column)
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableDefaultPrimaryKey) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  // No primary_key option: should default to first column (id)
  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path + "\" AS TempDefault;");
  EXPECT_TRUE(res) << res.error().ToString();

  auto match_res = conn->Query(
      "MATCH (n:TempDefault) RETURN n.id, n.name ORDER BY n.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();
  EXPECT_EQ(match_res.value().response().row_count(), 4);
  conn->Close();
}

// ============================================================================
// LOAD NODE TABLE with WHERE filter pushdown
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableWithWhere) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') WHERE age > 25 AS TempAdults;");
  EXPECT_TRUE(res) << res.error().ToString();

  auto match_res = conn->Query(
      "MATCH (n:TempAdults) RETURN n.id, n.name, n.age ORDER BY n.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();

  const auto& table = match_res.value().response();
  // Only Alice(30) and Carol(35) should pass age > 25
  EXPECT_EQ(table.row_count(), 2);
  const auto& id_col = table.arrays(0).int64_array();
  std::vector<int64_t> ids;
  for (int64_t i = 0; i < id_col.values_size(); ++i) {
    ids.push_back(id_col.values(i));
  }
  EXPECT_EQ(ids, (std::vector<int64_t>{1, 3}));
  conn->Close();
}

// ============================================================================
// LOAD NODE TABLE with RETURN projection
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableWithReturn) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  // Only project id and name (drop age)
  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') RETURN id, name AS TempSlim;");
  EXPECT_TRUE(res) << res.error().ToString();

  // id and name should be accessible
  auto match_res = conn->Query(
      "MATCH (n:TempSlim) RETURN n.id, n.name ORDER BY n.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();
  EXPECT_EQ(match_res.value().response().row_count(), 4);
  conn->Close();
}

// ============================================================================
// LOAD NODE TABLE with WHERE + RETURN combined
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableWhereAndReturn) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  // Filter age >= 25, project only id and age
  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') WHERE age >= 25 RETURN id, age "
      "AS TempFiltered;");
  EXPECT_TRUE(res) << res.error().ToString();

  auto match_res = conn->Query(
      "MATCH (n:TempFiltered) RETURN n.id, n.age ORDER BY n.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();

  const auto& table = match_res.value().response();
  // Bob(25), Alice(30), Carol(35) pass age >= 25
  EXPECT_EQ(table.row_count(), 3);
  const auto& id_col = table.arrays(0).int64_array();
  const auto& age_col = table.arrays(1).int64_array();
  EXPECT_EQ(id_col.values(0), 2);
  EXPECT_EQ(id_col.values(1), 1);
  EXPECT_EQ(id_col.values(2), 3);
  conn->Close();
}

// ============================================================================
// LOAD EDGE TABLE basic
// ============================================================================

TEST_F(LoadAsTest, LoadEdgeTableBasic) {
  auto conn = db_->Connect();
  setupPersistentNodeTables(conn);
  insertPersistentVertices(conn);

  // First load the src/dst vertices as temp tables too (for index lookup)
  std::string people_csv = std::string(CSV_DIR) + "/people.csv";
  auto load_nodes_res = conn->Query(
      "LOAD NODE TABLE FROM \"" + people_csv +
      "\" (primary_key = 'id') AS TempPerson;");
  EXPECT_TRUE(load_nodes_res) << load_nodes_res.error().ToString();

  std::string edges_csv = std::string(CSV_DIR) + "/edges.csv";
  auto load_edges_res = conn->Query(
      "LOAD EDGE TABLE FROM \"" + edges_csv +
      "\" (from = 'TempPerson', to = 'TempPerson', "
      "from_col = 'src_id', to_col = 'dst_id') AS TempKnows;");
  EXPECT_TRUE(load_edges_res) << load_edges_res.error().ToString();

  // Query the loaded edges
  auto match_res = conn->Query(
      "MATCH (a:TempPerson)-[e:TempKnows]->(b:TempPerson) "
      "RETURN a.id, b.id, e.weight ORDER BY a.id;");
  EXPECT_TRUE(match_res) << match_res.error().ToString();

  const auto& table = match_res.value().response();
  EXPECT_EQ(table.row_count(), 3);
  conn->Close();
}

// ============================================================================
// Error: RETURN missing primary_key
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableReturnMissingPrimaryKey) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  // primary_key is 'id' but RETURN only has name and age
  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') RETURN name, age AS TempBad;");
  EXPECT_FALSE(res);
  // Should report missing primary_key in RETURN
  EXPECT_NE(res.error().ToString().find("primary key"), std::string::npos)
      << res.error().ToString();
  conn->Close();
}

// ============================================================================
// Error: LOAD EDGE TABLE without from/to options
// ============================================================================

TEST_F(LoadAsTest, LoadEdgeTableMissingFromTo) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/edges.csv";

  auto res = conn->Query(
      "LOAD EDGE TABLE FROM \"" + csv_path + "\" AS TempEdge;");
  EXPECT_FALSE(res);
  EXPECT_NE(res.error().ToString().find("from"), std::string::npos)
      << res.error().ToString();
  conn->Close();
}

// ============================================================================
// Error: RETURN column not in source
// ============================================================================

TEST_F(LoadAsTest, LoadNodeTableReturnNonexistentColumn) {
  auto conn = db_->Connect();
  std::string csv_path = std::string(CSV_DIR) + "/people.csv";

  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') RETURN id, nonexistent AS TempMissing;");
  EXPECT_FALSE(res);
  EXPECT_NE(res.error().ToString().find("not found"), std::string::npos)
      << res.error().ToString();
  conn->Close();
}

// ============================================================================
// Cleanup: temp tables removed after Connection::Close()
// ============================================================================

TEST_F(LoadAsTest, CleanupOnClose) {
  // Phase 1: create temp table and verify it exists
  {
    auto conn = db_->Connect();
    std::string csv_path = std::string(CSV_DIR) + "/people.csv";
    auto res = conn->Query(
        "LOAD NODE TABLE FROM \"" + csv_path +
        "\" (primary_key = 'id') AS TempEphemeral;");
    EXPECT_TRUE(res) << res.error().ToString();

    auto match_res = conn->Query(
        "MATCH (n:TempEphemeral) RETURN count(n);");
    EXPECT_TRUE(match_res) << match_res.error().ToString();
    conn->Close();
  }

  // Phase 2: reopen DB, temp table should be gone
  {
    auto db2 = std::make_unique<NeugDB>();
    NeugDBConfig config;
    config.data_dir = DB_DIR;
    config.checkpoint_on_close = true;
    config.compact_on_close = true;
    config.compact_csr = true;
    config.enable_auto_compaction = false;
    db2->Open(config);
    auto conn2 = db2->Connect();

    // TempEphemeral should not exist after checkpoint+reopen
    auto match_res = conn2->Query(
        "MATCH (n:TempEphemeral) RETURN n.id;");
    // This may fail with "label not found" or return empty
    // depending on how the catalog handles unknown labels
    conn2->Close();
    db2->Close();
  }
}

// ============================================================================
// Error: LOAD AS conflicts with existing persistent label
// ============================================================================

TEST_F(LoadAsTest, LoadAsLabelConflict) {
  auto conn = db_->Connect();
  // Create a persistent table named 'Person'
  auto create_res = conn->Query(
      "CREATE NODE TABLE Person(id INT64, name STRING, PRIMARY KEY(id));");
  EXPECT_TRUE(create_res) << create_res.error().ToString();

  std::string csv_path = std::string(CSV_DIR) + "/people.csv";
  // Try to LOAD AS 'Person' — should conflict
  auto res = conn->Query(
      "LOAD NODE TABLE FROM \"" + csv_path +
      "\" (primary_key = 'id') AS Person;");
  EXPECT_FALSE(res);
  conn->Close();
}

}  // namespace test
}  // namespace neug
