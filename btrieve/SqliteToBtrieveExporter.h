#ifndef __SQLITE_TO_BTRIEVE_EXPORTER_H_
#define __SQLITE_TO_BTRIEVE_EXPORTER_H_

#include <string>

#include "ErrorCode.h"

namespace btrieve {

BtrieveError exportSqliteToBtrieveDat(const wchar_t* sqlitePath,
                                      const wchar_t* datPath,
                                      std::string* errorMessage = nullptr);

BtrieveError exportAllSqliteToBtrieveDatInDirectory(
    const wchar_t* directoryPath, unsigned int* exportedCount = nullptr,
    std::string* errorMessage = nullptr);

}  // namespace btrieve

#endif
