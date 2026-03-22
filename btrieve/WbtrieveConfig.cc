#include "WbtrieveConfig.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <system_error>

namespace btrieve {
namespace {

static std::string trim(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](unsigned char c) { return std::isspace(c); });
  if (first == value.end()) {
    return "";
  }

  const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                     [](unsigned char c) { return std::isspace(c); })
                        .base();
  return std::string(first, last);
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

static bool parseBool(const std::string& value, bool& parsedValue) {
  const auto lowered = toLower(trim(value));
  if (lowered == "true" || lowered == "1") {
    parsedValue = true;
    return true;
  }

  if (lowered == "false" || lowered == "0") {
    parsedValue = false;
    return true;
  }

  return false;
}

}  // namespace

WbtrieveConfig g_wbtrieveConfig;

WbtrieveConfig WbtrieveConfig::load(const std::filesystem::path& iniPath) {
  return loadFromPath(iniPath);
}

WbtrieveConfig WbtrieveConfig::loadFromPath(const std::filesystem::path& iniPath) {
  WbtrieveConfig config;

  try {
    std::ifstream input(iniPath);
    if (!input.is_open()) {
      return config;
    }

    enum class IniSection { None, Storage, Mirror };
    IniSection section = IniSection::None;

    std::string line;
    while (std::getline(input, line)) {
      const auto commentPos = line.find_first_of(";#");
      if (commentPos != std::string::npos) {
        line.erase(commentPos);
      }

      auto trimmed = trim(line);
      if (trimmed.empty()) {
        continue;
      }

      if (trimmed.front() == '[' && trimmed.back() == ']') {
        const auto sectionName = toLower(trim(trimmed.substr(1, trimmed.size() - 2)));
        if (sectionName == "storage") {
          section = IniSection::Storage;
        } else if (sectionName == "mirror") {
          section = IniSection::Mirror;
        } else {
          section = IniSection::None;
        }
        continue;
      }

      const auto equalsPos = trimmed.find('=');
      if (equalsPos == std::string::npos) {
        continue;
      }

      const auto key = toLower(trim(trimmed.substr(0, equalsPos)));
      const auto value = toLower(trim(trimmed.substr(equalsPos + 1)));

      if (section == IniSection::Storage && key == "mode") {
        if (value == "sqlite") {
          config.storageMode = StorageMode::Sqlite;
        } else if (value == "dat") {
          config.storageMode = StorageMode::Dat;
        } else if (value == "mirror") {
          config.storageMode = StorageMode::Mirror;
        } else if (value == "auto") {
          config.storageMode = StorageMode::Auto;
        }
      } else if (section == IniSection::Mirror && key == "sync_on_close") {
        bool parsedValue = true;
        if (parseBool(value, parsedValue)) {
          config.mirrorSyncOnClose = parsedValue;
        }
      }
    }
  } catch (const std::exception& ex) {
    std::cerr << "Failed to load wbtrv32 config from '" << iniPath.string()
              << "': " << ex.what() << std::endl;
    return WbtrieveConfig();
  } catch (...) {
    std::cerr << "Failed to load wbtrv32 config from '" << iniPath.string()
              << "': unknown error" << std::endl;
    return WbtrieveConfig();
  }

  return config;
}

StorageMode WbtrieveConfig::resolveAutoMode(const std::filesystem::path& dbPath,
                                            const std::filesystem::path& datPath) const {
  if (storageMode != StorageMode::Auto) {
    return storageMode;
  }

  try {
    std::error_code ec;
    const bool dbExists = std::filesystem::exists(dbPath, ec);
    if (ec) {
      return StorageMode::Sqlite;
    }

    const bool datExists = std::filesystem::exists(datPath, ec);
    if (ec) {
      return StorageMode::Sqlite;
    }

    if (dbExists && datExists) {
      const auto dbWriteTime = std::filesystem::last_write_time(dbPath, ec);
      if (ec) {
        return StorageMode::Sqlite;
      }

      const auto datWriteTime = std::filesystem::last_write_time(datPath, ec);
      if (ec) {
        return StorageMode::Sqlite;
      }

      return dbWriteTime >= datWriteTime ? StorageMode::Sqlite : StorageMode::Dat;
    }

    if (dbExists) {
      return StorageMode::Sqlite;
    }

    if (datExists) {
      return StorageMode::Dat;
    }
  } catch (const std::exception& ex) {
    std::cerr << "Failed to resolve auto storage mode: " << ex.what() << std::endl;
  } catch (...) {
    std::cerr << "Failed to resolve auto storage mode: unknown error" << std::endl;
  }

  return StorageMode::Sqlite;
}

}  // namespace btrieve
