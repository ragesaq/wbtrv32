#include "WbtrieveConfig.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

using namespace btrieve;

class WbtrieveConfigTest : public ::testing::Test {
 protected:
  std::filesystem::path tempDir;

  void SetUp() override {
    tempDir = std::filesystem::temp_directory_path() /
              std::filesystem::path("wbtrv32-config-test-" +
                                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                    "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(tempDir);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
  }

  std::filesystem::path writeIni(const std::string& content) {
    const auto iniPath = tempDir / "wbtrv32.ini";
    std::ofstream out(iniPath);
    out << content;
    out.close();
    return iniPath;
  }
};

TEST_F(WbtrieveConfigTest, MissingIniDefaultsToSqliteAndMirrorSyncOnCloseTrue) {
  const auto config = WbtrieveConfig::load(tempDir / "does-not-exist.ini");
  EXPECT_EQ(config.getStorageMode(), StorageMode::Sqlite);
  EXPECT_TRUE(config.getMirrorSyncOnClose());
}

TEST_F(WbtrieveConfigTest, ParsesStorageModeSqlite) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=sqlite\n"));
  EXPECT_EQ(config.getStorageMode(), StorageMode::Sqlite);
}

TEST_F(WbtrieveConfigTest, ParsesStorageModeDat) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=dat\n"));
  EXPECT_EQ(config.getStorageMode(), StorageMode::Dat);
}

TEST_F(WbtrieveConfigTest, ParsesStorageModeMirror) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=mirror\n"));
  EXPECT_EQ(config.getStorageMode(), StorageMode::Mirror);
}

TEST_F(WbtrieveConfigTest, ParsesStorageModeAuto) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=auto\n"));
  EXPECT_EQ(config.getStorageMode(), StorageMode::Auto);
}

TEST_F(WbtrieveConfigTest, ParsesSyncOnCloseFalse) {
  const auto config =
      WbtrieveConfig::load(writeIni("[mirror]\nsync_on_close=false\n"));
  EXPECT_FALSE(config.getMirrorSyncOnClose());
}

TEST_F(WbtrieveConfigTest, ResolveAutoModeOnlyDbExistsReturnsSqlite) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=auto\n"));

  const auto dbPath = tempDir / "sample.db";
  const auto datPath = tempDir / "sample.dat";
  std::ofstream(dbPath).close();

  EXPECT_EQ(config.resolveAutoMode(dbPath, datPath), StorageMode::Sqlite);
}

TEST_F(WbtrieveConfigTest, ResolveAutoModeOnlyDatExistsReturnsDat) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=auto\n"));

  const auto dbPath = tempDir / "sample.db";
  const auto datPath = tempDir / "sample.dat";
  std::ofstream(datPath).close();

  EXPECT_EQ(config.resolveAutoMode(dbPath, datPath), StorageMode::Dat);
}

TEST_F(WbtrieveConfigTest, ResolveAutoModeBothExistNewerDbReturnsSqlite) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=auto\n"));

  const auto dbPath = tempDir / "sample.db";
  const auto datPath = tempDir / "sample.dat";
  std::ofstream(dbPath).close();
  std::ofstream(datPath).close();

  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(datPath, now - std::chrono::seconds(10));
  std::filesystem::last_write_time(dbPath, now);

  EXPECT_EQ(config.resolveAutoMode(dbPath, datPath), StorageMode::Sqlite);
}

TEST_F(WbtrieveConfigTest, ResolveAutoModeBothExistNewerDatReturnsDat) {
  const auto config = WbtrieveConfig::load(writeIni("[storage]\nmode=auto\n"));

  const auto dbPath = tempDir / "sample.db";
  const auto datPath = tempDir / "sample.dat";
  std::ofstream(dbPath).close();
  std::ofstream(datPath).close();

  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(dbPath, now - std::chrono::seconds(10));
  std::filesystem::last_write_time(datPath, now);

  EXPECT_EQ(config.resolveAutoMode(dbPath, datPath), StorageMode::Dat);
}
