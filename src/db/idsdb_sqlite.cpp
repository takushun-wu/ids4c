#include "ids4c/idsconst.h"
#include "idsdb_internal.h"

#include <sqlite3.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>

class SQLiteStatement {
public:
    SQLiteStatement(sqlite3* database, const char* sql) {
        _valid = sqlite3_prepare_v2(database, sql, -1, &_statement, nullptr) == SQLITE_OK;
    }

    ~SQLiteStatement() {
        if(_statement != nullptr) sqlite3_finalize(_statement);
    }

    bool          valid() const { return _valid; }
    sqlite3_stmt* get() const { return _statement; }

private:
    sqlite3_stmt* _statement = nullptr;
    bool          _valid     = false;
};


static bool SQLiteExecute(sqlite3* database, const char* sql) {
    char*     error  = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if(result == SQLITE_OK) return true;

    std::cerr << "SQLite error: " << (error != nullptr ? error : sqlite3_errmsg(database)) << std::endl;
    if(error != nullptr) sqlite3_free(error);
    return false;
}


static std::string SQLiteColumnText(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text == nullptr ? "" : reinterpret_cast<const char*>(text);
}

static bool SQLiteTableExists(sqlite3* database, const char* tableName);
static constexpr const char* kComponentIndexVersion = "2";


// SQLite 中的 raw_ids_text 保存完整的根部唯一化前缀，加载时单独提取。
static std::string SQLiteLeadingUniqueSeparator(const std::string& expression) {
    if(expression.size() < 4 || expression.front() != '{') return "";
    const size_t closing = expression.find('}', 1);
    if(closing == std::string::npos || closing <= 1 || closing + 1 >= expression.size()) return "";
    return expression.substr(0, closing + 1);
}
static const char* DatabaseFormatName(IDSdbFormat format) {
    return format == IDSDB_YIBAI ? "yibai" : "default";
}

static bool ParseDatabaseFormat(const std::string& value, IDSdbFormat& format) {
    if(value == "default")
        format = IDSDB_DEFAULT;
    else if(value == "yibai")
        format = IDSDB_YIBAI;
    else
        return false;
    return true;
}


static sqlite3_int64 InsertSQLiteGlyph(sqlite3_stmt* statement, Ideograph glyph, const std::string& strokeCounts) {
    const std::string glyphKey = glyph.toString();
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_text(statement, 1, glyphKey.c_str(), -1, SQLITE_TRANSIENT);
    if(glyph.inUnicode()) {
        sqlite3_bind_int64(statement, 2, glyph.GetIdeo());
        if(glyph.GetVS() != 0)
            sqlite3_bind_int64(statement, 3, glyph.GetVS());
        else
            sqlite3_bind_null(statement, 3);
    } else {
        sqlite3_bind_null(statement, 2);
        sqlite3_bind_null(statement, 3);
    }
    const std::string suffix       = glyph.GetSuffix();
    const std::string abstractName = glyph.GetAbstractName();
    sqlite3_bind_text(statement, 4, suffix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, abstractName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, strokeCounts.c_str(), -1, SQLITE_TRANSIENT);
    if(sqlite3_step(statement) != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(sqlite3_db_handle(statement));
}

static std::string SerializeStrokeCounts(const StrokeCountSet& counts) {
    std::vector<uint32_t> ordered(counts.begin(), counts.end());
    std::sort(ordered.begin(), ordered.end());

    std::string serialized = "[";
    for(size_t index = 0; index < ordered.size(); index++) {
        if(index != 0) serialized += ',';
        serialized += std::to_string(ordered[index]);
    }
    return serialized + ']';
}

static StrokeCountSet ParseStrokeCounts(const std::string& serialized) {
    StrokeCountSet counts;
    size_t         position = serialized.find_first_not_of(" \t\r\n");
    if(position == std::string::npos || serialized[position] != '[') return counts;
    position++;

    while(true) {
        position = serialized.find_first_not_of(" \t\r\n", position);
        if(position == std::string::npos) return StrokeCountSet();
        if(serialized[position] == ']') {
            position = serialized.find_first_not_of(" \t\r\n", position + 1);
            return position == std::string::npos ? counts : StrokeCountSet();
        }
        if(serialized[position] < '0' || serialized[position] > '9') return StrokeCountSet();

        uint64_t value = 0;
        while(position < serialized.length() && serialized[position] >= '0' && serialized[position] <= '9') {
            value = value * 10 + static_cast<uint64_t>(serialized[position] - '0');
            if(value > std::numeric_limits<uint32_t>::max()) return StrokeCountSet();
            position++;
        }
        counts.insert(static_cast<uint32_t>(value));

        position = serialized.find_first_not_of(" \t\r\n", position);
        if(position == std::string::npos) return StrokeCountSet();
        if(serialized[position] == ',') {
            position++;
            continue;
        }
        if(serialized[position] != ']') return StrokeCountSet();
        position = serialized.find_first_not_of(" \t\r\n", position + 1);
        return position == std::string::npos ? counts : StrokeCountSet();
    }
}


bool IDSdatabase::LoadSqliteDatabase(const std::string& filename) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        _lastError = "Cannot open database: " + filename;
        return false;
    }

    _lastError.clear();
    bool        ok = true;
    std::string formatName;
    std::string schemaVersion;
    std::string hvCacheVersion;
    std::string componentIndexVersion;
    {
        SQLiteStatement formatStatement(database, "SELECT value FROM metadata WHERE key = 'format'");
        SQLiteStatement schemaStatement(database, "SELECT value FROM metadata WHERE key = 'schema_version'");
        if(!formatStatement.valid() || !schemaStatement.valid() || sqlite3_step(formatStatement.get()) != SQLITE_ROW ||
            sqlite3_step(schemaStatement.get()) != SQLITE_ROW)
            ok = false;
        else {
            formatName    = SQLiteColumnText(formatStatement.get(), 0);
            schemaVersion = SQLiteColumnText(schemaStatement.get(), 0);
        }
    }

    if(ok) {
        SQLiteStatement cacheVersionStatement(database, "SELECT value FROM metadata WHERE key = 'hv_cache_version'");
        if(cacheVersionStatement.valid() && sqlite3_step(cacheVersionStatement.get()) == SQLITE_ROW)
            hvCacheVersion = SQLiteColumnText(cacheVersionStatement.get(), 0);
        SQLiteStatement indexVersionStatement(database, "SELECT value FROM metadata WHERE key = 'component_index_version'");
        if(indexVersionStatement.valid() && sqlite3_step(indexVersionStatement.get()) == SQLITE_ROW)
            componentIndexVersion = SQLiteColumnText(indexVersionStatement.get(), 0);
    }
    IDSdbFormat format = IDSDB_DEFAULT;
    if(ok && !ParseDatabaseFormat(formatName, format)) {
        _lastError = "Unknown database format.";
        ok         = false;
    }
    if(ok && schemaVersion != "5" && schemaVersion != "6" && schemaVersion != "7") {
        _lastError = "Database schema 5, 6 or 7 is required. Please reimport the source IDS file.";
        ok         = false;
    }
    const bool hasPrivateGlyphTable = schemaVersion == "6" || schemaVersion == "7";
    const bool rebuildHVCache = hvCacheVersion != "5";

    if(ok) {
        _rawIDSDB.clear();
        ResetRuntimeCaches();
        _format = format;
        ConfigureDatabaseFormat(config, _format);

        SQLiteStatement rawStatement(database,
            "SELECT glyphs.glyph_key, ids_entries.raw_ids_text "
            "FROM ids_entries JOIN glyphs ON glyphs.id = ids_entries.glyph_id "
            "ORDER BY ids_entries.glyph_id, ids_entries.ordinal");
        if(!rawStatement.valid()) {
            _lastError = "The database does not contain raw IDS entries.";
            ok         = false;
        } else {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(rawStatement.get())) == SQLITE_ROW) {
                const std::string glyphText = SQLiteColumnText(rawStatement.get(), 0);
                const std::string rawText   = SQLiteColumnText(rawStatement.get(), 1);
                const std::string uniqueSeparator = SQLiteLeadingUniqueSeparator(rawText);
                IDSOwner          raw       = ParseIDSOwned(rawText);
                if(raw == nullptr || !AddRawIDS(Ideograph(glyphText), std::move(raw), uniqueSeparator)) {
                    _lastError = "Invalid raw IDS entry for " + glyphText;
                    ok         = false;
                    break;
                }
            }
            if(result != SQLITE_DONE) ok = false;
        }
    }

    if(ok && hasPrivateGlyphTable) {
        SQLiteStatement privateStatement(database,
            "SELECT glyphs.glyph_key FROM private_glyphs "
            "JOIN glyphs ON glyphs.id = private_glyphs.glyph_id "
            "ORDER BY private_glyphs.glyph_id");
        if(!privateStatement.valid()) {
            _lastError = "The database does not contain private glyph metadata.";
            ok         = false;
        } else {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(privateStatement.get())) == SQLITE_ROW) {
                const Ideograph glyph(SQLiteColumnText(privateStatement.get(), 0));
                if(_rawIDSDB.find(glyph) == _rawIDSDB.end()) {
                    _lastError = "Private glyph metadata refers to a missing raw IDS entry.";
                    ok         = false;
                    break;
                }
                _privateGlyphs.insert(glyph);
            }
            if(result != SQLITE_DONE) ok = false;
        }
    }

    if(ok) {
        BuildSameIDSHashIndex();
    }

    if(ok) {
        SQLiteStatement queryStatement(database,
            "SELECT glyphs.glyph_key, query_ids_entries.ids_text "
            "FROM query_ids_entries JOIN glyphs ON glyphs.id = query_ids_entries.glyph_id "
            "ORDER BY query_ids_entries.glyph_id, query_ids_entries.ordinal");
        if(!queryStatement.valid()) {
            _lastError = "The database does not contain the HV query cache.";
            ok         = false;
        } else {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(queryStatement.get())) == SQLITE_ROW) {
                const std::string glyphText = SQLiteColumnText(queryStatement.get(), 0);
                IDSOwner          cached    = ParseIDSOwned(SQLiteColumnText(queryStatement.get(), 1));
                if(cached == nullptr || ContainsQueryOnlyOperator(cached.get())) {
                    _lastError = "Invalid HV query-cache entry for " + glyphText;
                    ok         = false;
                    break;
                }
                _idsDB[Ideograph(glyphText)].push_back(std::move(cached));
            }
            if(result != SQLITE_DONE) ok = false;
            if(ok && _idsDB.empty() && !_rawIDSDB.empty()) {
                _lastError = "The HV query cache is empty. Please reimport the source IDS file.";
                ok         = false;
            }
        }
    }

    if(ok) {
        SQLiteStatement strokeStatement(database,
            "SELECT glyphs.glyph_key, stroke_sequences.sequence "
            "FROM stroke_sequences JOIN glyphs ON glyphs.id = stroke_sequences.glyph_id "
            "ORDER BY stroke_sequences.glyph_id");
        if(!strokeStatement.valid())
            ok = false;
        else {

            int result = SQLITE_ROW;
            while((result = sqlite3_step(strokeStatement.get())) == SQLITE_ROW)
                _strokeDB[SQLiteColumnText(strokeStatement.get(), 0)] =
                    StringSplit(SQLiteColumnText(strokeStatement.get(), 1), U',');
            if(result != SQLITE_DONE) ok = false;
        }
    }

    if(ok) {
        SQLiteStatement composedStatement(database,
            "SELECT glyphs.glyph_key, query_stroke_neutral_entries.ids_text "
            "FROM query_stroke_neutral_entries JOIN glyphs ON glyphs.id = query_stroke_neutral_entries.glyph_id "
            "ORDER BY query_stroke_neutral_entries.glyph_id, query_stroke_neutral_entries.ordinal");
        // 旧版本没有该派生表；只要稍后重建 HV 缓存即可继续迁移。
        if(!composedStatement.valid()) {
            if(!rebuildHVCache) {
                _lastError = "The stroke-neutral composition cache is missing.";
                ok = false;
            }
        } else {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(composedStatement.get())) == SQLITE_ROW) {
                const std::string glyphText = SQLiteColumnText(composedStatement.get(), 0);
                IDSOwner cached = ParseIDSOwned(SQLiteColumnText(composedStatement.get(), 1));
                if(cached == nullptr || ContainsQueryOnlyOperator(cached.get())) {
                    _lastError = "Invalid stroke-neutral composition cache entry for " + glyphText;
                    ok = false;
                    break;
                }
                _strokeNeutralCompositionDB[Ideograph(glyphText)].push_back(std::move(cached));
            }
            if(result != SQLITE_DONE) ok = false;
        }
    }
    if(ok) {
        SQLiteStatement strokeCountStatement(
            database, "SELECT glyph_key, stroke_count FROM glyphs WHERE stroke_count IS NOT NULL");
        if(strokeCountStatement.valid()) {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(strokeCountStatement.get())) == SQLITE_ROW) {
                const StrokeCountSet counts = ParseStrokeCounts(SQLiteColumnText(strokeCountStatement.get(), 1));
                if(!counts.empty())
                    _strokeCountCache[Ideograph(SQLiteColumnText(strokeCountStatement.get(), 0))] = counts;
            }
        }
    }
    bool upgradeComponentIndex = false;
    bool hasLegacyComponentIndex = false;
    if(ok && !rebuildHVCache) {
        bool loadedIndex = false;
        const int allowedMask = (1 << _directComponentIndex.size()) - 1;
        if(SQLiteTableExists(database, "query_component_postings") &&
            (componentIndexVersion == "1" || componentIndexVersion == kComponentIndexVersion)) {
            const bool legacy = componentIndexVersion == "1";
            SQLiteStatement indexStatement(database, legacy
                    ? "SELECT p.source_kind, p.component_key, glyphs.glyph_key "
                      "FROM query_component_postings AS p JOIN glyphs ON glyphs.id = p.glyph_id"
                    : "SELECT p.source_mask, p.component_key, glyphs.glyph_key "
                      "FROM query_component_postings AS p JOIN glyphs ON glyphs.id = p.glyph_id");
            if(indexStatement.valid()) {
                int result = SQLITE_ROW;
                while((result = sqlite3_step(indexStatement.get())) == SQLITE_ROW) {
                    const int value = sqlite3_column_int(indexStatement.get(), 0);
                    const int mask = legacy
                        ? (value >= 0 && value < static_cast<int>(_directComponentIndex.size()) ? 1 << value : 0)
                        : value;
                    if(mask <= 0 || (mask & ~allowedMask) != 0) break;
                    const std::string key = SQLiteColumnText(indexStatement.get(), 1);
                    const Ideograph glyph(SQLiteColumnText(indexStatement.get(), 2));
                    for(size_t source = 0; source < _directComponentIndex.size(); source++)
                        if((mask & (1 << source)) != 0) _directComponentIndex[source][key].insert(glyph);
                }
                loadedIndex = result == SQLITE_DONE && !_directComponentIndex[1].empty();
                hasLegacyComponentIndex = loadedIndex && legacy;
            }
        }
        if(loadedIndex) {
            _directComponentIndexReady = true;
        } else {
            for(auto& source: _directComponentIndex) source.clear();
            BuildDirectComponentIndex();
        }
        const bool knownVersion = componentIndexVersion.empty() || componentIndexVersion == "1" ||
            componentIndexVersion == kComponentIndexVersion;
        upgradeComponentIndex = knownVersion && (componentIndexVersion != kComponentIndexVersion || !loadedIndex);
    }
    sqlite3_close(database);
    if(ok && rebuildHVCache) {
        if(!BuildQueryCacheFromRaw() || !SaveSqliteDatabase(filename))
            ok = false;
    } else if(ok && upgradeComponentIndex) {
        // 派生索引升级失败不应使已加载的原始 IDS 和内存索引无法查询。
        UpgradeComponentIndexSqlite(filename, hasLegacyComponentIndex);
    }
    if(!ok) {
        _rawIDSDB.clear();
        ResetRuntimeCaches();
    }
    return ok;
}

bool IDSdatabase::UpgradeComponentIndexSqlite(const std::string& filename, bool hasLegacyIndex) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return false;
    }

    bool ok = SQLiteExecute(database,
        "PRAGMA foreign_keys = ON;"
        "BEGIN IMMEDIATE;"
        "DROP TABLE IF EXISTS query_component_postings_v2;"
        "CREATE TABLE query_component_postings_v2 ("
        "  component_key TEXT NOT NULL,"
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  source_mask INTEGER NOT NULL CHECK(source_mask BETWEEN 1 AND 7),"
        "  PRIMARY KEY (component_key, glyph_id)"
        ") WITHOUT ROWID;");

    if(ok && hasLegacyIndex) {
        ok = SQLiteExecute(database,
            "INSERT INTO query_component_postings_v2(component_key, glyph_id, source_mask) "
            "SELECT component_key, glyph_id, SUM(1 << source_kind) "
            "FROM query_component_postings GROUP BY component_key, glyph_id;");
    } else if(ok) {
        std::unordered_map<std::string, sqlite3_int64> glyphIds;
        SQLiteStatement glyphStatement(database, "SELECT id, glyph_key FROM glyphs");
        if(!glyphStatement.valid()) ok = false;
        if(ok) {
            int result = SQLITE_ROW;
            while((result = sqlite3_step(glyphStatement.get())) == SQLITE_ROW)
                glyphIds.emplace(SQLiteColumnText(glyphStatement.get(), 1), sqlite3_column_int64(glyphStatement.get(), 0));
            ok = result == SQLITE_DONE;
        }

        SQLiteStatement insertStatement(database,
            "INSERT INTO query_component_postings_v2(component_key, glyph_id, source_mask) VALUES(?, ?, ?) "
            "ON CONFLICT(component_key, glyph_id) DO UPDATE SET "
            "source_mask = source_mask | excluded.source_mask");
        if(!insertStatement.valid()) ok = false;
        for(size_t source = 0; source < _directComponentIndex.size() && ok; source++) {
            for(const auto& posting: _directComponentIndex[source]) {
                if(!ok) break;
                for(const Ideograph& glyph: posting.second) {
                    const auto found = glyphIds.find(glyph.toString());
                    if(found == glyphIds.end()) {
                        ok = false;
                        break;
                    }
                    sqlite3_reset(insertStatement.get());
                    sqlite3_clear_bindings(insertStatement.get());
                    sqlite3_bind_text(insertStatement.get(), 1, posting.first.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insertStatement.get(), 2, found->second);
                    sqlite3_bind_int(insertStatement.get(), 3, 1 << source);
                    ok = sqlite3_step(insertStatement.get()) == SQLITE_DONE;
                    if(!ok) break;
                }
            }
        }
    }

    if(ok)
        ok = SQLiteExecute(database,
            "DROP TABLE IF EXISTS query_component_postings;"
            "ALTER TABLE query_component_postings_v2 RENAME TO query_component_postings;");
    if(ok) {
        SQLiteStatement versionStatement(database,
            "INSERT INTO metadata(key, value) VALUES('component_index_version', ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        if(!versionStatement.valid())
            ok = false;
        else {
            sqlite3_bind_text(versionStatement.get(), 1, kComponentIndexVersion, -1, SQLITE_STATIC);
            ok = sqlite3_step(versionStatement.get()) == SQLITE_DONE;
        }
    }
    if(ok) ok = SQLiteExecute(database, "COMMIT;");
    if(!ok) SQLiteExecute(database, "ROLLBACK;");
    // VACUUM 在事务提交后收回旧 v1 表的页面；失败不影响已经提交的索引。
    if(ok) SQLiteExecute(database, "VACUUM;");
    sqlite3_close(database);
    return ok;
}

bool IDSdatabase::SaveSqliteDatabase(const std::string& filename) {
    if(!_directComponentIndexReady) BuildDirectComponentIndex();
    // sqlite3_open_v2(...CREATE) 会在失败路径留下空文件。已有有效数据库
    // 保存失败时应由事务回滚；没有有效旧库时则清理这个半成品。
    std::ifstream existingDatabase(filename, std::ios::binary | std::ios::ate);
    const bool    hadUsableDatabase = existingDatabase.good() && existingDatabase.tellg() > 0;

    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        if(!hadUsableDatabase) std::remove(filename.c_str());
        _lastError = "Cannot write database: " + filename;
        return false;
    }

    bool ok = SQLiteExecute(database,
        "PRAGMA foreign_keys = ON;"
        "BEGIN IMMEDIATE;"
        // "DROP TABLE IF EXISTS glyph_stroke_counts;"
        // "DROP TABLE IF EXISTS ids_entry_stroke_counts;"
        "DROP TABLE IF EXISTS private_glyphs;"
        "DROP TABLE IF EXISTS stroke_sequences;"
        // "DROP TABLE IF EXISTS iwds_subtree_index_entries;"
        // "DROP TABLE IF EXISTS iwds_subtree_index_keys;"
        "DROP TABLE IF EXISTS query_stroke_neutral_entries;"
        "DROP TABLE IF EXISTS query_component_postings;"
        "DROP TABLE IF EXISTS query_ids_entries;"
        "DROP TABLE IF EXISTS ids_same_expression;"
        "DROP TABLE IF EXISTS ids_entries;"
        "DROP TABLE IF EXISTS glyphs;"
        "DROP TABLE IF EXISTS metadata;"
        "CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE glyphs ("
        "  id INTEGER PRIMARY KEY,"
        "  glyph_key TEXT NOT NULL UNIQUE,"
        "  codepoint INTEGER,"
        "  variation_selector INTEGER,"
        "  suffix TEXT NOT NULL DEFAULT '',"
        "  abstract_name TEXT NOT NULL DEFAULT '',"
        "  stroke_count TEXT NOT NULL DEFAULT '[]'"
        ");"
        "CREATE TABLE private_glyphs ("
        "  glyph_id INTEGER PRIMARY KEY REFERENCES glyphs(id)"
        ");"
        "CREATE TABLE ids_entries ("
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  ordinal INTEGER NOT NULL,"
        "  raw_ids_text TEXT NOT NULL,"
        "  stroke_count TEXT NOT NULL DEFAULT '[]',"
        "  PRIMARY KEY (glyph_id, ordinal)"
        ");"
        "CREATE TABLE query_ids_entries ("
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  ordinal INTEGER NOT NULL,"
        "  ids_text TEXT NOT NULL,"
        "  PRIMARY KEY (glyph_id, ordinal)"
        ");"
        "CREATE TABLE ids_same_expression ("
        "  expression_hash INTEGER NOT NULL,"
        "  ids_text TEXT NOT NULL,"
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  not_equivalent INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (expression_hash, ids_text, glyph_id)"
        ");"
        "CREATE TABLE query_stroke_neutral_entries ("
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  ordinal INTEGER NOT NULL,"
        "  ids_text TEXT NOT NULL,"
        "  PRIMARY KEY (glyph_id, ordinal)"
        ");"
        "CREATE TABLE query_component_postings ("
        "  component_key TEXT NOT NULL,"
        "  glyph_id INTEGER NOT NULL REFERENCES glyphs(id),"
        "  source_mask INTEGER NOT NULL CHECK(source_mask BETWEEN 1 AND 7),"
        "  PRIMARY KEY (component_key, glyph_id)"
        ") WITHOUT ROWID;"
        "CREATE TABLE stroke_sequences ("
        "  glyph_id INTEGER PRIMARY KEY REFERENCES glyphs(id),"
        "  sequence TEXT NOT NULL"
        ");"
        "CREATE INDEX ids_entries_raw_ids_text_idx ON ids_entries(raw_ids_text);"
        "CREATE INDEX query_ids_entries_ids_text_idx ON query_ids_entries(ids_text);"
        "CREATE INDEX query_stroke_neutral_entries_ids_text_idx ON query_stroke_neutral_entries(ids_text);"
        "CREATE INDEX ids_same_expression_hash_idx ON ids_same_expression(expression_hash);");

    if(ok) {
        SQLiteStatement metadataStatement(database, "INSERT INTO metadata(key, value) VALUES(?, ?)");
        SQLiteStatement glyphStatement(database,
            "INSERT INTO glyphs(glyph_key, codepoint, variation_selector, suffix, abstract_name, stroke_count) " "VALUES(?, ?, ?, ?, ?, ?)");
        SQLiteStatement rawStatement(
            database, "INSERT INTO ids_entries(glyph_id, ordinal, raw_ids_text, stroke_count) VALUES(?, ?, ?, ?)");
        SQLiteStatement queryStatement(
            database, "INSERT INTO query_ids_entries(glyph_id, ordinal, ids_text) VALUES(?, ?, ?)");
        SQLiteStatement sameExpressionStatement(
            database,
            "INSERT INTO ids_same_expression(expression_hash, ids_text, glyph_id, not_equivalent) VALUES(?, ?, ?, ?)");
        SQLiteStatement composedStatement(
            database, "INSERT INTO query_stroke_neutral_entries(glyph_id, ordinal, ids_text) VALUES(?, ?, ?)");
        SQLiteStatement componentStatement(database,
            "INSERT INTO query_component_postings(component_key, glyph_id, source_mask) VALUES(?, ?, ?) "
            "ON CONFLICT(component_key, glyph_id) DO UPDATE SET "
            "source_mask = source_mask | excluded.source_mask");
        SQLiteStatement privateStatement(database, "INSERT INTO private_glyphs(glyph_id) VALUES(?)");
        SQLiteStatement strokeStatement(database, "INSERT INTO stroke_sequences(glyph_id, sequence) VALUES(?, ?)");
        if(!metadataStatement.valid() || !glyphStatement.valid() || !rawStatement.valid() || !queryStatement.valid() ||
            !composedStatement.valid() || !componentStatement.valid() ||
            !sameExpressionStatement.valid() || !privateStatement.valid() || !strokeStatement.valid())
            ok = false;

        auto insertMetadata = [&](const char* key, const char* value) {
            sqlite3_reset(metadataStatement.get());
            sqlite3_clear_bindings(metadataStatement.get());
            sqlite3_bind_text(metadataStatement.get(), 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_text(metadataStatement.get(), 2, value, -1, SQLITE_STATIC);
            return sqlite3_step(metadataStatement.get()) == SQLITE_DONE;
        };
        if(ok) ok = insertMetadata("format", DatabaseFormatName(_format));
        if(ok) ok = insertMetadata("schema_version", "6");
        if(ok) ok = insertMetadata("hv_cache_version", "5");
        if(ok) ok = insertMetadata("component_index_version", kComponentIndexVersion);

        std::unordered_map<std::string, sqlite3_int64> glyphIds;
        auto                                           ensureGlyph = [&](const Ideograph& glyph) -> sqlite3_int64 {
            const std::string key   = glyph.toString();
            const auto        found = glyphIds.find(key);
            if(found != glyphIds.end()) return found->second;
            const auto        cachedCounts = _strokeCountCache.find(glyph);
            const std::string strokeCounts =
                cachedCounts == _strokeCountCache.end() ? "[]" : SerializeStrokeCounts(cachedCounts->second);
            const sqlite3_int64 id = InsertSQLiteGlyph(glyphStatement.get(), glyph, strokeCounts);
            if(id <= 0) {
                ok = false;
                return -1;
            }
            glyphIds.insert({key, id});
            if(_privateGlyphs.find(glyph) != _privateGlyphs.end()) {
                sqlite3_reset(privateStatement.get());
                sqlite3_clear_bindings(privateStatement.get());
                sqlite3_bind_int64(privateStatement.get(), 1, id);
                if(sqlite3_step(privateStatement.get()) != SQLITE_DONE) {
                    ok = false;
                    return -1;
                }
            }
            return id;
        };

        // Persist the exact lv0 source definitions and their exact stroke counts.
        for(const auto& glyphEntry: _rawIDSDB) {
            if(!ok) break;
            const sqlite3_int64 glyphId = ensureGlyph(glyphEntry.first);
            if(glyphId <= 0) break;

            for(size_t index = 0; index < glyphEntry.second.size() && ok; index++) {
                IDS*              ids          = glyphEntry.second[index].get();
                std::string       idsText      = ids->toString();
                const auto uniqueSeparators   = _rawIDSUniqueSeparators.find(glyphEntry.first);
                if(uniqueSeparators != _rawIDSUniqueSeparators.end() && index < uniqueSeparators->second.size() &&
                    !uniqueSeparators->second[index].empty())
                    idsText = uniqueSeparators->second[index] + idsText;
                const std::string strokeCounts = SerializeStrokeCounts(GetStrokeCounts(ids));
                sqlite3_reset(rawStatement.get());
                sqlite3_clear_bindings(rawStatement.get());
                sqlite3_bind_int64(rawStatement.get(), 1, glyphId);
                sqlite3_bind_int64(rawStatement.get(), 2, static_cast<sqlite3_int64>(index));
                sqlite3_bind_text(rawStatement.get(), 3, idsText.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(rawStatement.get(), 4, strokeCounts.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(rawStatement.get()) == SQLITE_DONE;
            }
        }

        for(const auto& hashEntry: _sameIDSHashIndex) {
            if(!ok) break;
            for(const SameIDSHashGroup& group: hashEntry.second) {
                if(!ok) break;
                for(const Ideograph& glyph: group.glyphs) {
                    const sqlite3_int64 glyphId = ensureGlyph(glyph);
                    if(glyphId <= 0) {
                        ok = false;
                        break;
                    }
                    sqlite3_reset(sameExpressionStatement.get());
                    sqlite3_clear_bindings(sameExpressionStatement.get());
                    sqlite3_bind_int64(
                        sameExpressionStatement.get(), 1, static_cast<sqlite3_int64>(hashEntry.first));
                    sqlite3_bind_text(
                        sameExpressionStatement.get(), 2, group.expression.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(sameExpressionStatement.get(), 3, glyphId);
                    sqlite3_bind_int(
                        sameExpressionStatement.get(), 4,
                        group.notEquivalentGlyphs.find(glyph) != group.notEquivalentGlyphs.end() ? 1 : 0);
                    if(sqlite3_step(sameExpressionStatement.get()) != SQLITE_DONE) {
                        ok = false;
                        break;
                    }
                }
            }
        }

        // A cache entry is intentionally independent from raw ordinal because one raw IDS can have many HV expansions.
        for(const auto& glyphEntry: _idsDB) {
            if(!ok) break;
            const sqlite3_int64 glyphId = ensureGlyph(glyphEntry.first);
            if(glyphId <= 0) break;

            for(size_t index = 0; index < glyphEntry.second.size() && ok; index++) {
                std::string idsText = glyphEntry.second[index]->toString();
                if(!glyphEntry.second[index]->GetUniqueSeparator().empty())
                    idsText = glyphEntry.second[index]->GetUniqueSeparator() + idsText;
                sqlite3_reset(queryStatement.get());
                sqlite3_clear_bindings(queryStatement.get());
                sqlite3_bind_int64(queryStatement.get(), 1, glyphId);
                sqlite3_bind_int64(queryStatement.get(), 2, static_cast<sqlite3_int64>(index));
                sqlite3_bind_text(queryStatement.get(), 3, idsText.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(queryStatement.get()) == SQLITE_DONE;
            }
        }

        for(const auto& entry: _strokeDB) {
            if(!ok) break;
            const sqlite3_int64 glyphId = ensureGlyph(Ideograph(entry.first));
            if(glyphId <= 0) break;
            std::string sequence;
            for(size_t index = 0; index < entry.second.size(); index++) {
                if(index != 0) sequence += ',';
                sequence += entry.second[index];
            }
            sqlite3_reset(strokeStatement.get());
            sqlite3_clear_bindings(strokeStatement.get());
            sqlite3_bind_int64(strokeStatement.get(), 1, glyphId);
            sqlite3_bind_text(strokeStatement.get(), 2, sequence.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(strokeStatement.get()) == SQLITE_DONE;
        }
        for(const auto& glyphEntry: _strokeNeutralCompositionDB) {
            if(!ok) break;
            const sqlite3_int64 glyphId = ensureGlyph(glyphEntry.first);
            if(glyphId <= 0) break;

            for(size_t index = 0; index < glyphEntry.second.size() && ok; index++) {
                std::string idsText = glyphEntry.second[index]->toString();
                if(!glyphEntry.second[index]->GetUniqueSeparator().empty())
                    idsText = glyphEntry.second[index]->GetUniqueSeparator() + idsText;
                sqlite3_reset(composedStatement.get());
                sqlite3_clear_bindings(composedStatement.get());
                sqlite3_bind_int64(composedStatement.get(), 1, glyphId);
                sqlite3_bind_int64(composedStatement.get(), 2, static_cast<sqlite3_int64>(index));
                sqlite3_bind_text(composedStatement.get(), 3, idsText.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(composedStatement.get()) == SQLITE_DONE;
            }
        }
        for(size_t source = 0; source < _directComponentIndex.size() && ok; source++) {
            for(const auto& posting: _directComponentIndex[source]) {
                if(!ok) break;
                for(const Ideograph& glyph: posting.second) {
                    const sqlite3_int64 glyphId = ensureGlyph(glyph);
                    if(glyphId <= 0) break;
                    sqlite3_reset(componentStatement.get());
                    sqlite3_clear_bindings(componentStatement.get());
                    sqlite3_bind_text(componentStatement.get(), 1, posting.first.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(componentStatement.get(), 2, glyphId);
                    sqlite3_bind_int(componentStatement.get(), 3, 1 << source);
                    ok = sqlite3_step(componentStatement.get()) == SQLITE_DONE;
                    if(!ok) break;
                }
            }
        }
    }

    if(ok)
        ok = SQLiteExecute(database, "COMMIT;");
    else
        SQLiteExecute(database, "ROLLBACK;");
    if(ok) SQLiteExecute(database, "VACUUM;");
    sqlite3_close(database);
    if(ok) {
        _pendingStrokeCountCache.clear();
        _lastError.clear();
    } else {
        if(!hadUsableDatabase) std::remove(filename.c_str());
        _lastError = "Failed to save the SQLite database.";
    }
    return ok;
}

bool IDSdatabase::SaveStrokeCountCache(const std::string& filename) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return false;
    }

    bool hasGlyphStrokeCountColumn = false;
    bool hasEntryStrokeCountColumn = false;
    bool ok                        = true;
    {
        SQLiteStatement glyphColumnStatement(
            database, "SELECT 1 FROM pragma_table_info('glyphs') WHERE name = 'stroke_count'");
        SQLiteStatement entryColumnStatement(
            database, "SELECT 1 FROM pragma_table_info('ids_entries') WHERE name = 'stroke_count'");
        if(!glyphColumnStatement.valid() || !entryColumnStatement.valid())
            ok = false;
        else {
            hasGlyphStrokeCountColumn = sqlite3_step(glyphColumnStatement.get()) == SQLITE_ROW;
            hasEntryStrokeCountColumn = sqlite3_step(entryColumnStatement.get()) == SQLITE_ROW;
        }
    }
    if(ok && !hasGlyphStrokeCountColumn)
        ok = SQLiteExecute(database, "ALTER TABLE glyphs ADD COLUMN stroke_count TEXT NOT NULL DEFAULT '[]';");
    if(ok && !hasEntryStrokeCountColumn)
        ok = SQLiteExecute(database, "ALTER TABLE ids_entries ADD COLUMN stroke_count TEXT NOT NULL DEFAULT '[]';");
    if(ok)
        ok = SQLiteExecute(database,
            "DROP TABLE IF EXISTS glyph_stroke_counts;" "DROP TABLE IF EXISTS ids_entry_stroke_counts;");

    if(ok) ok = SQLiteExecute(database, "BEGIN IMMEDIATE;");
    if(ok) {
        // 空数组显式表示当前 IDS 数据无法完成笔画推导，避免留下 NULL。
        ok = SQLiteExecute(database,
            "UPDATE glyphs SET stroke_count = '[]' WHERE stroke_count IS NULL;" "UPDATE ids_entries SET stroke_count = '[]' WHERE stroke_count IS NULL;");
    }
    if(ok) {
        SQLiteStatement updateGlyphStatement(database, "UPDATE glyphs SET stroke_count = ? WHERE glyph_key = ?");
        SQLiteStatement updateEntryStatement(database,
            "UPDATE ids_entries SET stroke_count = ? " "WHERE glyph_id = (SELECT id FROM glyphs WHERE glyph_key = ?) AND ordinal = ?");
        if(!updateGlyphStatement.valid() || !updateEntryStatement.valid()) ok = false;

        for(const Ideograph& glyph: _pendingStrokeCountCache) {
            if(!ok) break;
            const auto cached = _strokeCountCache.find(glyph);
            if(cached == _strokeCountCache.end()) continue;
            const std::string glyphKey    = glyph.toString();
            const std::string glyphCounts = SerializeStrokeCounts(cached->second);

            sqlite3_reset(updateGlyphStatement.get());
            sqlite3_clear_bindings(updateGlyphStatement.get());
            sqlite3_bind_text(updateGlyphStatement.get(), 1, glyphCounts.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(updateGlyphStatement.get(), 2, glyphKey.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(updateGlyphStatement.get()) == SQLITE_DONE;

            const auto glyphEntries = _idsDB.find(glyph);
            if(glyphEntries == _idsDB.end()) continue;
            for(size_t ordinal = 0; ordinal < glyphEntries->second.size() && ok; ordinal++) {
                const std::string entryCounts =
                    SerializeStrokeCounts(GetStrokeCounts(glyphEntries->second[ordinal].get()));
                sqlite3_reset(updateEntryStatement.get());
                sqlite3_clear_bindings(updateEntryStatement.get());
                sqlite3_bind_text(updateEntryStatement.get(), 1, entryCounts.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(updateEntryStatement.get(), 2, glyphKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(updateEntryStatement.get(), 3, static_cast<sqlite3_int64>(ordinal));
                ok = sqlite3_step(updateEntryStatement.get()) == SQLITE_DONE;
            }
        }
    }

    if(ok)
        ok = SQLiteExecute(database, "COMMIT;");
    else
        SQLiteExecute(database, "ROLLBACK;");
    sqlite3_close(database);
    if(ok) _pendingStrokeCountCache.clear();
    return ok;
}

static bool SQLiteTableExists(sqlite3* database, const char* tableName) {
    SQLiteStatement statement(database, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
    if(!statement.valid()) return false;
    sqlite3_bind_text(statement.get(), 1, tableName, -1, SQLITE_STATIC);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}


bool IDSdatabase::LoadUnifiableSqlite(const std::string& filename) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return false;
    }

    for(auto& groups: _unifiableIdeoGroups)
        groups.clear();
    for(auto& index: _unifiableIdeoGroupIndex)
        index.clear();
    for(auto& groups: _unifiableIDSGroups)
        groups.clear();
    for(auto& index: _unifiableIDSGroupIndex)
        index.clear();
    for(auto& groups: _unifiableIDSParsedGroups)
        groups.clear();
    for(auto& index: _unifiableIDSShapeIndex)
        index.clear();

    std::array<UnificationGroupBuilder, IWDS_UNIFICATION_LEVEL_COUNT>           ideographBuilders;
    std::array<UnificationExpressionGroupBuilder, IWDS_UNIFICATION_LEVEL_COUNT> expressionBuilders;
    bool                                                                        ok = true;
    if(SQLiteTableExists(database, "unifiable_group_members")) {
        SQLiteStatement statement(database,
            "SELECT unifiable_groups.level, unifiable_group_members.group_id, unifiable_group_members.glyph_key "
            "FROM unifiable_group_members "
            "JOIN unifiable_groups ON unifiable_groups.id = unifiable_group_members.group_id "
            "ORDER BY unifiable_groups.level, unifiable_group_members.group_id, unifiable_group_members.ordinal");
        ok                                    = statement.valid();
        int                      currentLevel = -1;
        int                      currentGroup = -1;
        std::vector<std::string> members;
        int                      result     = SQLITE_ROW;
        auto                     flushGroup = [&]() {
            if(currentLevel < 0 || members.empty()) return;
            std::vector<Ideograph> ideographs;
            bool                   allIdeographs = true;
            for(const std::string& member: members) {
                IDSOwner parsed = ParseIDSOwned(member);
                if(parsed == nullptr || !IsIdeograph(parsed.get())) {
                    allIdeographs = false;
                    break;
                }
                ideographs.push_back(*AsIdeograph(parsed.get()));
            }
            if(allIdeographs)
                ideographBuilders[static_cast<size_t>(currentLevel)].AddGroup(ideographs);
            else
                expressionBuilders[static_cast<size_t>(currentLevel)].AddGroup(members);
            members.clear();
        };
        while(ok && (result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const int level   = sqlite3_column_int(statement.get(), 0);
            const int groupId = sqlite3_column_int(statement.get(), 1);
            if(level < static_cast<int>(IWDS_UNIFICATION_NONE) ||
                level >= static_cast<int>(IWDS_UNIFICATION_LEVEL_COUNT)) {
                ok = false;
                break;
            }
            if(currentGroup != -1 && (currentLevel != level || currentGroup != groupId)) flushGroup();
            currentLevel = level;
            currentGroup = groupId;
            members.push_back(SQLiteColumnText(statement.get(), 2));
        }
        if(ok && currentGroup != -1) flushGroup();
        ok = ok && result == SQLITE_DONE;
    } else if(SQLiteTableExists(database, "unifiable_pairs")) {
        // v1/v2 使用双向关系边；读取时合并回无冗余的字形等价组。
        SQLiteStatement levelColumnStatement(
            database, "SELECT 1 FROM pragma_table_info('unifiable_pairs') WHERE name = 'level'");
        if(!levelColumnStatement.valid())
            ok = false;
        else {
            const bool      hasLevelColumn = sqlite3_step(levelColumnStatement.get()) == SQLITE_ROW;
            SQLiteStatement statement(database,
                hasLevelColumn
                    ? "SELECT level, glyph_key, related_key FROM unifiable_pairs ORDER BY level, glyph_key, ordinal"
                    : "SELECT glyph_key, related_key FROM unifiable_pairs ORDER BY glyph_key, ordinal");
            ok         = statement.valid();
            int result = SQLITE_ROW;
            while(ok && (result = sqlite3_step(statement.get())) == SQLITE_ROW) {
                const int level = hasLevelColumn ? sqlite3_column_int(statement.get(), 0)
                                                 : static_cast<int>(IWDS_UNIFICATION_SOURCE_CODE_SEPARATION);
                if(level < static_cast<int>(IWDS_UNIFICATION_NONE) ||
                    level >= static_cast<int>(IWDS_UNIFICATION_LEVEL_COUNT)) {
                    ok = false;
                    break;
                }
                const int glyphColumn   = hasLevelColumn ? 1 : 0;
                const int relatedColumn = hasLevelColumn ? 2 : 1;
                ideographBuilders[static_cast<size_t>(level)].AddPair(
                    Ideograph(SQLiteColumnText(statement.get(), glyphColumn)),
                    Ideograph(SQLiteColumnText(statement.get(), relatedColumn)));
            }
            ok = ok && result == SQLITE_DONE;
        }
    } else
        ok = false;

    sqlite3_close(database);
    if(ok) {
        for(size_t level = 0; level < _unifiableIdeoGroups.size(); level++) {
            _unifiableIdeoGroups[level] = ideographBuilders[level].BuildGroups();
            _unifiableIDSGroups[level]  = expressionBuilders[level].BuildGroups();
        }
        BuildUnificationGroupIndexes();
        BuildUnificationIDSGroupIndexes();
    } else {
        for(auto& groups: _unifiableIdeoGroups)
            groups.clear();
        for(auto& index: _unifiableIdeoGroupIndex)
            index.clear();
        for(auto& groups: _unifiableIDSGroups)
            groups.clear();
        for(auto& index: _unifiableIDSGroupIndex)
            index.clear();
        for(auto& groups: _unifiableIDSParsedGroups)
            groups.clear();
        for(auto& index: _unifiableIDSShapeIndex)
            index.clear();
    }
    return ok;
}


bool IDSdatabase::SaveUnifiableSqlite(const std::string& filename) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(filename.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if(database != nullptr) sqlite3_close(database);
        return false;
    }

    bool ok = SQLiteExecute(database,
        "BEGIN IMMEDIATE;"
        "DROP TABLE IF EXISTS unifiable_group_members;"
        "DROP TABLE IF EXISTS unifiable_groups;"
        // "DROP TABLE IF EXISTS unifiable_pairs;"
        // "DROP TABLE IF EXISTS iwds_subtree_index_entries;"
        // "DROP TABLE IF EXISTS iwds_subtree_index_keys;"
        "DROP TABLE IF EXISTS metadata;"
        "CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE unifiable_groups ("
        "  id INTEGER PRIMARY KEY,"
        "  level INTEGER NOT NULL"
        ");"
        "CREATE TABLE unifiable_group_members ("
        "  group_id INTEGER NOT NULL REFERENCES unifiable_groups(id),"
        "  glyph_key TEXT NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  PRIMARY KEY (group_id, ordinal),"
        "  UNIQUE (group_id, glyph_key)"
        ");"
        "CREATE INDEX unifiable_group_members_glyph_idx ON unifiable_group_members(glyph_key);");
    if(ok) {
        SQLiteStatement metadataStatement(database, "INSERT INTO metadata(key, value) VALUES(?, ?)");
        SQLiteStatement groupStatement(database, "INSERT INTO unifiable_groups(level) VALUES(?)");
        SQLiteStatement memberStatement(
            database, "INSERT INTO unifiable_group_members(group_id, glyph_key, ordinal) VALUES(?, ?, ?)");
        if(!metadataStatement.valid() || !groupStatement.valid() || !memberStatement.valid())
            ok = false;
        else {
            auto insertMetadata = [&](const char* key, const char* value) {
                sqlite3_reset(metadataStatement.get());
                sqlite3_clear_bindings(metadataStatement.get());
                sqlite3_bind_text(metadataStatement.get(), 1, key, -1, SQLITE_STATIC);
                sqlite3_bind_text(metadataStatement.get(), 2, value, -1, SQLITE_STATIC);
                return sqlite3_step(metadataStatement.get()) == SQLITE_DONE;
            };
            auto insertGroup = [&](size_t level, const std::vector<std::string>& members) {
                if(members.size() < 2) return true;
                sqlite3_reset(groupStatement.get());
                sqlite3_clear_bindings(groupStatement.get());
                sqlite3_bind_int(groupStatement.get(), 1, static_cast<int>(level));
                if(sqlite3_step(groupStatement.get()) != SQLITE_DONE) return false;
                const sqlite3_int64 groupId = sqlite3_last_insert_rowid(database);
                for(size_t ordinal = 0; ordinal < members.size(); ordinal++) {
                    sqlite3_reset(memberStatement.get());
                    sqlite3_clear_bindings(memberStatement.get());
                    sqlite3_bind_int64(memberStatement.get(), 1, groupId);
                    sqlite3_bind_text(memberStatement.get(), 2, members[ordinal].c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(memberStatement.get(), 3, static_cast<sqlite3_int64>(ordinal));
                    if(sqlite3_step(memberStatement.get()) != SQLITE_DONE) return false;
                }
                return true;
            };
            ok = insertMetadata("format", "iwds-unification") && insertMetadata("schema_version", "4");
            for(size_t level = 0; level < _unifiableIdeoGroups.size() && ok; level++) {
                for(const UnifiableGroup& group: _unifiableIdeoGroups[level]) {
                    std::vector<std::string> members;
                    for(const Ideograph& glyph: group)
                        members.push_back(glyph.toString());
                    if(!insertGroup(level, members)) {
                        ok = false;
                        break;
                    }
                }
                for(const UnifiableIDSGroup& group: _unifiableIDSGroups[level]) {
                    if(!insertGroup(level, group)) {
                        ok = false;
                        break;
                    }
                }
            }
        }
    }
    if(ok)
        ok = SQLiteExecute(database, "COMMIT;");
    else
        SQLiteExecute(database, "ROLLBACK;");
    sqlite3_close(database);
    return ok;
}
