#include "SqliteToBtrieveExporter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "BtrieveException.h"
#include "Text.h"
#include "sqlite/sqlite3.h"

namespace btrieve {
namespace {

struct SqliteData {
  uint32_t recordLength = 0;
  uint32_t physicalRecordLength = 0;
  uint32_t pageLength = 0;
  bool variableLengthRecords = false;
  std::vector<std::vector<uint8_t>> records;
};

struct PatEntry {
  uint8_t type = 0;
  int32_t physicalPage = -1;
};

static uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static void writeLe16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void writeLe32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static bool fileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) &&
         std::filesystem::is_regular_file(path, ec);
}

static std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
  std::unique_ptr<FILE, decltype(&fclose)> f(
      fopen(path.string().c_str(), "rb"), &fclose);
  if (!f) {
    throw BtrieveException(BtrieveError::IOError, "Unable to open %s",
                           path.string().c_str());
  }

  fseek(f.get(), 0, SEEK_END);
  long length = ftell(f.get());
  fseek(f.get(), 0, SEEK_SET);

  if (length < 0) {
    throw BtrieveException(BtrieveError::IOError,
                           "Unable to read file length for %s",
                           path.string().c_str());
  }

  std::vector<uint8_t> data(static_cast<size_t>(length));
  if (!data.empty() &&
      fread(data.data(), 1, data.size(), f.get()) != data.size()) {
    throw BtrieveException(BtrieveError::IOError, "Failed reading %s",
                           path.string().c_str());
  }

  return data;
}

static void writeFileBytes(const std::filesystem::path& path,
                           const std::vector<uint8_t>& data) {
  std::unique_ptr<FILE, decltype(&fclose)> f(
      fopen(path.string().c_str(), "wb"), &fclose);
  if (!f) {
    throw BtrieveException(BtrieveError::IOError, "Unable to write %s",
                           path.string().c_str());
  }

  if (!data.empty() && fwrite(data.data(), 1, data.size(), f.get()) !=
                           data.size()) {
    throw BtrieveException(BtrieveError::IOError, "Failed writing %s",
                           path.string().c_str());
  }
}

static sqlite3* openSqlite(const std::filesystem::path& path) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path.string().c_str(), &db,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK || db == nullptr) {
    if (db) {
      sqlite3_close(db);
    }
    throw BtrieveException(BtrieveError::IOError, "Unable to open sqlite %s",
                           path.string().c_str());
  }
  return db;
}

static SqliteData loadSqliteData(const std::filesystem::path& sqlitePath) {
  SqliteData ret;
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(openSqlite(sqlitePath),
                                                        &sqlite3_close);

  {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT record_length, physical_record_length, page_length, "
        "variable_length_records FROM metadata_t";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw BtrieveException(BtrieveError::IOError,
                             "Failed preparing metadata query");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
        stmt, &sqlite3_finalize);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
      throw BtrieveException(BtrieveError::IOError,
                             "metadata_t has no rows in %s",
                             sqlitePath.string().c_str());
    }

    ret.recordLength = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
    ret.physicalRecordLength =
        static_cast<uint32_t>(sqlite3_column_int(stmt, 1));
    ret.pageLength = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
    ret.variableLengthRecords = sqlite3_column_int(stmt, 3) != 0;
  }

  {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT data FROM data_t ORDER BY id";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw BtrieveException(BtrieveError::IOError,
                             "Failed preparing records query");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
        stmt, &sqlite3_finalize);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const uint8_t* blob =
          reinterpret_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
      const int blobLen = sqlite3_column_bytes(stmt, 0);
      if (blobLen < 0) {
        throw BtrieveException(BtrieveError::IOError,
                               "Invalid blob length in data_t");
      }

      std::vector<uint8_t> record(static_cast<size_t>(blobLen));
      if (blobLen > 0 && blob != nullptr) {
        memcpy(record.data(), blob, static_cast<size_t>(blobLen));
      }
      ret.records.emplace_back(std::move(record));
    }
  }

  if (ret.pageLength < 512 || ret.physicalRecordLength < 2 ||
      ret.recordLength == 0) {
    throw BtrieveException(BtrieveError::IOError,
                           "Invalid metadata values in %s",
                           sqlitePath.string().c_str());
  }

  return ret;
}

static int32_t decodePatPhysicalPage(const uint8_t* entry) {
  const int64_t page =
      (static_cast<int64_t>(entry[0]) << 16) |
      (static_cast<int64_t>(entry[3]) << 8) | static_cast<int64_t>(entry[2]);
  if (page == 0xFFFFFFL) {
    return -1;
  }
  return static_cast<int32_t>(page);
}

static void encodePatEntry(uint8_t* entry, uint8_t type, int32_t physicalPage) {
  if (physicalPage < 0) {
    entry[0] = entry[2] = entry[3] = 0xFF;
    entry[1] = 0x00;
    return;
  }

  entry[0] = static_cast<uint8_t>((physicalPage >> 16) & 0xFF);
  entry[1] = type;
  entry[2] = static_cast<uint8_t>(physicalPage & 0xFF);
  entry[3] = static_cast<uint8_t>((physicalPage >> 8) & 0xFF);
}

static std::vector<PatEntry> readTemplatePatEntries(
    const std::vector<uint8_t>& templateDat, uint32_t pageLength,
    uint32_t pageCountLogical) {
  std::vector<PatEntry> entries(pageCountLogical);

  if (templateDat.size() < static_cast<size_t>(pageLength) * 4u) {
    return entries;
  }

  const uint8_t* pat1 = templateDat.data() + (2u * pageLength);
  const uint8_t* pat2 = templateDat.data() + (3u * pageLength);

  const uint32_t pat1Usage = readLe32(pat1 + 4);
  const uint32_t pat2Usage = readLe32(pat2 + 4);
  const uint8_t* activePat = (pat2Usage >= pat1Usage) ? pat2 : pat1;

  const uint32_t pagesPerPat = (pageLength / 4u) - 2u;
  if (pageCountLogical > pagesPerPat) {
    throw BtrieveException(BtrieveError::IOError,
                           "Exporter currently supports a single PAT page");
  }

  for (uint32_t logical = 0; logical < pageCountLogical; ++logical) {
    const uint8_t* e = activePat + 8 + (logical * 4);
    entries[logical].type = e[1];
    entries[logical].physicalPage = decodePatPhysicalPage(e);
  }

  return entries;
}

static void patchFcrPage(uint8_t* page, uint32_t pageLength,
                         uint32_t recordLength, uint32_t physicalRecordLength,
                         uint32_t recordCount, uint32_t pageCountLogical,
                         bool variableLengthRecords, uint32_t usageCount) {
  if (page[0] != 'F' || page[1] != 'C') {
    page[0] = 'F';
    page[1] = 'C';
    page[2] = 0;
    page[3] = 0;
  }

  writeLe32(page + 4, usageCount);
  writeLe16(page + 0x08, static_cast<uint16_t>(pageLength));
  writeLe16(page + 0x16, static_cast<uint16_t>(recordLength));
  writeLe16(page + 0x18, static_cast<uint16_t>(physicalRecordLength));

  writeLe16(page + 0x1A, static_cast<uint16_t>((recordCount >> 16) & 0xFFFF));
  writeLe16(page + 0x1C, static_cast<uint16_t>(recordCount & 0xFFFF));

  writeLe16(page + 0x26,
            static_cast<uint16_t>((pageCountLogical >> 16) & 0xFFFF));
  writeLe16(page + 0x28, static_cast<uint16_t>(pageCountLogical & 0xFFFF));

  page[0x38] = variableLengthRecords ? 0x01 : 0x00;

  // deleted record pointer = no deleted records
  page[0x10] = page[0x11] = page[0x12] = page[0x13] = 0xFF;
}

static void writePatPage(uint8_t* page, uint32_t pageLength,
                         const std::vector<PatEntry>& logicalEntries,
                         uint32_t usageCount) {
  memset(page, 0, pageLength);
  page[0] = 'P';
  page[1] = 'P';
  page[2] = 0x01;
  page[3] = 0x00;
  writeLe32(page + 4, usageCount);

  const uint32_t pagesPerPat = (pageLength / 4u) - 2u;
  const uint32_t maxEntries = std::min<uint32_t>(pagesPerPat,
                                                 logicalEntries.size());

  for (uint32_t logical = 0; logical < maxEntries; ++logical) {
    uint8_t* e = page + 8 + (logical * 4);
    encodePatEntry(e, logicalEntries[logical].type,
                   logicalEntries[logical].physicalPage);
  }

  for (uint32_t logical = maxEntries; logical < pagesPerPat; ++logical) {
    uint8_t* e = page + 8 + (logical * 4);
    encodePatEntry(e, 0, -1);
  }
}

static uint32_t maxPhysicalPageFromPat(const std::vector<PatEntry>& entries) {
  uint32_t maxPage = 3;
  for (const auto& e : entries) {
    if (e.physicalPage >= 0) {
      maxPage = std::max(maxPage, static_cast<uint32_t>(e.physicalPage));
    }
  }
  return maxPage;
}

static void writeDataPage(uint8_t* page, uint32_t pageLength,
                          uint32_t logicalPage, uint32_t physicalRecordLength,
                          uint32_t recordLength,
                          const std::vector<std::vector<uint8_t>>& records,
                          size_t startRecordIndex) {
  memset(page, 0, pageLength);

  page[0] = 0x00;
  page[1] = 'D';
  writeLe16(page + 2, static_cast<uint16_t>(logicalPage & 0xFFFF));
  writeLe16(page + 4, 0x8000);  // active

  const uint32_t recordsPerPage = (pageLength - 6u) / physicalRecordLength;

  for (uint32_t slot = 0; slot < recordsPerPage; ++slot) {
    const size_t recordIndex = startRecordIndex + slot;
    uint8_t* phys = page + 6 + (slot * physicalRecordLength);

    if (recordIndex >= records.size()) {
      continue;
    }

    // v6 fixed-length physical slot: 2-byte usage count, then record
    phys[0] = 0x00;
    phys[1] = 0x01;

    const auto& rec = records[recordIndex];
    const size_t toCopy = std::min<size_t>(recordLength, rec.size());
    if (toCopy > 0) {
      memcpy(phys + 2, rec.data(), toCopy);
    }
    if (toCopy < recordLength) {
      memset(phys + 2 + toCopy, 0, recordLength - toCopy);
    }
  }
}

static std::filesystem::path replaceExtensionCaseAware(
    const std::filesystem::path& path, const std::wstring& extLower,
    const std::wstring& extUpper) {
  auto lower = path;
  lower.replace_extension(extLower);
  if (fileExists(lower)) {
    return lower;
  }

  auto upper = path;
  upper.replace_extension(extUpper);
  if (fileExists(upper)) {
    return upper;
  }

  return lower;
}

static BtrieveError exportInternal(const std::filesystem::path& sqlitePath,
                                   const std::filesystem::path& datPath) {
  const SqliteData sqliteData = loadSqliteData(sqlitePath);

  const uint32_t pageLength = sqliteData.pageLength;
  const uint32_t recordLength = sqliteData.recordLength;
  const uint32_t physicalRecordLength = sqliteData.physicalRecordLength;

  if (sqliteData.variableLengthRecords) {
    throw BtrieveException(
        BtrieveError::NotSupported,
        "SQLite->DAT exporter currently supports fixed-length records only");
  }

  if (physicalRecordLength <= 2) {
    throw BtrieveException(BtrieveError::NotSupported,
                           "Invalid physical record length");
  }

  const uint32_t recordsPerPage = (pageLength - 6u) / physicalRecordLength;
  if (recordsPerPage == 0) {
    throw BtrieveException(BtrieveError::NotSupported,
                           "Record size too large for page");
  }

  std::vector<uint8_t> templateDat;
  if (fileExists(datPath)) {
    templateDat = readFileBytes(datPath);
  }

  uint32_t templateLogicalPageCount = 1;
  if (templateDat.size() >= pageLength * 2u) {
    const uint8_t* fcr1 = templateDat.data();
    const uint8_t* fcr2 = templateDat.data() + pageLength;
    const uint32_t fcr1Usage = readLe32(fcr1 + 4);
    const uint32_t fcr2Usage = readLe32(fcr2 + 4);
    const uint8_t* activeFcr = (fcr2Usage >= fcr1Usage) ? fcr2 : fcr1;
    templateLogicalPageCount =
        (static_cast<uint32_t>(readLe16(activeFcr + 0x26)) << 16) |
        readLe16(activeFcr + 0x28);
    if (templateLogicalPageCount == 0) {
      templateLogicalPageCount = 1;
    }
  }

  std::vector<PatEntry> templateEntries =
      readTemplatePatEntries(templateDat, pageLength, templateLogicalPageCount);

  std::vector<uint32_t> preservedLogical;
  std::vector<uint32_t> preservedPhysical;
  std::vector<uint32_t> templateDataLogical;

  for (uint32_t logical = 0; logical < templateEntries.size(); ++logical) {
    const auto& e = templateEntries[logical];
    if (e.physicalPage < 0) {
      continue;
    }

    if (e.type == 'D' || e.type == 'V') {
      templateDataLogical.push_back(logical);
    } else {
      preservedLogical.push_back(logical);
      preservedPhysical.push_back(static_cast<uint32_t>(e.physicalPage));
    }
  }

  const uint32_t dataPagesNeeded =
      static_cast<uint32_t>((sqliteData.records.size() + recordsPerPage - 1) /
                            recordsPerPage);

  std::vector<uint32_t> dataLogicalPages;
  dataLogicalPages.reserve(std::max<uint32_t>(1, dataPagesNeeded));
  for (size_t i = 0; i < templateDataLogical.size() &&
                     dataLogicalPages.size() < dataPagesNeeded;
       ++i) {
    dataLogicalPages.push_back(templateDataLogical[i]);
  }

  uint32_t nextLogicalPage = 1;
  if (!templateEntries.empty()) {
    nextLogicalPage = static_cast<uint32_t>(templateEntries.size());
  }
  while (dataLogicalPages.size() < dataPagesNeeded) {
    dataLogicalPages.push_back(nextLogicalPage++);
  }

  uint32_t logicalPageCount = 1;
  for (uint32_t lp : preservedLogical) {
    logicalPageCount = std::max(logicalPageCount, lp + 1);
  }
  for (uint32_t lp : dataLogicalPages) {
    logicalPageCount = std::max(logicalPageCount, lp + 1);
  }

  const uint32_t pagesPerPat = (pageLength / 4u) - 2u;
  if (logicalPageCount > pagesPerPat) {
    throw BtrieveException(BtrieveError::NotSupported,
                           "Logical page count exceeds single PAT capacity");
  }

  std::vector<PatEntry> newPat(logicalPageCount);
  for (uint32_t i = 0; i < newPat.size(); ++i) {
    newPat[i].type = 0;
    newPat[i].physicalPage = -1;
  }

  for (uint32_t i = 0; i < preservedLogical.size(); ++i) {
    newPat[preservedLogical[i]].type = templateEntries[preservedLogical[i]].type;
    newPat[preservedLogical[i]].physicalPage =
        templateEntries[preservedLogical[i]].physicalPage;
  }

  uint32_t nextPhysicalPage = 4;
  if (!templateDat.empty()) {
    nextPhysicalPage = static_cast<uint32_t>(templateDat.size() / pageLength);
  }
  nextPhysicalPage =
      std::max(nextPhysicalPage, maxPhysicalPageFromPat(newPat) + 1);

  std::vector<uint32_t> dataPhysicalPages;
  dataPhysicalPages.reserve(dataLogicalPages.size());
  for (size_t i = 0; i < dataLogicalPages.size(); ++i) {
    int32_t phys = -1;
    if (i < templateDataLogical.size()) {
      phys = templateEntries[templateDataLogical[i]].physicalPage;
    }

    if (phys < 0) {
      phys = static_cast<int32_t>(nextPhysicalPage++);
    }

    dataPhysicalPages.push_back(static_cast<uint32_t>(phys));
  }

  for (size_t i = 0; i < dataLogicalPages.size(); ++i) {
    newPat[dataLogicalPages[i]].type = 'D';
    newPat[dataLogicalPages[i]].physicalPage =
        static_cast<int32_t>(dataPhysicalPages[i]);
  }

  const uint32_t maxPhysicalPage = maxPhysicalPageFromPat(newPat);
  const size_t outBytes = static_cast<size_t>(maxPhysicalPage + 1u) * pageLength;
  std::vector<uint8_t> out(outBytes, 0);

  // Preserve non-data pages from template if available.
  if (!templateDat.empty()) {
    const size_t copyLen = std::min(templateDat.size(), out.size());
    memcpy(out.data(), templateDat.data(), copyLen);
  }

  if (out.size() < static_cast<size_t>(4u * pageLength)) {
    throw BtrieveException(BtrieveError::IOError,
                           "Output DAT too small for FCR/PAT pages");
  }

  // Write fresh data pages
  for (size_t i = 0; i < dataPhysicalPages.size(); ++i) {
    const uint32_t physicalPage = dataPhysicalPages[i];
    const uint32_t logicalPage = dataLogicalPages[i];
    uint8_t* page = out.data() + (static_cast<size_t>(physicalPage) * pageLength);
    writeDataPage(page, pageLength, logicalPage, physicalRecordLength,
                  recordLength, sqliteData.records,
                  i * static_cast<size_t>(recordsPerPage));
  }

  // FCR shadow pair (pages 0 and 1)
  patchFcrPage(out.data() + 0, pageLength, recordLength, physicalRecordLength,
               static_cast<uint32_t>(sqliteData.records.size()),
               logicalPageCount, sqliteData.variableLengthRecords, 1);
  patchFcrPage(out.data() + pageLength, pageLength, recordLength,
               physicalRecordLength,
               static_cast<uint32_t>(sqliteData.records.size()),
               logicalPageCount, sqliteData.variableLengthRecords, 2);

  // PAT shadow pair (pages 2 and 3)
  writePatPage(out.data() + (2u * pageLength), pageLength, newPat, 1);
  writePatPage(out.data() + (3u * pageLength), pageLength, newPat, 2);

  writeFileBytes(datPath, out);
  return BtrieveError::Success;
}

}  // namespace

BtrieveError exportSqliteToBtrieveDat(const wchar_t* sqlitePath,
                                      const wchar_t* datPath,
                                      std::string* errorMessage) {
  try {
    if (sqlitePath == nullptr || datPath == nullptr) {
      return BtrieveError::InvalidFileName;
    }

    return exportInternal(std::filesystem::path(sqlitePath),
                          std::filesystem::path(datPath));
  } catch (const BtrieveException& ex) {
    if (errorMessage) {
      *errorMessage = ex.getErrorMessage();
    }
    return ex.getError();
  } catch (const std::exception& ex) {
    if (errorMessage) {
      *errorMessage = ex.what();
    }
    return BtrieveError::IOError;
  } catch (...) {
    if (errorMessage) {
      *errorMessage = "unknown error";
    }
    return BtrieveError::IOError;
  }
}

BtrieveError exportAllSqliteToBtrieveDatInDirectory(
    const wchar_t* directoryPath, unsigned int* exportedCount,
    std::string* errorMessage) {
  if (exportedCount) {
    *exportedCount = 0;
  }

  if (directoryPath == nullptr) {
    return BtrieveError::InvalidFileName;
  }

  try {
    std::filesystem::path dir(directoryPath);
    if (!std::filesystem::is_directory(dir)) {
      return BtrieveError::InvalidFileName;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }

      auto p = entry.path();
      std::wstring ext = p.extension().wstring();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
      });
      if (ext != L".db") {
        continue;
      }

      auto datPath = replaceExtensionCaseAware(p, L".dat", L".DAT");
      BtrieveError err = exportInternal(p, datPath);
      if (err != BtrieveError::Success) {
        return err;
      }

      if (exportedCount) {
        ++(*exportedCount);
      }
    }

    return BtrieveError::Success;
  } catch (const BtrieveException& ex) {
    if (errorMessage) {
      *errorMessage = ex.getErrorMessage();
    }
    return ex.getError();
  } catch (const std::exception& ex) {
    if (errorMessage) {
      *errorMessage = ex.what();
    }
    return BtrieveError::IOError;
  } catch (...) {
    if (errorMessage) {
      *errorMessage = "unknown error";
    }
    return BtrieveError::IOError;
  }
}

}  // namespace btrieve
