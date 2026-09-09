#include <algorithm>
#include <cstdio>
#include <fstream>

#include <iostream>
#include <sqlite3.h>
#include <vector>

#include "ids4c/ids4c.h"
#include "ids4c/idsdb.h"

namespace {

    bool HasIdeograph(const std::vector<Ideograph>& ideographs, const char* character) {
        return std::any_of(ideographs.begin(), ideographs.end(),
            [character](const Ideograph& ideograph) { return ideograph == Ideograph(character); });
    }

    int Fail(const char* message) {
        std::cerr << message << std::endl;
        return 1;
    }

    bool ExpectMatch(IDSdatabase& database, const char* queryText, const char* expectedCharacter) {
        IDSOwner query = ParseIDSOwned(queryText);
        if(query == nullptr) return false;
        return HasIdeograph(database.MatchQuery(query.get()), expectedCharacter);
    }

    bool ExpectMatchWithOptions(
        IDSdatabase& database, const char* queryText, const IDSqueryOptions& options, const char* expectedCharacter) {
        IDSOwner query = ParseIDSOwned(queryText);
        if(query == nullptr) return false;
        return HasIdeograph(database.MatchQuery(query.get(), options), expectedCharacter);
    }
    bool ExpectComponentSearch(IDSdatabase& database, const char* queryText, const char* expectedCharacter) {
        IDSOwnerList terms = ParseComponentSearchTerms(queryText);
        if(terms.empty()) return false;
        return HasIdeograph(database.Search(terms), expectedCharacter);
    }

} // namespace

int main() {
    IDSOwner basicStroke = ParseIDSOwned("#(S)");
    if(basicStroke == nullptr || basicStroke->GetType() != IDS_STROKE)
        return Fail("A standalone abstract stroke did not parse.");
    std::vector<std::string> localeOrder;
    if(!ParseLocaleSuffixFallbackOrder("C>G>.>H>T", localeOrder) ||
        localeOrder != std::vector<std::string>({"C", "G", "", "H", "T"}) ||
        ParseLocaleSuffixFallbackOrder("C>>G", localeOrder) ||
        !ParseLocaleSuffixFallbackOrder("C=G>.=H", localeOrder) ||
        localeOrder != std::vector<std::string>({"C=G", ".=H"}) ||
        ParseLocaleSuffixFallbackOrder("C==G", localeOrder) ||
        ParseLocaleSuffixFallbackOrder("C>G=C", localeOrder))
        return Fail("Locale suffix fallback order parsing did not validate or normalize correctly.");

    {
        std::ofstream source("db/parse-diagnostics.dat");
        source << u8"\u574F\t\u2FF1\u4E00\u4E00\t\u2FF0\u4E00\u4E8C)\n";
    }
    std::remove("db/parse-diagnostics.sqlite");
    {
        IDSdatabase diagnosticDatabase("parse-diagnostics");
        if(diagnosticDatabase.ImportDB("db/parse-diagnostics.dat") != 0)
            return Fail("A partially valid IDS import was unexpectedly rejected.");
        const IDSimportReport& report = diagnosticDatabase.GetLastImportReport();
        if(report.issues.size() != 1 || report.issues.front().line != 1 ||
            report.issues.front().idsIndex != 2 || report.issues.front().characterIndex != 4 ||
            report.issues.front().unexpectedCharacter != ")" ||
            report.issues.front().message != "unexpected ')'") {
            return Fail("IDS import diagnostics did not preserve line, expression, and character positions.");
        }
    }
    std::remove("db/parse-diagnostics.dat");
    std::remove("db/parse-diagnostics.sqlite");

    std::remove("db/custom-reader.sqlite");
    {
        IDSdatabase customReaderDatabase("custom-reader");
        IDSImportReader reader = [](const IDSImportRecordCallback& emit, std::string&) {
            IDSImportRecord record;
            record.line       = 12;
            record.idsIndex   = 1;
            record.glyphs     = {u8"\uE100", u8"\uE101"};
            record.expression = u8"\u2FF1\u4E00\u4E00";
            record.ids        = record.expression;
            return emit(record);
        };
        if(customReaderDatabase.ImportDB(reader) != 0 ||
            customReaderDatabase.GetRawIDSOwned(Ideograph(u8"\uE100")).empty() ||
            customReaderDatabase.GetRawIDSOwned(Ideograph(u8"\uE101")).empty())
            return Fail("A custom IDS reader did not import its emitted records.");
    }
    std::remove("db/custom-reader.sqlite");

    // 同一个字形的多个 IDS 定义必须各自独立匹配，不能把不同定义的部件拼接起来。
    std::remove("db/cross-ids.sqlite");

    {
        std::ofstream source("db/cross-ids.dat");
        source << u8"𳄼\t⿰𣁎糸\n";
        source << u8"𣁎\t⿰幺言\n";
        source << u8"𣁎\t⿰言幺\n";
    }
    {
        IDSdatabase crossIDSDatabase("cross-ids");
        if(crossIDSDatabase.ImportDB("db/cross-ids.dat") != 0)
            return Fail("The cross-IDS regression fixture could not be imported.");
        if(ExpectMatch(crossIDSDatabase, u8"<search=幺,言,幺>", u8"𳄼"))
            return Fail("Search terms were incorrectly combined across alternative IDS definitions.");
    }
    std::remove("db/cross-ids.dat");
    std::remove("db/cross-ids.sqlite");

    // A repeated variable may bind a contiguous HV expansion and later match
    // an ideograph that expands to the same directional sequence.
    std::remove("db/variable-hv.sqlite");
    {
        std::ofstream source("db/variable-hv.dat");
        source << u8"\uE150\t\u2FF3\U000200D7\u4E3F\u6728d\n";
        source << u8"\uE151\t\u2FF1\uE150\u2FF0\uE150\uE150\n";
    }
    {
        IDSdatabase variableHVDatabase("variable-hv");
        if(variableHVDatabase.ImportDB("db/variable-hv.dat") != 0)
            return Fail("The variable HV fixture could not be imported.");

        IDSOwner variableHVQuery = ParseIDSOwned(u8"\u2FF1<var=1>\u2FF0<var=1><var=1>");
        if(variableHVQuery == nullptr || !HasIdeograph(variableHVDatabase.MatchQuery(variableHVQuery.get()), u8"\uE151"))
            return Fail("A repeated variable did not match its contiguous HV expansion.");

        const std::vector<IDSMatchDetail> variableDetails = variableHVDatabase.MatchDetailed(variableHVQuery.get());
        const auto variableDetail = std::find_if(variableDetails.begin(), variableDetails.end(), [](const IDSMatchDetail& detail) {
            return detail.glyph == Ideograph(u8"\uE151");
        });
        const bool hasBoundRange = variableDetail != variableDetails.end() &&
            std::any_of(variableDetail->matchPaths.begin(), variableDetail->matchPaths.end(), [](const IDSMatchPath& path) {
                return path.queryPath == "/child[0:1]" && path.path == "/child[0:3]";
            });
        const bool hasRepeatedRange = variableDetail != variableDetails.end() &&
            std::any_of(variableDetail->matchPaths.begin(), variableDetail->matchPaths.end(), [](const IDSMatchPath& path) {
                return path.queryPath == "/child[1]/child[0:1]" && path.path == "/child[3]/child[0:1]";
            });
        if(!hasBoundRange || !hasRepeatedRange)
            return Fail("Variable HV matching did not expose the bound and repeated match paths.");
    }
    std::remove("db/variable-hv.dat");

    // A four-variable normalized arrangement must reject short candidates without
    // constructing an out-of-range child interval.
    IDSOwner shortVariableQuery = ParseIDSOwned(u8"\u2FF2\u2FF0<var=1><var=1><var=1><var=1>");
    IDSdatabase shortVariableDatabase("variable-hv");
    if(shortVariableQuery == nullptr || !shortVariableDatabase.MatchQuery(shortVariableQuery.get()).empty())
        return Fail("A short candidate crashed or matched a four-variable arrangement.");
    std::remove("db/variable-hv.sqlite");

    // Stroke-neutral composition cache: build 斤 from a lowercase stroke variant.
    std::remove("db/stroke-neutral.sqlite");
    {
        std::ofstream source("db/stroke-neutral.dat");
        source << u8"斤\t⿸𠂆s丅\n";
        source << u8"丘\t⿱⿸𠂆s丅一\n";
    }
    {
        IDSdatabase compositionDatabase("stroke-neutral");
        if(compositionDatabase.ImportDB("db/stroke-neutral.dat") != 0)
            return Fail("The stroke-neutral composition fixture could not be imported.");

        if(!ExpectMatch(compositionDatabase, u8"⿱斤一", u8"丘") ||
            !ExpectComponentSearch(compositionDatabase, u8"斤", u8"丘"))
            return Fail("The stroke-neutral composition cache did not support matching and search.");

        IDSOwner compositionQuery = ParseIDSOwned(u8"⿱斤一");
        if(compositionQuery == nullptr)
            return Fail("The stroke-neutral composition query did not parse.");
        const std::vector<IDSMatchDetail> details = compositionDatabase.MatchDetailed(compositionQuery.get());
        const auto detail = std::find_if(details.begin(), details.end(),
            [](const IDSMatchDetail& value) { return value.glyph == Ideograph(u8"丘"); });
        if(detail == details.end())
            return Fail("The stroke-neutral composition detail did not identify its cache source.");
    }
    {
        IDSdatabase reloadedCompositionDatabase("stroke-neutral");
        if(!ExpectMatch(reloadedCompositionDatabase, u8"⿱斤一", u8"丘"))
            return Fail("The persisted stroke-neutral composition cache did not reload.");
    }
    std::remove("db/stroke-neutral.dat");
    std::remove("db/stroke-neutral.sqlite");

    {
        std::ofstream source("db/yibai-stroke.dat");
        source << u8"\u4E28\t#(S)\n";
    }
    std::remove("db/yibai-stroke.sqlite");
    IDSdatabase yibaiStrokeDatabase("yibai-stroke");
    const int yibaiStrokeResult = yibaiStrokeDatabase.ImportDB("db/yibai-stroke.dat", IDSDB_YIBAI);
    const bool yibaiStrokeImported = yibaiStrokeResult == 0 && !yibaiStrokeDatabase.isEmpty();
    std::remove("db/yibai-stroke.dat");
    std::remove("db/yibai-stroke.sqlite");
    if(!yibaiStrokeImported)
        return Fail("A YiBai stroke expression did not import.");

    // YiBai 私有库可能使用单独的 CR 换行；相邻字形不能因此合并。
    std::remove("db/yibai-cr-private.sqlite");
    {
        std::ofstream base("db/yibai-cr-base.dat", std::ios::binary);
        base << u8"基\t⿰一一\n";
        std::ofstream privateSource("db/yibai-cr-private.dat", std::ios::binary);
        privateSource << u8"㩂C\t⿰扌斛.\t⿰捔.斗\r";
        privateSource << u8"㪱C\t⿰文.d奂\r";
        privateSource << u8"㫆C\t⿰方.尒.\t⿸㫃.小.\r";
    }
    {
        IDSdatabase crPrivateDatabase("yibai-cr-private");
        if(crPrivateDatabase.ImportDB("db/yibai-cr-base.dat") != 0 ||
            crPrivateDatabase.ImportPrivateDB("db/yibai-cr-private.dat", IDSDB_YIBAI) != 0)
            return Fail("A CR-separated private YiBai IDS library could not be imported.");
        const IDSOwnerList firstGlyph = crPrivateDatabase.GetRawIDSOwned(Ideograph(u8"㩂C"));
        const IDSOwnerList secondGlyph = crPrivateDatabase.GetRawIDSOwned(Ideograph(u8"㪱C"));
        const IDSOwnerList thirdGlyph = crPrivateDatabase.GetRawIDSOwned(Ideograph(u8"㫆C"));
        if(firstGlyph.size() != 2 || secondGlyph.size() != 1 || thirdGlyph.size() != 2 ||
            !ExpectMatch(crPrivateDatabase, u8"⿰文奂", u8"㪱C"))
            return Fail("CR-separated YiBai records were merged into the wrong C-suffixed glyph.");
    }
    std::remove("db/yibai-cr-base.dat");
    std::remove("db/yibai-cr-private.dat");
    std::remove("db/yibai-cr-private.sqlite");

    std::remove("db/locale-order.sqlite");
    {
        std::ofstream source("db/locale-order.dat");
        source << u8"甲C\t⿰一一\n";
        source << u8"甲G\t⿰一一\n";
        source << u8"甲\t⿰一一\n";
        source << u8"甲H\t⿰一一\n";
        source << u8"甲T\t⿰一一\n";
        source << u8"乙X\t⿰二二\n";
        source << u8"丁C\t⿰三三\n";
        source << u8"丁G\t⿰四四\n";
        source << u8"丙\t⿰四四\n";
        source << u8"丙J\t⿰一一\n";
        source << u8"乙T\t⿰二二\n";
    }
    {
        IDSdatabase localeDatabase("locale-order");
        if(localeDatabase.ImportDB("db/locale-order.dat", IDSDB_YIBAI) != 0)
            return Fail("The locale suffix fallback fixture could not be imported.");
        IDSOwner firstLocaleQuery = ParseIDSOwned(u8"⿰一一");
        IDSOwner secondLocaleQuery = ParseIDSOwned(u8"⿰二二");
        if(firstLocaleQuery == nullptr || secondLocaleQuery == nullptr)
            return Fail("The locale suffix fallback queries did not parse.");
        IDSqueryOptions localeOptions;
        localeOptions.filter.resultFilter = IDS_RESULT_IGNORE_OTHER_LOCALES;
        if(!ParseLocaleSuffixFallbackOrder("C>G>.>H>T", localeOptions.filter.localeSuffixFallbackOrder))
            return Fail("The locale suffix fallback test order did not parse.");
        const IDSOwnerList defaultIDS = localeDatabase.GetIDSOwned(Ideograph(u8"丁"));
        if(defaultIDS.size() != 2)
            return Fail("FindIDS changed its default suffix fallback behavior.");
        localeDatabase.config.fuzzyMatch.localeSuffixFallbackOrder = {"G", "C"};
        const IDSOwnerList preferredIDS = localeDatabase.GetIDSOwned(Ideograph(u8"丁"));
        if(preferredIDS.size() != 1 || preferredIDS.front()->toString() != u8"▥(四|四)")
            return Fail("FindIDS did not apply the configured suffix fallback order.");
        localeDatabase.config.fuzzyMatch.localeSuffixFallbackOrder = {"C=G"};
        const IDSOwnerList sameLevelIDS = localeDatabase.GetIDSOwned(Ideograph(u8"丁"));
        if(sameLevelIDS.size() != 2)
            return Fail("FindIDS did not try same-level suffixes together.");
        localeDatabase.config.fuzzyMatch.localeSuffixFallbackOrder = {"G", "C"};
        const std::vector<Ideograph> firstLocaleMatches =
            localeDatabase.MatchQuery(firstLocaleQuery.get(), localeOptions);
        const std::vector<Ideograph> secondLocaleMatches =
            localeDatabase.MatchQuery(secondLocaleQuery.get(), localeOptions);
        if(firstLocaleMatches.size() != 1 || !HasIdeograph(firstLocaleMatches, u8"甲C") ||
            secondLocaleMatches.size() != 1 || !HasIdeograph(secondLocaleMatches, u8"乙T"))
            return Fail("The configured locale suffix fallback order was not applied.");
        std::remove("db/locale-order.dat");
        std::remove("db/locale-order.sqlite");
        std::remove("db/yibai-compat.sqlite");
    }
    {
        std::ofstream source("db/yibai-compat.dat");
        source << u8"\u8863\t\u2FF1\u4EA0.\U00027607.(.)\n";
        source << u8"\u54C0\t\u2FF4\u8863.\u53E3.(.)\n";
        source << u8"\u887E\t\u2FF4\u8863.\u516C.(.)\n";
    }
    {
        IDSdatabase yibaiCompatibilityDatabase("yibai-compat");
        if(yibaiCompatibilityDatabase.ImportDB("db/yibai-compat.dat", IDSDB_YIBAI) != 0)
            return Fail("The YiBai yibai-compatibility fixture could not be imported.");
        std::remove("db/yibai-compat.dat");

        IDSOwner yibaiQuery = ParseIDSOwned(u8"\u2FF3\u4EA0\u2B1A\U00027607");
        if(yibaiQuery == nullptr)
            return Fail("The YiBai yibai-compatibility query did not parse.");
        const std::vector<Ideograph> yibaiMatches = yibaiCompatibilityDatabase.MatchQuery(yibaiQuery.get());
        if(!HasIdeograph(yibaiMatches, u8"\u54C0") || !HasIdeograph(yibaiMatches, u8"\u887E"))
            return Fail("The YiBai ⿳ABC compatibility query did not match ⿴衣B entries.");

        const std::vector<std::string> equivalentQueries =
            yibaiCompatibilityDatabase.GetEquivalentQueries(yibaiQuery.get());
        if(std::find(equivalentQueries.begin(), equivalentQueries.end(), u8"\u2FF4\u8863\u2B1A") ==
            equivalentQueries.end())
            return Fail("The YiBai compatibility query did not expose ⿴衣⬚ as an equivalent query.");
    }
    std::remove("db/yibai-compat.sqlite");
    std::remove("db/hv-origin.sqlite");
    {
        std::ofstream source("db/hv-origin.dat");
        source << u8"土\t⿱十一\n";
        source << u8"士\t{士}⿱十一\n";
        source << u8"吉\t⿱士口\n";
        source << u8"干\t⿱一十\n";
        source << u8"王\t⿱一土\t⿱干一\n";
        source << u8"丙\t{?0}⿱一二\n";
        source << u8"丁\t⿱一二\n";
        source << u8"㞷\t⿱屮王\n";
    }
    {
        IDSdatabase hvOriginDatabase("hv-origin");
        if(hvOriginDatabase.ImportDB("db/hv-origin.dat") != 0)
            return Fail("The HV origin-range fixture could not be imported.");
        IDSOwner sameIDSQuery = ParseIDSOwned(u8"⿱一二");
        if(sameIDSQuery == nullptr)
            return Fail("The same-IDS prefix fixture query did not parse.");
        const std::vector<Ideograph> sameIDSMatches = hvOriginDatabase.MatchQuery(sameIDSQuery.get());
        if(!HasIdeograph(sameIDSMatches, u8"丙") || !HasIdeograph(sameIDSMatches, u8"丁"))
            return Fail("{?0} incorrectly disabled same-IDS preprocessing.");

        const IDSOwnerList cachedJi = hvOriginDatabase.GetIDSOwned(Ideograph(u8"吉"));
        if(cachedJi.size() != 1 || cachedJi.front()->toString().find(u8"[士=0:2]") == std::string::npos)
            return Fail("HVExtract did not preserve the ambiguous source glyph range.");
        if(!ExpectMatch(hvOriginDatabase, u8"<search=士>", u8"吉"))
            return Fail("An HV origin range did not preserve matching of the original ambiguous glyph.");
        if(ExpectMatch(hvOriginDatabase, u8"<search=土>", u8"吉"))
            return Fail("An HV origin range still confused 士 with 土 after expansion.");
        const IDSOwnerList cachedHu = hvOriginDatabase.GetIDSOwned(Ideograph(u8"㞷"));
        if(cachedHu.empty()) return Fail("HVExtract did not produce any cached expansion for the ambiguous glyph.");
        if(ExpectMatch(hvOriginDatabase, u8"<search=士>", u8"㞷"))
            return Fail("A not-equivalent same-IDS source still matched its unique counterpart.");
        bool foundFlattenedHu = false;
        for(const IDSOwner& cached: cachedHu) {
            if(cached->toString().find(u8"十一") == std::string::npos) continue;
            foundFlattenedHu = true;
            if(cached->toString().find(u8"[土=2:4]") == std::string::npos)
                return Fail("An alternate HV path lost the ambiguous same-IDS source range.");
        }
        if(!foundFlattenedHu)
            return Fail("The HV origin fixture did not produce the alternate flattened path.");
        if(!ExpectMatch(hvOriginDatabase, u8"<search=土>", u8"㞷"))
            return Fail("The original ambiguous same-IDS source no longer matched its HV expansion.");
    }
    std::remove("db/hv-origin.dat");
    {
        IDSdatabase reloadedHVOriginDatabase("hv-origin");
        const IDSOwnerList cachedJi = reloadedHVOriginDatabase.GetIDSOwned(Ideograph(u8"吉"));
        if(cachedJi.size() != 1 || cachedJi.front()->toString().find(u8"[士=0:2]") == std::string::npos)
            return Fail("HV origin-range metadata did not survive SQLite reload.");
        if(ExpectMatch(reloadedHVOriginDatabase, u8"<search=土>", u8"吉"))
            return Fail("HV origin-range matching changed after SQLite reload.");
        const IDSOwnerList cachedHu = reloadedHVOriginDatabase.GetIDSOwned(Ideograph(u8"㞷"));
        bool foundReloadedFlattenedHu = false;
        for(const IDSOwner& cached: cachedHu) {
            if(cached->toString().find(u8"十一") == std::string::npos) continue;
            foundReloadedFlattenedHu = true;
            if(cached->toString().find(u8"[土=2:4]") == std::string::npos)
                return Fail("An alternate HV path lost its range after SQLite reload.");
        }
        if(!foundReloadedFlattenedHu)
            return Fail("The alternate flattened HV path did not survive SQLite reload.");
    }
    std::remove("db/hv-origin.sqlite");

    std::remove("db/replace-query.sqlite");
    {
        IDSdatabase importedDatabase("replace-query");
        if(importedDatabase.ImportDB("db/replace-query.dat") != 0)
            return Fail("The SQLite test database could not be imported.");
    }
    IDSdatabase database("replace-query");
    if(database.isEmpty()) return Fail("The SQLite test database did not reload.");

    sqlite3* sameExpressionDatabase = nullptr;
    if(sqlite3_open_v2("db/replace-query.sqlite", &sameExpressionDatabase, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return Fail("The SQLite database containing the strict IDS index could not be opened.");
    auto QueryParameterizedCount = [](sqlite3* databaseHandle, const char* sql, const char* parameter) {
        sqlite3_stmt* statement = nullptr;
        if(sqlite3_prepare_v2(databaseHandle, sql, -1, &statement, nullptr) != SQLITE_OK) return -1;
        if(parameter != nullptr)
            sqlite3_bind_text(statement, 1, parameter, -1, SQLITE_TRANSIENT);
        const int result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
        sqlite3_finalize(statement);
        return result;
    };
    const int sameExpressionRowCount = QueryParameterizedCount(
        sameExpressionDatabase, "SELECT COUNT(*) FROM ids_same_expression", nullptr);
    const int repeatedExpressionRowCount = QueryParameterizedCount(
        sameExpressionDatabase, "SELECT COUNT(*) FROM ids_same_expression WHERE ids_text = ?", u8"⿱一一");
    const int uniqueExpressionRowCount = QueryParameterizedCount(
        sameExpressionDatabase, "SELECT COUNT(*) FROM ids_same_expression WHERE ids_text = ?", "#(S)");
    sqlite3_close(sameExpressionDatabase);
    if(sameExpressionRowCount < 3 || repeatedExpressionRowCount < 3 || uniqueExpressionRowCount != 0)
        return Fail("The strict IDS hash table did not store only repeated raw expressions.");

    const std::vector<Ideograph> repeatedExpressionMatches = database.MatchQuery(ParseIDSOwned(u8"⿱一一").get());
    if(!HasIdeograph(repeatedExpressionMatches, u8"二") || !HasIdeograph(repeatedExpressionMatches, u8"亍") ||
        !HasIdeograph(repeatedExpressionMatches, u8"㐍"))
        return Fail("Strict IDS preprocessing did not return all glyphs sharing one raw expression.");

    std::remove("db/unifiable.sqlite");
    if(!database.ImportIWDSXml("db/iwds-srcseparation.xml"))
        return Fail("The IWDS SourceCodeSeparation fixture could not be imported.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_NONE;
    if(ExpectMatch(database, "⿰乙一", "由") || ExpectMatch(database, "⿰丙一", "由"))
        return Fail("IWDS unification leaked into strict matching.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_SOURCE_CODE_SEPARATION;
    if(!ExpectMatch(database, "⿰乙一", "由") || !ExpectMatch(database, "⿰丙一", "由"))
        return Fail("SourceCodeSeparation did not match direct and transitive groups.");
    IDSOwner sourceSeparationDetailQuery = ParseIDSOwned(u8"⿰乙一");
    if(sourceSeparationDetailQuery == nullptr)
        return Fail("The SourceCodeSeparation detail query did not parse.");
    const std::vector<IDSMatchDetail> sourceSeparationDetails =
        database.MatchDetailed(sourceSeparationDetailQuery.get());
    const auto detailForYou = std::find_if(sourceSeparationDetails.begin(), sourceSeparationDetails.end(),
        [](const IDSMatchDetail& detail) { return detail.glyph == Ideograph(u8"由"); });
    const bool hasSourceSeparationRule = detailForYou != sourceSeparationDetails.end() &&
        std::find(detailForYou->preprocessRules.begin(), detailForYou->preprocessRules.end(),
            IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION) != detailForYou->preprocessRules.end();
    if(!hasSourceSeparationRule)
        return Fail("Match details did not record SourceCodeSeparation as a preprocessing rule.");
    IDSOwner sourceSeparationSearchQuery = ParseIDSOwned(u8"<search=乙>");
    if(sourceSeparationSearchQuery == nullptr)
        return Fail("The SourceCodeSeparation search query did not parse.");
    const std::vector<IDSMatchDetail> sourceSeparationSearchDetails =
        database.MatchDetailed(sourceSeparationSearchQuery.get());
    const auto searchDetailForYou = std::find_if(sourceSeparationSearchDetails.begin(),
        sourceSeparationSearchDetails.end(), [](const IDSMatchDetail& detail) {
            return detail.glyph == Ideograph(u8"由");
        });
    const bool hasNestedSourceSeparationRule = searchDetailForYou != sourceSeparationSearchDetails.end() &&
        std::find(searchDetailForYou->preprocessRules.begin(), searchDetailForYou->preprocessRules.end(),
            IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION) != searchDetailForYou->preprocessRules.end();
    if(!hasNestedSourceSeparationRule)
        return Fail("Nested any/search preprocessing did not preserve SourceCodeSeparation.");
    IDSOwner exactSourceSeparationSearchQuery = ParseIDSOwned(u8"<search=甲>");
    if(exactSourceSeparationSearchQuery == nullptr)
        return Fail("The exact SourceCodeSeparation search query did not parse.");
    const std::vector<IDSMatchDetail> exactSourceSeparationDetails =
        database.MatchDetailed(exactSourceSeparationSearchQuery.get());
    const auto exactSearchDetailForYou = std::find_if(exactSourceSeparationDetails.begin(),
        exactSourceSeparationDetails.end(), [](const IDSMatchDetail& detail) {
            return detail.glyph == Ideograph(u8"由");
        });
    const bool exactBranchHasSourceSeparationRule = exactSearchDetailForYou != exactSourceSeparationDetails.end() &&
        std::find(exactSearchDetailForYou->preprocessRules.begin(), exactSearchDetailForYou->preprocessRules.end(),
            IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION) != exactSearchDetailForYou->preprocessRules.end();
    if(exactBranchHasSourceSeparationRule)
        return Fail("An exact any/search branch was incorrectly marked as SourceCodeSeparation.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_LV1;
    if(ExpectMatch(database, u8"\u2ff0\u6c34\u706b", u8"\uE005"))
        return Fail("A concrete lv1 query matched an unrelated structure.");
    IDSdatabase reloadedIWDSDatabase("replace-query");
    reloadedIWDSDatabase.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_SOURCE_CODE_SEPARATION;
    if(!ExpectMatch(reloadedIWDSDatabase, "⿰丙一", "由"))
        return Fail("The IWDS SourceCodeSeparation cache did not persist after SQLite reload.");
    sqlite3* unifiableSqlite = nullptr;
    if(sqlite3_open_v2("db/unifiable.sqlite", &unifiableSqlite, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return Fail("The IWDS unification database could not be opened.");
    auto QueryUnifiableCount = [unifiableSqlite](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if(sqlite3_prepare_v2(unifiableSqlite, sql, -1, &statement, nullptr) != SQLITE_OK) return -1;
        const int result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
        sqlite3_finalize(statement);
        return result;
    };
    const int unificationGroupCount =
        QueryUnifiableCount("SELECT COUNT(*) FROM unifiable_groups WHERE level = 1");
    const int componentGroupCount =
        QueryUnifiableCount("SELECT COUNT(*) FROM unifiable_groups WHERE level = 2");
    const int unificationMemberCount = QueryUnifiableCount("SELECT COUNT(*) FROM unifiable_group_members");
    const int legacyPairTableCount =
        QueryUnifiableCount("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'unifiable_pairs'");
    sqlite3_close(unifiableSqlite);
    if(unificationGroupCount != 1 || componentGroupCount != 1 || unificationMemberCount != 6 || legacyPairTableCount != 0)
        return Fail("IWDS unification was not stored as one group with its members.");
    if(!database.ImportIWDSXml("db/iwds-levels.xml"))
        return Fail("The IWDS level 1/2 fixture could not be imported.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_NONE;
    if(ExpectMatch(database, "⿰一一", "三") || ExpectMatch(database, "⿱一一", "三"))
        return Fail("IWDS component data leaked into strict matching.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_SOURCE_CODE_SEPARATION;
    if(ExpectMatch(database, "⿰一一", "三") || ExpectMatch(database, "⿱一一", "三"))
        return Fail("IWDS component data leaked into SourceCodeSeparation matching.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_LV1;
    if(!ExpectMatch(database, "⿰一一", "三") || ExpectMatch(database, "⿱一一", "三"))
        return Fail("IWDS lv1 did not match an IDS component equivalence.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_LV2;
    if(!ExpectMatch(database, "⿰一一", "三") || !ExpectMatch(database, "⿱一一", "三"))
        return Fail("IWDS lv2 did not inherit lv1 or apply level 2 components.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_NONE;
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_NONE;
    const IDSOwnerList rawHVFixture    = database.GetRawIDSOwned(Ideograph(u8"\u340C"));
    const IDSOwnerList cachedHVFixture = database.GetIDSOwned(Ideograph(u8"\u340C"));
    if(rawHVFixture.size() != 1 || cachedHVFixture.size() != 2)
        return Fail("The HV query cache did not preserve all alternatives of one raw IDS.");
    const IDSOwnerList extractedHVFixture = database.HVExtractOwned(rawHVFixture.front().get());
    if(extractedHVFixture.size() != 2) return Fail("HVExtractOwned did not expose every valid HV alternative.");
    const Ideograph privateGlyph(u8"\uE000");
    const Ideograph reimportedPrivateGlyph(u8"\uE001");
    const Ideograph invalidPrivateGlyph(u8"\uE002");
    const Ideograph affectedBaseGlyph(u8"\u3412");
    const IDSOwnerList cacheBeforePrivateImport = database.GetIDSOwned(affectedBaseGlyph);
    if(cacheBeforePrivateImport.size() != 1 ||
        cacheBeforePrivateImport.front()->toString() != u8"\u25A4(\uE000|\u7532)")
        return Fail("The private-extension base cache fixture did not load.");
    if(database.ImportPrivateDB("db/private-extension-invalid.dat") == 0 ||
        !database.GetRawIDSOwned(invalidPrivateGlyph).empty())
        return Fail("An invalid private IDS import changed the database.");

    if(database.ImportPrivateDB("db/private-extension.dat") != 0)
        return Fail("A valid private IDS library could not be imported.");
    if(database.GetLastImportReport().rebuiltCacheGlyphs != 2)
        return Fail("Private IDS import rebuilt an unexpected number of HV cache entries.");
    const IDSOwnerList privateRaw = database.GetRawIDSOwned(privateGlyph);
    const IDSOwnerList privateCache = database.GetIDSOwned(privateGlyph);
    const IDSOwnerList affectedCache = database.GetIDSOwned(affectedBaseGlyph);
    if(privateRaw.size() != 1 || privateRaw.front()->toString() != u8"\u2FF1\u4E00\u4E59" ||
        privateCache.size() != 1 || privateCache.front()->toString() != u8"\u25A4(\u4E00|\u4E59)" ||
        affectedCache.size() != 1 || affectedCache.front()->toString() != u8"\u25A4(\u4E00\u4E59|\u7532)")
        return Fail("Private IDS import did not update raw IDS and affected HV cache entries.");
    if(!ExpectMatch(database, u8"\u2FF3\u4E00\u4E59\u7532", u8"\u3412"))
        return Fail("A base glyph did not use its private component's rebuilt HV cache.");

    if(database.ReimportPrivateDB("db/private-extension-reimport.dat") != 0)
        return Fail("The private IDS library could not be reimported.");
    const IDSOwnerList revertedCache = database.GetIDSOwned(affectedBaseGlyph);
    if(!database.GetRawIDSOwned(privateGlyph).empty() ||
        database.GetRawIDSOwned(reimportedPrivateGlyph).size() != 1 ||
        revertedCache.size() != 1 || revertedCache.front()->toString() != u8"\u25A4(\uE000|\u7532)")
        return Fail("Reimporting a private IDS library did not replace old private entries and caches.");

    IDSdatabase reloadedPrivateDatabase("replace-query");
    if(reloadedPrivateDatabase.isEmpty() || !reloadedPrivateDatabase.GetRawIDSOwned(privateGlyph).empty() ||
        reloadedPrivateDatabase.GetRawIDSOwned(reimportedPrivateGlyph).size() != 1 ||
        !ExpectMatch(reloadedPrivateDatabase, u8"\u2FF0\u7532\u4E59", u8"\uE001"))
        return Fail("Private IDS metadata or cache did not persist after SQLite reload.");

    if(database.ImportPrivateDB("db/iwds-variable-extension.dat") != 0)
        return Fail("The IWDS variable matching fixture could not be imported.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_LV1;
    if(!ExpectMatch(database, u8"⿰礼目", u8"祺") || !ExpectMatch(database, u8"⿱礼目", u8"\uE00A"))
        return Fail("IWDS lv1 did not perform the requested ⿰/⿱ structure transformation.");
    if(!ExpectMatch(database, u8"⿺礼目", u8"祺") || !ExpectMatch(database, u8"⿸礼目", u8"\uE00A"))
        return Fail("IWDS lv1 did not perform the reverse enclosure structure transformation.");
    if(!ExpectMatch(database, u8"<search=⿱龴田>", u8"\uE00B"))
        return Fail("A fixed IDS search term did not match its indexed component.");
    if(ExpectMatch(database, u8"\u2ff0\u6c34\u706b", u8"\uE005"))
        return Fail("IWDS lv1 variables were not substituted according to the concrete query.");
    if(ExpectMatch(database, u8"⿱⿰<var=a>隹灬", u8"雷"))
        return Fail("A variable query was widened by an unrelated lv1 structure template.");
    if(ExpectMatch(database, u8"⿱⿰⬚隹灬", u8"雷"))
        return Fail("A wildcard query was widened by an unrelated lv1 structure template.");
    if(!ExpectMatch(database, u8"⿱⿰<var=a>隹灬", u8"\U0002448F") ||
        !ExpectMatch(database, u8"⿱⿰⬚隹灬", u8"\U0002448F"))
        return Fail("Variable and wildcard queries did not match the same expanded component.");
    IDSOwner nestedGlyphQuery = ParseIDSOwned(u8"⿱睢灬");
    if(nestedGlyphQuery == nullptr)
        return Fail("The concrete nested IWDS query did not parse.");
    const std::vector<Ideograph> nestedGlyphMatches = database.MatchQuery(nestedGlyphQuery.get());
    if(nestedGlyphMatches.size() != 1 || !HasIdeograph(nestedGlyphMatches, u8"\u77a7"))
        return Fail("A concrete nested IWDS query did not remain constrained to its concrete equivalent.");
    if(!ExpectMatch(database, u8"⿱⿰⬚隹灬", u8"\uE008") ||
        !ExpectMatch(database, u8"⿱⿰<var=a>隹灬", u8"\uE008") ||
        !ExpectMatch(database, u8"⿱⿰⬚隹灬", u8"\uE009"))
        return Fail("The nested IWDS variable template did not match its intended structures.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_LV2;
    IDSOwner combinedStructureQuery = ParseIDSOwned(u8"⿱⬚一");
    if(combinedStructureQuery == nullptr)
        return Fail("The combined IWDS structure query did not parse.");
    const std::vector<std::string> combinedStructureEquivalents =
        database.GetEquivalentQueries(combinedStructureQuery.get());
    if(std::find(combinedStructureEquivalents.begin(), combinedStructureEquivalents.end(), u8"⿸⬚二") ==
        combinedStructureEquivalents.end())
        return Fail("IWDS lv2 did not combine structure and component alternatives.");
    if(!ExpectMatch(database, u8"⿰礼目", u8"祺") || !ExpectMatch(database, u8"⿱礼目", u8"\uE00A"))
        return Fail("IWDS lv2 did not preserve the requested ⿰/⿱ structure transformation.");
    if(!ExpectMatch(database, u8"⿺礼目", u8"祺") || !ExpectMatch(database, u8"⿸礼目", u8"\uE00A"))
        return Fail("IWDS lv2 did not preserve the reverse enclosure structure transformation.");
    if(ExpectMatch(database, u8"\u2ff0\u4e8c\u6c34", u8"\uE003") ||
        ExpectMatch(database, u8"\u2ff0\u4e8c\u6c34", u8"\uE004") ||
        !ExpectMatch(database, u8"\u2ff0\u4e8c\u2b1a", u8"\uE003") ||
        !ExpectMatch(database, u8"\u2ff0\u4e8c\u2b1a", u8"\uE004") ||
        ExpectMatch(database, u8"\u2ff0\u6c34\u706b", u8"\uE005") ||
        ExpectMatch(database, u8"\u2ff0\u6c34\u706b", u8"\uE004") ||
        !ExpectMatch(database, u8"\u2ff0\u2b1a\u2b1a", u8"\uE005") ||
        !ExpectMatch(database, u8"\u2ff0\u2b1a\u2b1a", u8"\uE004") ||
        !ExpectMatch(database, u8"\u2ff1\u96e8\u2ff0\u2b1a\u2b1a", u8"\uE006") ||
        !ExpectMatch(database, u8"\u2ff1\u96e8\u2ff0\u2b1a\u2b1a", u8"\uE007"))
        return Fail("IWDS lv2 variables did not preserve concrete substitutions.");
    const std::vector<Ideograph> nestedGlyphMatchesLv2 = database.MatchQuery(nestedGlyphQuery.get());
    if(nestedGlyphMatchesLv2.size() != 1 || !HasIdeograph(nestedGlyphMatchesLv2, u8"\u77a7"))
        return Fail("The lv2 concrete nested IWDS query became unconstrained.");
    IDSOwner groupedSearchQuery = ParseIDSOwned(u8"<search=\u2FF1\u4E8C\u4E00>");
    if(groupedSearchQuery == nullptr)
        return Fail("The grouped search query did not parse.");
    const std::vector<std::string> equivalentSearchQueries =
        database.GetEquivalentQueries(groupedSearchQuery.get());
    bool hasNestedAny = false;
    for(const std::string& equivalentQuery: equivalentSearchQueries)
        if(equivalentQuery.find("<any=") != std::string::npos) hasNestedAny = true;
    if(equivalentSearchQueries.size() != 1 || !hasNestedAny)
        return Fail("IWDS search preprocessing did not group alternatives in a nested any expression.");
    database.config.fuzzyMatch.unificationLevel = IWDS_UNIFICATION_NONE;
    IDSOwner repeatedVariable = ParseIDSOwned(u8"\u2FF1<var=part_1><var=part_1>");
    if(repeatedVariable == nullptr || repeatedVariable->toString() != u8"\u2FF1<var=part_1><var=part_1>")
        return Fail("A repeated IDS variable did not parse or round-trip.");
    if(!HasIdeograph(database.MatchQuery(repeatedVariable.get()), u8"\u340D") ||
        HasIdeograph(database.MatchQuery(repeatedVariable.get()), u8"\u340F"))
        return Fail("A repeated IDS variable did not retain the first subtree binding.");
    if(!ExpectMatch(database, u8"\u2FF1<var=left><var=right>", u8"\u340F"))
        return Fail("Distinct IDS variables did not bind independently.");
    if(!ExpectMatch(database, u8"<search=\u2FF1<var=part><var=part>>", u8"\u340D") ||
        ExpectMatch(database, u8"<search=\u2FF1<var=part><var=part>>", u8"\u340F"))
        return Fail("IDS variables did not retain bindings inside a search expression.");
    if(!ExpectMatch(database, u8"\u2FF0<var=suffix><var=suffix>", u8"\u3410"))
        return Fail("IDS variables did not ignore equivalent glyph suffixes.");
    if(!ExpectMatch(database, u8"\u2FF0<var=component><var=component>", u8"\U0002020C"))
        return Fail("IDS variables did not match the U+2020C suffix variants.");
    if(ParseIDSOwned("<var=>") != nullptr || ParseIDSOwned("<var=a!>") != nullptr ||
        ParseIDSOwned(u8"<var=\uFF21>") != nullptr)
        return Fail("An IDS variable accepted an invalid or fullwidth identifier.");
    const char* middleSearch = u8"<search=\u4E2D>";
    if(!ExpectMatch(database, middleSearch, u8"\u3402") || !ExpectMatch(database, middleSearch, u8"\u3403"))
        return Fail("The overlay query fixtures did not match before filtering.");
    IDSqueryOptions ignoreOverlay;
    ignoreOverlay.filter.ignoreOverlayStructure = true;
    IDSqueryOptions extA;
    extA.filter.glyphDomain  = IDS_GLYPH_DOMAIN_UNICODE;
    extA.filter.unicodeBlocks = {IDS_UNICODE_BLOCK_CJK_EXT_A};
    if(!ExpectMatchWithOptions(database, middleSearch, extA, u8"㐂") ||
        ExpectMatchWithOptions(database, middleSearch, extA, u8"勒"))
        return Fail("Unicode block filtering did not retain only CJK Extension A results.");
    IDSqueryOptions multipleBlocks;
    multipleBlocks.filter.glyphDomain = IDS_GLYPH_DOMAIN_UNICODE;
    multipleBlocks.filter.unicodeBlocks = {IDS_UNICODE_BLOCK_CJK_EXT_A, IDS_UNICODE_BLOCK_CJK_BASIC};
    if(!ExpectMatchWithOptions(database, middleSearch, multipleBlocks, u8"㐂") ||
        !ExpectMatchWithOptions(database, middleSearch, multipleBlocks, u8"勒"))
        return Fail("Multiple Unicode block filtering did not union selected blocks.");
    IDSCustomGlyphRange dictionaryRange;
    dictionaryRange.name = "dictionary-sample";
    dictionaryRange.exactGlyphs = {u8"㐂"};
    if(!database.RegisterGlyphRange(dictionaryRange)) return Fail("A custom glyph range could not be registered.");
    IDSqueryOptions dictionaryFilter;
    dictionaryFilter.filter.customRanges = {"dictionary-sample"};
    if(!ExpectMatchWithOptions(database, middleSearch, dictionaryFilter, u8"㐂") ||
        ExpectMatchWithOptions(database, middleSearch, dictionaryFilter, u8"勒"))
        return Fail("A registered custom glyph range did not filter results.");
    IDSCustomGlyphRange suffixRange;
    suffixRange.name = "lowercase-suffix";
    suffixRange.suffixes = {"a"};
    if(!database.RegisterGlyphRange(suffixRange)) return Fail("A suffix-only glyph range could not be registered.");
    IDSqueryOptions suffixFilter;
    suffixFilter.filter.customRanges = {"lowercase-suffix"};
    if(!ExpectMatchWithOptions(database, middleSearch, suffixFilter, u8"㐀a"))
        return Fail("A suffix-list custom glyph range did not match the expected glyph.");
    IDSqueryOptions privateGlyphs;
    privateGlyphs.filter.glyphDomain = IDS_GLYPH_DOMAIN_PRIVATE;
    IDSqueryOptions unicodeGlyphs;
    unicodeGlyphs.filter.glyphDomain = IDS_GLYPH_DOMAIN_UNICODE;
    if(!ExpectMatchWithOptions(database, u8"⿰二一", privateGlyphs, u8"\uE003") ||
        ExpectMatchWithOptions(database, u8"⿰二一", unicodeGlyphs, u8"\uE003"))
        return Fail("Unicode/private glyph-domain filtering did not separate PUA results.");
    if(ExpectMatchWithOptions(database, middleSearch, ignoreOverlay, u8"\u3402") ||
        !ExpectMatchWithOptions(database, middleSearch, ignoreOverlay, u8"\u3403") ||
        !ExpectMatchWithOptions(database, middleSearch, ignoreOverlay, u8"\u3404"))
        return Fail("Overlay filtering did not skip only overlay IDS entries.");
    if(!ExpectMatch(database, middleSearch, u8"\u52D2") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreOverlay, u8"\u52D2"))
        return Fail("Overlay filtering did not reject a match that required recursive component expansion.");

    IDSqueryOptions ignoreLcSuffix;
    ignoreLcSuffix.filter.resultFilter = IDS_RESULT_IGNORE_LC_SUFFIX;
    if(!ExpectMatchWithOptions(database, middleSearch, ignoreLcSuffix, u8"\u3400") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreLcSuffix, u8"\u3400a") ||
        !ExpectMatchWithOptions(database, middleSearch, ignoreLcSuffix, u8"\u3400B") ||
        !ExpectMatchWithOptions(database, middleSearch, ignoreLcSuffix, u8"\u3400C"))
        return Fail("Lowercase suffix filtering did not run in the database matcher.");

    IDSqueryOptions ignoreOtherLocales;
    ignoreOtherLocales.filter.resultFilter = IDS_RESULT_IGNORE_OTHER_LOCALES;
    if(!ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3400") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3400a") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3400B") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3400C") ||
        !ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3401B") ||
        ExpectMatchWithOptions(database, middleSearch, ignoreOtherLocales, u8"\u3401C"))
        return Fail("Locale filtering did not use the cached base-glyph table.");
    if(!ExpectMatch(database, "⿰礻⬚", "祺"))
        return Fail("An enclosing radical did not expand for an open horizontal query.");
    if(ExpectMatch(database, "⿰⬚目", "祺"))
        return Fail("An enclosing radical matched a constrained horizontal query.");
    if(!ExpectMatch(database, u8"<search=㇯谁讠>", u8"隹"))
        return Fail("A subtract query nested inside <search=...> did not reduce to 隹.");

    IDSOwner subtractQuery = ParseIDSOwned(u8"㇯㐌五");
    if(subtractQuery == nullptr || subtractQuery->toString() != u8"㇯㐌五")
        return Fail("A subtract query did not parse.");
    if(!ExpectMatch(database, u8"㇯㐌五", u8"㐋")) return Fail("A subtract query did not match the reduced result.");

    if(!ExpectMatch(database, u8"⿰甲⬚", u8"由")) return Fail("A forced-HV left-right query did not match 由.");
    if(!ExpectMatch(database, u8"⿱二⬚", u8"三")) return Fail("A forced-HV above-below query did not match 三.");
    if(!ExpectMatch(database, u8"⿱⬚合", u8"㐑") || !ExpectMatch(database, u8"⿱不合", u8"㐑"))
        return Fail("A vertical query did not match a suffix-ambiguous expanded component.");
    if(!ExpectMatch(database, u8"⿲一二⬚", u8"横"))
        return Fail("A forced-HV left-middle-right query did not match 横.");
    if(!ExpectMatch(database, u8"⿳一二⬚", u8"竖"))
        return Fail("A forced-HV above-middle-below query did not match 竖.");
    IDSOwner suffixVariantQuery = ParseIDSOwned(u8"\u2FF1\u96E8\u2FFA\u9B3C\u2B1A");
    if(suffixVariantQuery == nullptr) return Fail("A suffix-variant query did not parse.");
    IDSOwnerList unpreservedSuffixVariants = database.HVExtractOwned(suffixVariantQuery.get());
    if(unpreservedSuffixVariants.empty()) return Fail("The suffix-variant query did not produce an HV candidate.");
    for(const auto& variant: unpreservedSuffixVariants)
        if(HasIdeograph(database.Match(variant.get()), u8"\u96F7"))
            return Fail("The suffix-variant regression fixture did not require ambiguous-glyph preservation.");
    if(!ExpectMatch(database, u8"\u2FF1\u96E8\u2FFA\u9B3C\u2B1A", u8"\u96F7"))
        return Fail("A suffixless glyph did not match its suffix variant in an HV query.");
    IDSOwner detailQuery = ParseIDSOwned(u8"⿱一一");
    if(detailQuery == nullptr)
        return Fail("The detail query did not parse.");
    const std::vector<IDSMatchDetail> details = database.MatchDetailed(detailQuery.get());
    const auto detailForEr = std::find_if(details.begin(), details.end(), [](const IDSMatchDetail& detail) {
        return detail.glyph == Ideograph(u8"二");
    });
    if(detailForEr == details.end() || detailForEr->matchedIDS.empty() || detailForEr->rawIDS.empty() ||
        detailForEr->matchPaths.empty())
        return Fail("Match details did not preserve the matched and raw IDS information.");

    IDSqueryOptions noPathTracking;
    noPathTracking.trackMatchPaths = false;
    const std::vector<IDSMatchDetail> noPathDetails = database.MatchDetailed(detailQuery.get(), noPathTracking);
    const auto noPathDetailForEr = std::find_if(noPathDetails.begin(), noPathDetails.end(), [](const IDSMatchDetail& detail) {
        return detail.glyph == Ideograph(u8"二");
    });
    if(noPathDetailForEr == noPathDetails.end() || !noPathDetailForEr->matchPaths.empty())
        return Fail("Disabling match-path tracking did not suppress structured paths.");

    IDSOwner indexedPathQuery = ParseIDSOwned(u8"⿰<any=一,中>一");
    if(indexedPathQuery == nullptr)
        return Fail("The indexed match-path query did not parse.");
    const std::vector<IDSMatchDetail> indexedPathDetails = database.MatchDetailed(indexedPathQuery.get());
    const auto detailForZao = std::find_if(indexedPathDetails.begin(), indexedPathDetails.end(),
        [](const IDSMatchDetail& detail) { return detail.glyph == Ideograph(u8"㐃"); });
    if(detailForZao == indexedPathDetails.end() || detailForZao->matchPaths.size() != 2)
        return Fail("Equivalent match paths were not deduplicated.");
    for(const IDSMatchPath& path: detailForZao->matchPaths)
        if(path.queryExpressionIndex != 0)
            return Fail("A deduplicated match path did not reference equivalent_syntax.");
    for(size_t left = 0; left < detailForZao->matchPaths.size(); left++)
        for(size_t right = left + 1; right < detailForZao->matchPaths.size(); right++) {
            const IDSMatchPath& first  = detailForZao->matchPaths[left];
            const IDSMatchPath& second = detailForZao->matchPaths[right];
            if(first.kind == second.kind && first.index == second.index && first.queryPath == second.queryPath &&
                first.path == second.path)
                return Fail("matchPaths still contains duplicate semantic paths.");
        }
    if(!HasIdeograph(database.GetSameIDSCharacters(Ideograph(u8"二")), u8"亍") ||
        !HasIdeograph(database.GetContainingCharacters(Ideograph(u8"一")), u8"二"))
        return Fail("Strict same-expression or reverse component lookup did not return expected glyphs.");
    IDSOwner hvRangeQuery = ParseIDSOwned(u8"⿱⬚合");
    if(hvRangeQuery == nullptr)
        return Fail("The HV range query did not parse.");
    const std::vector<IDSMatchDetail> hvRangeDetails = database.MatchDetailed(hvRangeQuery.get());
    const auto detailForQumu = std::find_if(hvRangeDetails.begin(), hvRangeDetails.end(), [](const IDSMatchDetail& detail) {
        return detail.glyph == Ideograph(u8"㐑");
    });
    const bool hasHeRange = detailForQumu != hvRangeDetails.end() &&
        std::any_of(detailForQumu->matchPaths.begin(), detailForQumu->matchPaths.end(), [](const IDSMatchPath& path) {
            return path.kind == IDS_MATCH_PATH_NODE && path.queryPath == "/child[1:2]" && path.path == "/child[1:3]";
        });
    if(!hasHeRange)
        return Fail("HVExtract component matching did not expose the concrete replacement range in matchPaths.");
    if(ParseIDSOwned(u8"⿰贝⬚") == nullptr) return Fail("A normal open IDS query no longer parsed.");
    IDSOwner shortSearch = ParseIDSOwned(u8"<search=⺆,二>");
    if(shortSearch == nullptr || shortSearch->toString() != u8"<search=⺆,二>")
        return Fail("An unquoted unified search expression did not parse or round-trip.");
    IDSOwner quotedSearch = ParseIDSOwned(u8"<search=\"⿻[a,b]一二\",'三'>");
    if(quotedSearch == nullptr || quotedSearch->toString() != u8"<search=\"⿻[a,b]一二\",三>")
        return Fail("Quoted unified search terms did not parse or round-trip.");
    IDSOwner anySearch = ParseIDSOwned(u8"<any=\u4E00,\u4E8C>");
    if(anySearch == nullptr || anySearch->toString() != u8"<any=\u4E00,\u4E8C>")
        return Fail("An any expression did not parse or round-trip.");
    if(ParseIDSOwned(u8"<any=\u4E00,<residue=1>>") != nullptr ||
        ParseIDSOwned(u8"<any=\u2FF1\u4E00<residue=1>,\u4E8C>") != nullptr)
        return Fail("An any expression accepted a residue condition.");
    if(!ExpectMatch(database, "<stroke=2>", "二")) return Fail("An exact stroke-count IDS query did not match 二.");
    if(!ExpectMatch(database, "⿱<stroke=1><stroke=1>", "二"))
        return Fail("A nested stroke-count IDS query did not match 二.");
    if(!ExpectMatch(database, "<stroke=1>", "〇"))
        return Fail("Continuous curve fragments were not counted as one stroke.");
    if(!ExpectMatch(database, "<stroke=1>", "字") || ExpectMatch(database, "<stroke=2>", "字"))
        return Fail("A pure letter stroke expression was not counted as one stroke.");
    IDSOwner negatedStroke = ParseIDSOwned(u8"#(-\U000200CC\u4E59)");
    if(negatedStroke == nullptr || negatedStroke->toString() != u8"#(-\U000200CC\u4E59)")
        return Fail("A negated non-ASCII stroke token did not round-trip.");
    if(!ExpectMatch(database, "<stroke=2>", u8"\u353E"))
        return Fail("The stroke count for U+353E was not calculated as two.");
    if(!ExpectMatch(database, "<stroke=1>", u8"\u66F2") || ExpectMatch(database, "<stroke=2>", u8"\u66F2"))
        return Fail("Curve fragments did not connect adjacent stroke segments.");
    if(!ExpectMatch(database, "<stroke=2>", u8"\u65AD"))
        return Fail("A break marker did not separate curve-connected stroke segments.");
    if(!(Ideograph(u8"\u7532") == Ideograph(u8"\u7532.")) || !ExpectMatch(database, "<stroke=2>", u8"\u7531"))
        return Fail("A standalone dot suffix was not normalized to the base glyph for stroke counting.");
    if(!ExpectComponentSearch(database, "<stroke=1>", "多") || !ExpectComponentSearch(database, "<stroke=2>", "多"))
        return Fail("A glyph with multiple IDS values did not retain all stroke counts.");
    if(!ExpectComponentSearch(database, "<stroke=2>", "二"))
        return Fail("An exact component stroke query did not match 二.");
    if(!ExpectMatch(database, u8"<search=\u4E8C,<residue=0>>", u8"\u4E8C") ||
        !ExpectMatch(database, u8"<search=\u4E8C,<residue=0>>", u8"\u4E8D"))
        return Fail("A zero-residue glyph search did not match identical IDS entries.");
    if(ExpectMatch(database, u8"<search=\u4E8C,<residue=2>>", u8"\u4E8C") ||
        ExpectMatch(database, u8"<search=\u4E8C,<residue=1>>", u8"\u4E8C"))
        return Fail("A whole-glyph match incorrectly retained its own strokes as residue.");
    if(!ExpectComponentSearch(database, u8"\u4E00<residue=1>", u8"\u4E8C"))
        return Fail("A residue query did not count the remaining component strokes.");
    if(!ExpectComponentSearch(database, u8"\u4E00<residue=2>", u8"\u4E09"))
        return Fail("A residue query did not ignore the remaining IDS structure.");
    if(!ParseComponentSearchTerms(u8"\u4E00<residue=1><residue=2>").empty())
        return Fail("A component search accepted multiple residue conditions.");
    if(!ExpectComponentSearch(database, "<stroke=-2>", "一"))
        return Fail("An upper-bounded component stroke query did not match 一.");
    if(!ExpectComponentSearch(database, "<stroke=2->", "三"))
        return Fail("A lower-bounded component stroke query did not match 三.");
    if(!ExpectComponentSearch(database, "<stroke=2-3>", "二"))
        return Fail("A ranged component stroke query did not match 二.");
    // ~N 在解析阶段归一化为以中心值为基准的闭区间。
    IDSOwner strokeDelta = ParseIDSOwned("<stroke=21~1>");
    IDSOwner residueDelta = ParseIDSOwned("<residue=12~2>");
    if(strokeDelta == nullptr || strokeDelta->toString() != "<stroke=20-22>" || residueDelta == nullptr ||
        residueDelta->toString() != "<residue=10-14>" || ParseIDSOwned("<stroke=21~1-2>") != nullptr)
        return Fail("Stroke and residue delta syntax was not normalized or validated correctly.");
    if(!ExpectComponentSearch(database, "<stroke=2~1>", "一") ||
        !ExpectComponentSearch(database, "<stroke=2~1>", "二"))
        return Fail("A stroke delta query did not match the normalized interval.");
    if(!ExpectComponentSearch(database, "一<stroke=2>", "二"))
        return Fail("A mixed component and stroke query did not stop after all terms matched.");

    if(!ExpectComponentSearch(database, u8"一二", u8"三"))
        return Fail("A legacy two-component search did not match 三.");
    if(!ExpectMatch(database, u8"<search=一,二>", u8"三"))
        return Fail("A top-level unified search expression did not match 三.");
    IDSOwner repeatedSearch = ParseIDSOwned(u8"<search=一,一>");
    if(repeatedSearch == nullptr) return Fail("The repeated-component search query did not parse.");
    const std::vector<IDSMatchDetail> repeatedDetails = database.MatchDetailed(repeatedSearch.get());
    const auto detailForErRepeated = std::find_if(repeatedDetails.begin(), repeatedDetails.end(), [](const IDSMatchDetail& detail) {
        return detail.glyph == Ideograph(u8"二");
    });
    const bool hasFirstRepeatedPath = detailForErRepeated != repeatedDetails.end() &&
        std::any_of(detailForErRepeated->matchPaths.begin(), detailForErRepeated->matchPaths.end(), [](const IDSMatchPath& path) {
            return path.kind == IDS_MATCH_PATH_ALL_TERM && path.index == 0 && path.path == "/child[0]";
        });
    const bool hasSecondRepeatedPath = detailForErRepeated != repeatedDetails.end() &&
        std::any_of(detailForErRepeated->matchPaths.begin(), detailForErRepeated->matchPaths.end(), [](const IDSMatchPath& path) {
            return path.kind == IDS_MATCH_PATH_ALL_TERM && path.index == 1 && path.path == "/child[1]";
        });
    if(!hasFirstRepeatedPath || !hasSecondRepeatedPath)
        return Fail("Repeated search terms did not receive distinct match paths.");
    if(ExpectMatch(database, u8"<search=一,一,一>", u8"二"))
        return Fail("Repeated search terms incorrectly reused one matched component.");
    IDSOwner exceptSearch = ParseIDSOwned(u8"<search=\u7532,<except=\u4E59,\u4E19>>");
    if(exceptSearch == nullptr || exceptSearch->toString() != u8"<search=\u7532,<except=\u4E59,\u4E19>>")
        return Fail("An except expression did not parse or round-trip.");
    IDSOwner rootExcept = ParseIDSOwned(u8"<except=\u4E59>");
    if(rootExcept == nullptr || rootExcept->toString() != u8"<except=\u4E59>")
        return Fail("A root exception did not parse or round-trip.");
    if(ParseIDSOwned(u8"<search=\u7532,<except=\u4E59>,<except=\u4E19>>") != nullptr ||
        ParseIDSOwned(u8"<search=<except=\u4E59>>") != nullptr ||
        ParseIDSOwned(u8"<any=\u7532,<except=<residue=1>>>") != nullptr ||
        ParseIDSOwned(u8"\u2FF0<except=\u7532>\u4E59") != nullptr)
        return Fail("An except expression was accepted outside its allowed search context.");
    if(!ExpectMatch(database, u8"<search=\u7532,<except=\u4E59>>", u8"\u3408") ||
        ExpectMatch(database, u8"<search=\u7532,<except=\u4E59>>", u8"\u3407"))
        return Fail("A search exception did not exclude matching component terms.");
    if(!ExpectMatch(database, u8"<except=\u4E59>", u8"\u3408") ||
        ExpectMatch(database, u8"<except=\u4E59>", u8"\u3407") ||
        ExpectMatch(database, u8"<except=\u4E59>", u8"\u3409"))
        return Fail("A root exception did not exclude matching ideographs across all IDS entries.");
    if(!ExpectMatch(database, u8"<search=\u4E59,\u4E19>", u8"\u340A"))
        return Fail("The conjunction exception fixture did not load.");
    if(ExpectMatch(database, u8"<search=\u7532,<except=\u4E59,\u4E19>>", u8"\u3407") ||
        ExpectMatch(database, u8"<search=\u7532,<except=\u4E59,\u4E19>>", u8"\u3408") ||
        ExpectMatch(database, u8"<search=\u7532,<except=\u4E59,\u4E19>>", u8"\u3409") ||
        ExpectMatch(database, u8"<search=\u7532,<except=\u4E59,\u4E19>>", u8"\u340A"))
        return Fail("Direct exception terms were not treated as alternatives.");
    if(!ExpectMatch(database, u8"<search=\u7532,<except=<search=\u4E59,\u4E19>>>", u8"\u3407") ||
        !ExpectMatch(database, u8"<search=\u7532,<except=<search=\u4E59,\u4E19>>>", u8"\u3408") ||
        !ExpectMatch(database, u8"<search=\u7532,<except=<search=\u4E59,\u4E19>>>", u8"\u3409") ||
        ExpectMatch(database, u8"<search=\u7532,<except=<search=\u4E59,\u4E19>>>", u8"\u340A"))
        return Fail("A nested search exception did not require every component.");
    if(!ExpectMatch(database, u8"<except=<search=\u4E59,\u4E19>>", u8"\u3407") ||
        !ExpectMatch(database, u8"<except=<search=\u4E59,\u4E19>>", u8"\u3408") ||
        !ExpectMatch(database, u8"<except=<search=\u4E59,\u4E19>>", u8"\u3409") ||
        ExpectMatch(database, u8"<except=<search=\u4E59,\u4E19>>", u8"\u340A"))
        return Fail("A root nested search exception did not require every component.");
    if(!ExpectMatch(database, u8"<any=\u3407,\u3408,<except=\u4E59>>", u8"\u3408") ||
        ExpectMatch(database, u8"<any=\u3407,\u3408,<except=\u4E59>>", u8"\u3407"))
        return Fail("An any exception did not exclude a direct OR match.");
    if(!ExpectMatch(database, u8"<search=<any=\u7532,\u4E19,<except=\u4E59>>>", u8"\u3408") ||
        ExpectMatch(database, u8"<search=<any=\u7532,\u4E19,<except=\u4E59>>>", u8"\u3407"))
        return Fail("A nested any exception did not preserve its exclusion condition.");
    if(!ExpectMatch(database, u8"<any=\u7532,\u4E8C>", u8"\u4E8C") ||
        ExpectMatch(database, u8"<any=\u7532,\u4E8C>", u8"\u7531") ||
        ExpectMatch(database, u8"<any=\u7532,\u4E8C>", u8"\u4E09"))
        return Fail("An any expression did not use direct OR matching.");
    IDSOwner nestedAnySearch = ParseIDSOwned(u8"<search=<any=\u7532,\u4E8C>>");
    if(nestedAnySearch == nullptr) return Fail("A nested any expression did not parse.");
    if(!ExpectMatch(database, u8"<search=<any=\u7532,\u4E8C>>", u8"\u7531"))
        return Fail("A nested any expression did not match the first component alternative.");
    if(!ExpectMatch(database, u8"<search=<any=\u7532,\u4E8C>>", u8"\u4E09"))
        return Fail("A nested any expression did not match the second component alternative.");
    if(ExpectMatch(database, u8"<search=<any=\u7532,\u4E8C>>", u8"\u3007"))
        return Fail("A nested any expression matched a non-member character.");
    if(!ExpectMatch(database, u8"<any=\u2FF1\u4E00\u4E00,\u2FF0\u7532\u4E00>", u8"\u4E8C"))
        return Fail("An any expression did not accept an IDS term.");
    if(!ExpectMatch(database, u8"⿱<search=一>一", u8"二"))
        return Fail("A nested unified search expression did not match 二.");
    if(!ExpectMatch(database, u8"<search=⿱一一>", u8"二"))
        return Fail("An IDS term inside a unified search expression did not match 二.");
    if(!ExpectMatch(database, "<search=\\u4E00,\\u4E8C>", u8"三"))
        return Fail("Unicode escapes were not expanded before unified search parsing.");
    if(!ExpectMatch(database, "\\u2FF1\\u4E00\\u4E00", u8"二"))
        return Fail("Unicode escapes were not expanded before IDS matching.");
    if(ParseIDSOwned("\\uD800") != nullptr || ParseIDSOwned("\\U00110000") != nullptr ||
        ParseIDSOwned("\\U0010FFFF") == nullptr)
        return Fail("Unicode escape validation accepted an invalid scalar value or rejected U+10FFFF.");

    if(ParseIDSOwned("#(丨b一𠃍)") == nullptr) return Fail("A basic stroke chain did not parse.");
    if(ParseIDSOwned("#(-(一𠃍-丨b一)𠂭𰀄)") == nullptr) return Fail("A grouped stroke chain did not parse.");
    if(ParseIDSOwned("⿺乚#(丨b一𠃍)") == nullptr)
        return Fail("A surrounding structure containing a stroke chain did not parse.");
    if(ParseIDSOwned("⿻[1:]⿺乚#(丨b一𠃍)#(-(一𠃍-丨b一)𠂭𰀄)") == nullptr)
        return Fail("An overlay structure containing grouped strokes did not parse.");
    if(ParseIDSOwned("⿹⺈⿴⿻[1:]⿺乚#(丨b一𠃍)#(-(一𠃍-丨b一)𠂭𰀄)#(一)") == nullptr)
        return Fail("A nested surrounding structure containing grouped strokes did not parse.");
    if(ParseIDSOwned("▥(禾▤(⿹⺈⿴⿻[1:]⿺乚#(丨b一𠃍)#(-(一𠃍-丨b一)𠂭𰀄)#(一))|火)") == nullptr)
        return Fail("An arrangement containing grouped strokes did not parse.");
    IDSOwner complexStrokeIds = ParseIDSOwned("▤(▥(禾▤(⿹⺈⿴⿻[1:]⿺乚#(丨b一𠃍)#(-(一𠃍-丨b一)𠂭𰀄)#(一))|火))");
    if(complexStrokeIds == nullptr) return Fail("A nested stroke IDS did not parse.");
    if(ParseIDSOwned(complexStrokeIds->toString()) == nullptr) return Fail("A nested stroke IDS did not round-trip.");

    if(!ExpectMatch(database, u8"\U0001F504\u8D62\u8D1D\u2B1A", u8"\u8D62") ||
        !ExpectMatch(database, u8"\U0001F504\u8D62\u8D1D\u2B1A", u8"\u5B34"))
        return Fail("A wildcard replacement query did not match all replacement values.");
    if(!ExpectMatch(database, "🔄赢贝女", "嬴")) return Fail("The leaf-component replacement query did not find 嬴.");
    if(!ExpectMatch(database, "🔄湘相先", "洗"))
        return Fail("The horizontal compound replacement query did not find 洗.");
    if(!ExpectMatch(database, "🔄湘沐木", "相"))
        return Fail("The horizontal prefix replacement query did not find 相.");
    if(!ExpectMatch(database, "🔄纵复丁", "结"))
        return Fail("The vertical compound replacement query did not find 结.");

    IDSOwner originalQuery = ParseIDSOwned("🔄赢贝女");
    if(HasIdeograph(database.Match(originalQuery.get()), "赢"))
        return Fail("The source character matched its replacement query.");
    if(!database.GetIDSOwned(Ideograph("坏")).empty())
        return Fail("A query-only replacement operator was stored in the database.");

    sqlite3* sqlite = nullptr;
    if(sqlite3_open_v2("db/replace-query.sqlite", &sqlite, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return Fail("The SQLite test database could not be opened.");
    sqlite3_stmt* columnStatement = nullptr;
    const bool    hasStrokeCount =
        sqlite3_prepare_v2(sqlite, "SELECT 1 FROM pragma_table_info('glyphs') WHERE name = 'stroke_count'", -1,
            &columnStatement, nullptr) == SQLITE_OK &&
        sqlite3_step(columnStatement) == SQLITE_ROW;
    if(columnStatement != nullptr) sqlite3_finalize(columnStatement);
    sqlite3_stmt* countStatement = nullptr;
    std::string   cachedStrokeCounts;
    const bool    hasCachedStrokeCount =
        sqlite3_prepare_v2(sqlite, "SELECT stroke_count FROM glyphs WHERE glyph_key = '二'", -1, &countStatement,
            nullptr) == SQLITE_OK &&
        sqlite3_step(countStatement) == SQLITE_ROW;
    if(hasCachedStrokeCount) {
        const unsigned char* text = sqlite3_column_text(countStatement, 0);
        if(text != nullptr) cachedStrokeCounts = reinterpret_cast<const char*>(text);
    }
    if(countStatement != nullptr) sqlite3_finalize(countStatement);
    auto QueryCount = [sqlite](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        if(sqlite3_prepare_v2(sqlite, sql, -1, &statement, nullptr) != SQLITE_OK) return -1;
        const int result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
        sqlite3_finalize(statement);
        return result;
    };
    const int idsEntryStrokeColumn =
        QueryCount("SELECT COUNT(*) FROM pragma_table_info('ids_entries') WHERE name = 'stroke_count'");
    const int idsEntryStrokeCount = QueryCount(
        "SELECT COUNT(*) FROM ids_entries JOIN glyphs ON glyphs.id = ids_entries.glyph_id " "WHERE glyphs.glyph_key = '二' AND ids_entries.stroke_count = '[2]'");
    const int multiEntryCounts = QueryCount(
        "SELECT COUNT(*) FROM ids_entries JOIN glyphs ON glyphs.id = ids_entries.glyph_id " "WHERE glyphs.glyph_key = '多' AND ids_entries.stroke_count IN ('[1]', '[2]')");
    const int multiGlyphCounts =
        QueryCount("SELECT COUNT(*) FROM glyphs WHERE glyph_key = '多' AND stroke_count = '[1,2]'");
    const int u353eStorage = QueryCount(
        u8"SELECT COUNT(*) FROM ids_entries JOIN glyphs ON glyphs.id = ids_entries.glyph_id "
        "WHERE glyphs.glyph_key = '\u353E' AND ids_entries.raw_ids_text = '#(-\U000200CC\u4E59)' "
        "AND glyphs.stroke_count = '[2]' AND ids_entries.stroke_count = '[2]'");
    const int queryCacheTable =
        QueryCount("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'query_ids_entries'");    const int privateGlyphTable =
        QueryCount("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'private_glyphs'");
    const int iwdsIndexKeyTable =
        QueryCount("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'iwds_subtree_index_keys'");
    const int iwdsIndexEntryTable =
        QueryCount("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'iwds_subtree_index_entries'");
    const int reimportedPrivateGlyphRow = QueryCount(
        u8"SELECT COUNT(*) FROM private_glyphs JOIN glyphs ON glyphs.id = private_glyphs.glyph_id "
        "WHERE glyphs.glyph_key = '\uE001'");
    const int schemaVersion6 =
        QueryCount("SELECT COUNT(*) FROM metadata WHERE key = 'schema_version' AND value = '6'");
    const int rawHVFixtureEntries = QueryCount(
        "SELECT COUNT(*) FROM ids_entries JOIN glyphs ON glyphs.id = ids_entries.glyph_id " "WHERE glyphs.glyph_key = '\u340C'");
    const int cachedHVFixtureEntries = QueryCount(
        "SELECT COUNT(*) FROM query_ids_entries JOIN glyphs ON glyphs.id = query_ids_entries.glyph_id " "WHERE glyphs.glyph_key = '\u340C'");
    const int legacyStrokeTables = QueryCount(
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' " "AND name IN ('ids_entry_stroke_counts', 'glyph_stroke_counts')");
    const int nullStrokeCounts = QueryCount(
        "SELECT (SELECT COUNT(*) FROM glyphs WHERE stroke_count IS NULL OR stroke_count = '') + " "(SELECT COUNT(*) FROM ids_entries WHERE stroke_count IS NULL OR stroke_count = '')");
    sqlite3_close(sqlite);
    if(!hasStrokeCount) return Fail("The SQLite glyph schema does not reserve stroke_count.");
    if(!hasCachedStrokeCount || cachedStrokeCounts != "[2]")
        return Fail("The SQLite glyph cache did not persist 二 as a JSON stroke-count array.");
    if(idsEntryStrokeColumn != 1 || idsEntryStrokeCount != 1)
        return Fail("The SQLite IDS entry stroke_count was not persisted as a JSON array.");
    if(multiEntryCounts != 2 || multiGlyphCounts != 1)
        return Fail("The SQLite primary tables did not preserve all IDS stroke-count values.");
    if(queryCacheTable != 1 || rawHVFixtureEntries != 1 || cachedHVFixtureEntries != 2)
        return Fail("The SQLite schema did not persist raw IDS and all HV query-cache alternatives.");
    if(privateGlyphTable != 1 || reimportedPrivateGlyphRow != 1 || schemaVersion6 != 1)
        return Fail("The SQLite schema did not persist private IDS glyph metadata.");
    if(iwdsIndexKeyTable != 0 || iwdsIndexEntryTable != 0)
        return Fail("The main IDS SQLite database unexpectedly contains an IWDS inverted cache.");
    if(legacyStrokeTables != 0 || nullStrokeCounts != 0)
        return Fail("The SQLite schema retained redundant stroke tables or empty stroke_count fields.");
    if(u353eStorage != 1)
        return Fail("The SQLite raw IDS text or stroke cache for U+353E did not match the source data.");

    IDSOwner missingComponentQuery = ParseIDSOwned("🔄赢木女");
    if(missingComponentQuery == nullptr) return Fail("The missing-component query did not parse.");
    if(!database.Match(missingComponentQuery.get()).empty())
        return Fail("A replacement query matched without the requested component.");

    std::remove("db/same-ids-components.sqlite");
    {
        IDSdatabase sameIDSDatabase("same-ids-components");
        IDSImportReader reader = [](const IDSImportRecordCallback& emit, std::string&) {
            auto add = [&emit](const char* glyph, const char* expression) {
                IDSImportRecord record;
                record.line       = 1;
                record.idsIndex   = 1;
                record.glyphs     = {glyph};
                record.expression = expression;
                record.ids        = expression;
                return emit(record);
            };

            return add(u8"\uE110", "#(S)") &&
                add(u8"\uE111", "#(S)") &&
                add(u8"\uE112", u8"\u2FF1\uE110\u4E09") &&
                add(u8"\uE113", u8"\u2FF1\uE111\u4E09") &&
                add(u8"\uE114", "#(T)");
        };
        if(sameIDSDatabase.ImportDB(reader) != 0)
            return Fail("The same-IDS component fixture could not be imported.");

        IDSOwner sameIDSQuery = ParseIDSOwned(u8"\u2FF1\uE110\u4E09");
        if(sameIDSQuery == nullptr)
            return Fail("The same-IDS component query did not parse.");

        const std::vector<std::string> equivalentQueries =
            sameIDSDatabase.GetEquivalentQueries(sameIDSQuery.get());
        bool hasSameIDSVariant = false;
        for(const std::string& equivalentQuery: equivalentQueries)
            if(equivalentQuery.find(u8"\uE111") != std::string::npos) hasSameIDSVariant = true;
        if(!hasSameIDSVariant)
            return Fail("Same-IDS component preprocessing did not expose the alternate glyph.");

        const std::vector<Ideograph> matches = sameIDSDatabase.MatchQuery(sameIDSQuery.get());
        if(!HasIdeograph(matches, u8"\uE112") || !HasIdeograph(matches, u8"\uE113") ||
            HasIdeograph(matches, u8"\uE114"))
            return Fail("Same-IDS component matching did not preserve the exact set boundary.");
    }
    std::remove("db/same-ids-components.sqlite");

    std::remove("db/single-stroke-suffix.sqlite");
    {
        IDSdatabase singleStrokeDatabase("single-stroke-suffix");
        IDSImportReader reader = [](const IDSImportRecordCallback& emit, std::string&) {
            auto add = [&emit](const char* glyph, const char* expression) {
                IDSImportRecord record;
                record.line       = 1;
                record.idsIndex   = 1;
                record.glyphs     = {glyph};
                record.expression = expression;
                record.ids        = expression;
                return emit(record);
            };

            return add(u8"\uE120", u8"\u2FF0\u4E00\u4E36") &&
                add(u8"\uE121", u8"\u2FF0\u4E00t\u4E36") &&
                add(u8"\u4E00", "#(T)") &&
                add(u8"\u4E00t", "#(T)");
        };
        if(singleStrokeDatabase.ImportDB(reader) != 0)
            return Fail("The single-stroke suffix fixture could not be imported.");

        IDSOwner singleStrokeQuery = ParseIDSOwned(u8"\u2FF0\u31D0\u4E36");
        if(singleStrokeQuery == nullptr)
            return Fail("The single-stroke query did not parse.");

        const std::vector<Ideograph> matches = singleStrokeDatabase.MatchQuery(singleStrokeQuery.get());
        if(!HasIdeograph(matches, u8"\uE120") || HasIdeograph(matches, u8"\uE121"))
            return Fail("A CJK single-stroke fallback matched an unrelated glyph suffix.");

        IDSOwner ordinaryStrokeQuery = ParseIDSOwned(u8"\u2FF0\u4E00\u4E36");
        if(ordinaryStrokeQuery == nullptr)
            return Fail("The ordinary single-stroke query did not parse.");

        const std::vector<Ideograph> ordinaryMatches = singleStrokeDatabase.MatchQuery(ordinaryStrokeQuery.get());
        if(!HasIdeograph(ordinaryMatches, u8"\uE120") || HasIdeograph(ordinaryMatches, u8"\uE121"))
            return Fail("An ordinary single-stroke glyph matched an unrelated glyph suffix.");
    }
    std::remove("db/single-stroke-suffix.sqlite");

    std::remove("db/single-stroke-same-ids.sqlite");
    {
        IDSdatabase singleStrokeSameIDSDatabase("single-stroke-same-ids");
        IDSImportReader reader = [](const IDSImportRecordCallback& emit, std::string&) {
            auto add = [&emit](const char* glyph, const char* expression) {
                IDSImportRecord record;
                record.line       = 1;
                record.idsIndex   = 1;
                record.glyphs     = {glyph};
                record.expression = expression;
                record.ids        = record.expression;
                return emit(record);
            };

            return add(u8"\u31CD", "#(HSHw)") &&
                add(u8"\u31C8w", "#(HSHw)") &&
                add(u8"\u31C8", "#(HSHw)") &&
                add(u8"\uE131", u8"\u2FF0\u31CD\u4E00") &&
                add(u8"\uE132", u8"\u2FF0\u31C8w\u4E00") &&
                add(u8"\uE133", u8"\u2FF0\u31C8\u4E00");
        };
        if(singleStrokeSameIDSDatabase.ImportDB(reader) != 0)
            return Fail("The same-IDS single-stroke fixture could not be imported.");

        IDSOwner query = ParseIDSOwned(u8"<search=\u31CD>");
        if(query == nullptr)
            return Fail("The same-IDS single-stroke query did not parse.");

        for(const std::string& equivalentQuery: singleStrokeSameIDSDatabase.GetEquivalentQueries(query.get()))
            if(equivalentQuery.find(u8"\u31C8w") != std::string::npos)
                return Fail("Single-stroke preprocessing exposed a suffixed same-IDS variant.");

        const std::vector<Ideograph> matches = singleStrokeSameIDSDatabase.MatchQuery(query.get());
        if(!HasIdeograph(matches, u8"\uE131") || !HasIdeograph(matches, u8"\uE133") ||
            HasIdeograph(matches, u8"\uE132"))
            return Fail("Single-stroke same-IDS matching did not preserve suffix boundaries.");
    }
    std::remove("db/single-stroke-same-ids.sqlite");

    std::remove("db/cjk-symbol-fallback.sqlite");
    {
        IDSdatabase cjkSymbolDatabase("cjk-symbol-fallback");
        IDSImportReader reader = [](const IDSImportRecordCallback& emit, std::string&) {
            auto add = [&emit](const char* glyph, const char* expression) {
                IDSImportRecord record;
                record.line       = 1;
                record.idsIndex   = 1;
                record.glyphs     = {glyph};
                record.expression = expression;
                record.ids        = expression;
                return emit(record);
            };

            return add(u8"\uE130", u8"\u2FF0\u2E84\u4E00") &&
                add(u8"\uE131", u8"\u2FF0\u31C8n\u4E00") &&
                add(u8"\uE132", u8"\u2FF0\u4E59\u4E00") &&
                add(u8"\u2E84", "#(HNg)") &&
                add(u8"\u31C8n", "#(HNg)");
        };
        if(cjkSymbolDatabase.ImportDB(reader) != 0)
            return Fail("The CJK symbol fallback fixture could not be imported.");

        if(!ExpectMatch(cjkSymbolDatabase, u8"\u2FF0\u31E4\u4E00", u8"\uE130") ||
            !ExpectMatch(cjkSymbolDatabase, u8"<search=\u31E4>", u8"\uE130") ||
            !ExpectMatch(cjkSymbolDatabase, u8"<search=\u2E84>", u8"\uE130") ||
            !ExpectMatch(cjkSymbolDatabase, u8"<search=\u2E84>", u8"\uE131") ||
            !ExpectMatch(cjkSymbolDatabase, u8"<search=\u31E0>", u8"\uE132"))
            return Fail("U+31E4 did not match its CJK radical fallback U+2E84.");
    }
    std::remove("db/cjk-symbol-fallback.sqlite");

    return 0;
}
