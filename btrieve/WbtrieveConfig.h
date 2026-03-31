#ifndef __WBTRIEVE_CONFIG_H_
#define __WBTRIEVE_CONFIG_H_

#include <filesystem>
#include <string>

namespace btrieve {

enum class StorageMode {
  Sqlite,  // Use SQLite (.db) as primary. Default.
  Dat,     // Use native Btrieve .dat files directly.
  Mirror,  // SQLite primary; write .dat mirror on every close.
  Auto,    // Auto-detect: prefer whichever file is newer.
};

class WbtrieveConfig {
 public:
  static WbtrieveConfig load(const std::filesystem::path& iniPath);
  static WbtrieveConfig loadFromPath(const std::filesystem::path& iniPath);

  StorageMode getStorageMode() const { return storageMode; }
  bool getMirrorSyncOnClose() const { return mirrorSyncOnClose; }

  bool shouldMirrorOnClose() const {
    return storageMode == StorageMode::Mirror && mirrorSyncOnClose;
  }

  StorageMode resolveAutoMode(const std::filesystem::path& dbPath,
                              const std::filesystem::path& datPath) const;

 private:
  StorageMode storageMode = StorageMode::Sqlite;
  bool mirrorSyncOnClose = true;

  explicit WbtrieveConfig(StorageMode mode, bool syncOnClose)
      : storageMode(mode), mirrorSyncOnClose(syncOnClose) {}

 public:
  WbtrieveConfig() = default;
};

extern WbtrieveConfig g_wbtrieveConfig;

}  // namespace btrieve

#endif
