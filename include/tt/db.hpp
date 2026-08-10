#pragma once

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdint>
#include <string>

namespace tt
{

    inline constexpr int kSchemaVersion = 1;

    inline constexpr std::int64_t kMinId = 100000;
    inline constexpr std::int64_t kMaxId = 999999;

    std::int64_t new_id(SQLite::Database &db, const std::string &table);

    SQLite::Database open_database(const std::string &path);

}
