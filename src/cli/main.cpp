#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <iostream>
#include <string>

#include <vector>

#ifdef _WIN32
#    include <windows.h>
#endif

#include "ids4c/ids4c.h"
#include "ids4c/idsdb.h"

namespace po = boost::program_options;

namespace {

#ifdef _WIN32
    std::string WideToUtf8(const std::wstring& value) {
        if(value.empty()) return std::string();

        const int requiredSize = WideCharToMultiByte(
            CP_UTF8, 0, value.c_str(), static_cast<int>(value.length()), nullptr, 0, nullptr, nullptr);
        if(requiredSize <= 0) return std::string();

        std::string out(static_cast<size_t>(requiredSize), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.c_str(), static_cast<int>(value.length()), &out[0], requiredSize, nullptr, nullptr);
        return out;
    }

    std::vector<std::string> CollectUtf8Args() {
        int                      wideArgc = 0;
        wchar_t**                wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        std::vector<std::string> args;
        if(wideArgv == nullptr) return args;

        args.reserve(static_cast<size_t>(wideArgc));
        for(int index = 0; index < wideArgc; index++)
            args.push_back(WideToUtf8(wideArgv[index]));
        LocalFree(wideArgv);
        return args;
    }

    std::vector<char*> BuildArgvStorage(std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.reserve(args.size());
        for(auto& arg: args)
            argv.push_back(&arg[0]);
        return argv;
    }
#endif

    bool ParseResultFilter(const std::string& value, IDSresultFilter& filter) {
        if(value == "all") {
            filter = IDS_RESULT_ALL;
            return true;
        }
        if(value == "ignore-lc-suffix") {
            filter = IDS_RESULT_IGNORE_LC_SUFFIX;
            return true;
        }
        if(value == "ignore-other-locales-base-only") {
            filter = IDS_RESULT_IGNORE_OTHER_LOCALES_BASE_ONLY;
            return true;
        }
        if(value == "ignore-other-locales-keep-ivs") {
            filter = IDS_RESULT_IGNORE_OTHER_LOCALES_KEEP_IVS;
            return true;
        }
        return false;
    }
    bool ParseGlyphArgument(const std::string& value, Ideograph& glyph) {
        if(value.size() > 2 &&
            ((value[0] == 'U' && value[1] == '+') || (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')))) {
            try {
                const size_t        prefix       = 2;
                size_t              parsedLength = 0;
                const unsigned long codepoint    = std::stoul(value.substr(prefix), &parsedLength, 16);
                if(parsedLength != value.size() - prefix || codepoint > 0x10FFFF ||
                    (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                    return false;
                glyph = Ideograph(static_cast<uint32_t>(codepoint));
                return glyph.inUnicode();
            } catch(const std::exception&) {
                return false;
            }
        }
        glyph = Ideograph(value);
        return glyph.inUnicode() || !glyph.GetAbstractName().empty();
    }

    void PrintIDSList(const char* label, const IDSOwnerList& entries) {
        std::cout << label;
        if(entries.empty()) {
            std::cout << " (none)" << std::endl;
            return;
        }
        for(const auto& entry: entries)
            std::cout << " " << entry->toString();
        std::cout << std::endl;
    }
    bool ParseImportFormat(const std::string& value, IDSdbFormat& format) {
        if(value == "default") {
            format = IDSDB_DEFAULT;
            return true;
        }
        if(value == "yibai") {
            format = IDSDB_YIBAI;
            return true;
        }
        return false;
    }

    enum class CLIOutputFormat { TEXT, CSV, TSV, JSON };

    bool ParseCLIOutputFormat(const std::string& value, CLIOutputFormat& format) {
        if(value == "text")
            format = CLIOutputFormat::TEXT;
        else if(value == "csv")
            format = CLIOutputFormat::CSV;
        else if(value == "tsv")
            format = CLIOutputFormat::TSV;
        else if(value == "json")
            format = CLIOutputFormat::JSON;
        else
            return false;
        return true;
    }

    bool ParseCLIJsonUnicodeMode(const std::string& value, bool& escapeUnicode) {
        if(value == "raw")
            escapeUnicode = false;
        else if(value == "escaped")
            escapeUnicode = true;
        else
            return false;
        return true;
    }

    void AppendJsonUnicodeEscape(std::string& out, std::uint32_t codepoint) {
        static const char hex[]      = "0123456789ABCDEF";
        auto              appendUnit = [&out](std::uint32_t value) {
            out += "\\u";
            out += hex[(value >> 12) & 0x0F];
            out += hex[(value >> 8) & 0x0F];
            out += hex[(value >> 4) & 0x0F];
            out += hex[value & 0x0F];
        };
        if(codepoint <= 0xFFFF) {
            appendUnit(codepoint);
            return;
        }
        codepoint -= 0x10000;
        appendUnit(0xD800 + (codepoint >> 10));
        appendUnit(0xDC00 + (codepoint & 0x3FF));
    }

    bool DecodeUTF8Codepoint(const std::string& value, size_t& index, std::uint32_t& codepoint) {
        if(index >= value.size()) return false;
        const unsigned char first   = static_cast<unsigned char>(value[index]);
        size_t              length  = 0;
        std::uint32_t       minimum = 0;
        if(first < 0x80) {
            length    = 1;
            codepoint = first;
        } else if(first >= 0xC2 && first <= 0xDF) {
            length    = 2;
            minimum   = 0x80;
            codepoint = first & 0x1F;
        } else if(first >= 0xE0 && first <= 0xEF) {
            length    = 3;
            minimum   = 0x800;
            codepoint = first & 0x0F;
        } else if(first >= 0xF0 && first <= 0xF4) {
            length    = 4;
            minimum   = 0x10000;
            codepoint = first & 0x07;
        } else
            return false;

        if(index + length > value.size()) return false;
        for(size_t offset = 1; offset < length; offset++) {
            const unsigned char next = static_cast<unsigned char>(value[index + offset]);
            if((next & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if(codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
        index += length;
        return true;
    }

    std::string JsonString(const std::string& value, bool escapeUnicode = false) {
        std::string out   = "\"";
        size_t      index = 0;
        while(index < value.size()) {
            const size_t  start     = index;
            std::uint32_t codepoint = 0;
            if(!DecodeUTF8Codepoint(value, index, codepoint)) {
                const unsigned char byte = static_cast<unsigned char>(value[index++]);
                if(byte < 0x20) {
                    out                    += "\\u00";
                    static const char hex[] = "0123456789ABCDEF";
                    out                    += hex[(byte >> 4) & 0x0F];
                    out                    += hex[byte & 0x0F];
                } else
                    AppendJsonUnicodeEscape(out, byte);
                continue;
            }

            if(codepoint == '\\')
                out += "\\\\";
            else if(codepoint == '\"')
                out += "\\\"";
            else if(codepoint == '\b')
                out += "\\b";
            else if(codepoint == '\f')
                out += "\\f";
            else if(codepoint == '\n')
                out += "\\n";
            else if(codepoint == '\r')
                out += "\\r";
            else if(codepoint == '\t')
                out += "\\t";
            else if(codepoint < 0x20) {
                out                    += "\\u00";
                static const char hex[] = "0123456789ABCDEF";
                out                    += hex[(codepoint >> 4) & 0x0F];
                out                    += hex[codepoint & 0x0F];
            } else if(codepoint < 0x80)
                out += static_cast<char>(codepoint);
            else if(escapeUnicode)
                AppendJsonUnicodeEscape(out, codepoint);
            else
                out.append(value, start, index - start);
        }
        out += '\"';
        return out;
    }

    std::string PreprocessRulesText(const IDSMatchDetail& detail) {
        std::string out;
        for(const IDSPreprocessRule rule: detail.preprocessRules) {
            if(!out.empty()) out += ",";
            out += IDSPreprocessRuleName(rule);
        }
        return out;
    }

    std::string MatchPathsText(const std::vector<IDSMatchPath>& paths) {
        std::string out;
        for(const IDSMatchPath& path: paths) {
            if(!out.empty()) out += ";";
            out += std::string(IDSMatchPathKindName(path.kind)) + ":";
            out += path.queryExpressionIndex == std::numeric_limits<size_t>::max()
                ? "input@"
                : "q[" + std::to_string(path.queryExpressionIndex) + "]@";
            out += path.queryPath + "=>" + path.path;
        }
        return out;
    }
    std::string DelimitedValue(const std::string& value, char delimiter) {
        if(delimiter == '\t') {
            std::string out;
            for(const char character: value) {
                if(character == '\\')
                    out += "\\\\";
                else if(character == '\t')
                    out += "\\t";
                else if(character == '\r')
                    out += "\\r";
                else if(character == '\n')
                    out += "\\n";
                else
                    out += character;
            }
            return out;
        }
        std::string out      = value;
        size_t      position = 0;
        while((position = out.find('"', position)) != std::string::npos) {
            out.insert(position, 1, '"');
            position += 2;
        }
        if(out.find_first_of(",\r\n") != std::string::npos) return '"' + out + '"';
        return out;
    }

    void PrintDelimitedResults(const std::vector<IDSMatchDetail>& details, CLIOutputFormat format) {
        const char delimiter = format == CLIOutputFormat::TSV ? '\t' : ',';
        std::cout << "glyph" << delimiter << "matched_ids" << delimiter << "raw_ids" << delimiter << "match_kind"
                  << delimiter << "match_source" << delimiter << "preprocess_rules" << delimiter << "match_paths"
                  << std::endl;
        for(const IDSMatchDetail& detail: details) {
            std::cout << DelimitedValue(detail.glyph.toString(), delimiter) << delimiter
                      << DelimitedValue(detail.matchedIDS, delimiter) << delimiter
                      << DelimitedValue(detail.rawIDS, delimiter) << delimiter
                      << DelimitedValue(IDSMatchKindName(detail.matchKind), delimiter) << delimiter
                      << DelimitedValue(IDSMatchSourceName(detail.matchSource), delimiter) << delimiter
                      << DelimitedValue(PreprocessRulesText(detail), delimiter) << delimiter
                      << DelimitedValue(MatchPathsText(detail.matchPaths), delimiter) << std::endl;
        }
    }

    void PrintJsonResults(const std::string& query, const std::vector<std::string>& equivalentQueries,
        const std::vector<IDSMatchDetail>& details, long long elapsedMilliseconds, bool escapeUnicode) {
        const auto json = [escapeUnicode](const std::string& value) {
            return JsonString(value, escapeUnicode);
        };
        std::cout << "{\n  \"query\": " << json(query) << ",\n  \"equivalent_syntax\": [";
        for(size_t index = 0; index < equivalentQueries.size(); index++) {
            if(index != 0) std::cout << ", ";
            std::cout << json(equivalentQueries[index]);
        }
        std::cout << "],\n  \"matches\": [";
        for(size_t index = 0; index < details.size(); index++) {
            const IDSMatchDetail& detail = details[index];
            if(index != 0) std::cout << ",";
            std::cout << "\n    {\n      \"glyph\": " << json(detail.glyph.toString())
                      << ",\n      \"matched_ids\": " << json(detail.matchedIDS)
                      << ",\n      \"raw_ids\": " << json(detail.rawIDS)
                      << ",\n      \"match_kind\": " << json(IDSMatchKindName(detail.matchKind))
                      << ",\n      \"match_source\": " << json(IDSMatchSourceName(detail.matchSource))
                      << ",\n      \"preprocess_rules\": [";
            for(size_t ruleIndex = 0; ruleIndex < detail.preprocessRules.size(); ruleIndex++) {
                if(ruleIndex != 0) std::cout << ", ";
                std::cout << json(IDSPreprocessRuleName(detail.preprocessRules[ruleIndex]));
            }
            std::cout << "],\n      \"match_paths\": [\n";
            for(size_t pathIndex = 0; pathIndex < detail.matchPaths.size(); pathIndex++) {
                const IDSMatchPath& path = detail.matchPaths[pathIndex];
                std::cout << "        {" << '"' << "kind" << '"' << ": " << json(IDSMatchPathKindName(path.kind))
                          << ", ";
                std::cout << '"' << "query_expression_index" << '"' << ": ";
                if(path.queryExpressionIndex == std::numeric_limits<size_t>::max())
                    std::cout << "null";
                else
                    std::cout << path.queryExpressionIndex;
                std::cout << ", " << '"' << "query_path" << '"' << ": " << json(path.queryPath) << ", " << '"' << "path"
                          << '"' << ": " << json(path.path) << "}";
                if(pathIndex != detail.matchPaths.size() - 1) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "      ]";
            std::cout << "\n    }";
        }
        std::cout << "\n  ],\n  \"elapsed_ms\": " << elapsedMilliseconds << "\n}" << std::endl;
    }
    void PrintImportDiagnostics(const IDSimportReport& report) {
        for(const IDSimportIssue& issue: report.issues) {
            std::cerr << "line " << issue.line;
            if(issue.idsIndex != 0) std::cerr << ", IDS #" << issue.idsIndex;
            if(issue.characterIndex != 0) {
                std::cerr << ", character #" << issue.characterIndex;
                if(!issue.unexpectedCharacter.empty()) std::cerr << " ('" << issue.unexpectedCharacter << "')";
            }
            if(!issue.glyph.empty()) std::cerr << ", glyph " << issue.glyph;
            if(!issue.expression.empty()) std::cerr << ", IDS " << issue.expression;
            std::cerr << ": " << issue.message << std::endl;
        }
    }

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    setlocale(LC_ALL, ".utf-8");
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> utf8Args = CollectUtf8Args();
    std::vector<char*>       utf8Argv = BuildArgvStorage(utf8Args);
    argc                              = static_cast<int>(utf8Argv.size());
    argv                              = utf8Argv.data();
#endif

    po::options_description options("IDS4C options");
    options.add_options()("help,h", "Show this help message.")("database,d", po::value<std::string>(),
        "SQLite database name, without .sqlite.")("query,q", po::value<std::string>(), "Run an IDS query.")(
        "import,i", po::value<std::string>(), "Import an IDS source file into db/DATABASE.sqlite.")(
        "import-private,p", po::value<std::string>(), "Add or update private IDS entries in an existing database.")(
        "reimport-private", po::value<std::string>(), "Replace all private IDS entries in an existing database.")(
        "import-iwds", po::value<std::string>(), "Import IWDS unification data from an IWDS XML file.")("format",
        po::value<std::string>()->default_value("default"), "Import format: default or yibai.")("unification-level",
        po::value<std::string>()->default_value("none"), "IWDS fuzzy matching: none, srcseparation, lv1, or lv2.")(
        "default-region", po::value<std::string>()->default_value(""),
        "Fallback glyph region/suffix used when an exact component glyph is unavailable.")(
        "ignore-overlay", "Ignore candidates whose matching path uses an overlay structure.")(
        "locale-suffix-order", po::value<std::string>()->default_value(""),
        "Locale suffix fallback order; use > for fallback and = for same-level suffixes, for example C=G>.=H.")(
        "disable-same-ids-exclusion", "Allow same-IDS variants marked with {glyph} to match each other.")(
        "no-match-paths", "Do not calculate match paths in detailed output.")("result-filter",
        po::value<std::string>()->default_value("all"),
        "all, ignore-lc-suffix, ignore-other-locales-base-only, or ignore-other-locales-keep-ivs.")("glyph-domain",
        po::value<std::string>()->default_value("all"), "Glyph domain: all, unicode, private, or abstract.")(
        "unicode-block", po::value<std::string>()->default_value("all"),
        "Unicode blocks, comma separated: all, cjk, basic, ext-a..ext-j, compatibility, radicals, strokes, private-bmp, private-plane15, private-plane16, abstract, or other.")(

        "explain,e", "Show matched IDS, source, rules, and paths.")("inspect", po::value<std::string>(),
        "Inspect one glyph by character, U+XXXX, 0xXXXX, or abstract name.")("show-ids", "Alias for --explain.")(
        "output-format", po::value<std::string>()->default_value("text"), "Query output: text, csv, tsv, or json.")(
        "json-unicode", po::value<std::string>()->default_value("raw"), "JSON Unicode mode: raw or escaped.");

    po::variables_map values;
    try {
        po::store(po::parse_command_line(argc, argv, options), values);
        if(values.count("help") != 0) {
            std::cout << "Query:   ids4c-cli --database NAME --query IDS [options]" << std::endl;
            std::cout << "Inspect: ids4c-cli --database NAME --inspect GLYPH" << std::endl;
            std::cout << "Import: ids4c-cli --database NAME --import SOURCE [--format default|yibai]" << std::endl;
            std::cout << "Private: ids4c-cli --database NAME --import-private SOURCE [--format default|yibai]"
                      << std::endl;
            std::cout << "Reimport: ids4c-cli --database NAME --reimport-private SOURCE [--format default|yibai]"
                      << std::endl;
            std::cout << "IWDS:     ids4c-cli --import-iwds SOURCE.xml" << std::endl;
            std::cout << options << std::endl;
            return 0;
        }
        po::notify(values);
    } catch(const po::error& error) {
        std::cerr << "Argument error: " << error.what() << std::endl;
        std::cerr << options << std::endl;
        return 2;
    }

    const bool hasQuery           = values.count("query") != 0;
    const bool hasInspect         = values.count("inspect") != 0;
    const bool hasImport          = values.count("import") != 0;
    const bool hasPrivateImport   = values.count("import-private") != 0;
    const bool hasPrivateReimport = values.count("reimport-private") != 0;
    const bool hasIWDSImport      = values.count("import-iwds") != 0;
    const int operationCount = static_cast<int>(hasQuery) + static_cast<int>(hasInspect) + static_cast<int>(hasImport) +
        static_cast<int>(hasPrivateImport) + static_cast<int>(hasPrivateReimport) + static_cast<int>(hasIWDSImport);
    if(operationCount != 1 || (!hasIWDSImport && values.count("database") == 0)) {
        std::cerr << "Specify exactly one operation, and use --database except for --import-iwds." << std::endl;
        return 2;
    }

    if(hasIWDSImport) {
        boost::system::error_code filesystemError;
        boost::filesystem::create_directories("db", filesystemError);
        if(filesystemError) {
            std::cerr << "Cannot create the database directory: " << filesystemError.message() << std::endl;
            return 3;
        }

        IDSdatabase unificationDatabase("unifiable");
        if(!unificationDatabase.ImportIWDSXml(values["import-iwds"].as<std::string>())) {
            const std::string& error = unificationDatabase.GetLastError();
            std::cerr << (error.empty() ? "Unable to import the IWDS XML file." : error) << std::endl;
            return 3;
        }
        std::cout << "Imported IWDS unification data." << std::endl;
        return 0;
    }

    const std::string databaseName = values["database"].as<std::string>();
    if(hasImport || hasPrivateImport || hasPrivateReimport) {
        IDSdbFormat importFormat = IDSDB_DEFAULT;
        if(!ParseImportFormat(values["format"].as<std::string>(), importFormat)) {
            std::cerr << "Invalid --format value: " << values["format"].as<std::string>() << std::endl;
            return 2;
        }

        if(hasImport) {
            boost::system::error_code filesystemError;
            boost::filesystem::create_directories("db", filesystemError);
            if(filesystemError) {
                std::cerr << "Cannot create the database directory: " << filesystemError.message() << std::endl;
                return 3;
            }
        }

        IDSdatabase database(databaseName, hasImport ? IDSdbOpenMode::StartEmpty : IDSdbOpenMode::LoadExisting);
        if((hasPrivateImport || hasPrivateReimport) && database.isEmpty()) {
            const std::string& error = database.GetLastError();
            std::cerr << (error.empty() ? "Database is empty or unavailable." : error) << std::endl;
            return 3;
        }

        const std::string source       = hasImport ? values["import"].as<std::string>()
            : hasPrivateImport                     ? values["import-private"].as<std::string>()
                                                   : values["reimport-private"].as<std::string>();
        const IDSImportStageCallback showStage = [](IDSimportStage stage) {
            std::cerr << "[import] " << IDSImportStageName(stage) << std::endl;
        };
        const int importResult = hasImport ? database.ImportDB(source, importFormat, showStage)
            : hasPrivateImport              ? database.ImportPrivateDB(source, importFormat, showStage)
                                            : database.ReimportPrivateDB(source, importFormat, showStage);
        if(importResult != 0) {
            const std::string& error = database.GetLastError();
            std::cerr << (error.empty() ? "Unable to import the IDS source file." : error) << std::endl;
            PrintImportDiagnostics(database.GetLastImportReport());
            return 3;
        }

        const IDSimportReport& report = database.GetLastImportReport();
        std::cout << report.Summary() << std::endl;
        if(report.HasIssues() || report.cacheTruncations != 0) PrintImportDiagnostics(report);
        return 0;
    }

    if(hasInspect) {
        IDSdatabase database(databaseName);
        std::vector<std::string> inspectLocaleOrder;
        if(!ParseLocaleSuffixFallbackOrder(values["locale-suffix-order"].as<std::string>(), inspectLocaleOrder)) {
            std::cerr << "Invalid --locale-suffix-order value: " << values["locale-suffix-order"].as<std::string>() << std::endl;
            return 2;
        }
        database.config.fuzzyMatch.defaultRegion = values["default-region"].as<std::string>();
        database.config.fuzzyMatch.localeSuffixFallbackOrder = inspectLocaleOrder;
        if(database.isEmpty()) {
            const std::string& error = database.GetLastError();
            std::cerr << (error.empty() ? "Database is empty or unavailable." : error) << std::endl;
            return 3;
        }
        Ideograph glyph(0);
        if(!ParseGlyphArgument(values["inspect"].as<std::string>(), glyph)) {
            std::cerr << "Invalid glyph argument for --inspect." << std::endl;
            return 2;
        }
        std::cout << "Glyph: " << glyph.toString() << std::endl;
        PrintIDSList("Raw IDS:", database.GetRawIDSOwned(glyph));
        PrintIDSList("HV IDS:", database.GetIDSOwned(glyph));
        std::cout << "Same IDS:";
        for(const Ideograph& value: database.GetSameIDSCharacters(glyph))
            std::cout << " " << value.toString();
        std::cout << std::endl;
        std::cout << "Containing:";
        for(const Ideograph& value: database.GetContainingCharacters(glyph))
            std::cout << " " << value.toString();
        std::cout << std::endl;
        return 0;
    }

    IDSqueryOptions queryOptions;
    queryOptions.filter.ignoreOverlayStructure = values.count("ignore-overlay") != 0;
    queryOptions.trackMatchPaths               = values.count("no-match-paths") == 0;
    if(!ParseIDSglyphDomain(values["glyph-domain"].as<std::string>(), queryOptions.filter.glyphDomain)) {
        std::cerr << "Invalid --glyph-domain value: " << values["glyph-domain"].as<std::string>() << std::endl;
        return 2;
    }
    if(!ParseIDSunicodeBlocks(values["unicode-block"].as<std::string>(), queryOptions.filter.unicodeBlocks)) {
        std::cerr << "Invalid --unicode-block value: " << values["unicode-block"].as<std::string>() << std::endl;
        return 2;
    }

    const std::string filterValue = values["result-filter"].as<std::string>();
    if(!ParseResultFilter(filterValue, queryOptions.filter.resultFilter)) {
        std::cerr << "Invalid --result-filter value: " << filterValue << std::endl;
        return 2;
    }

    IWDSUnificationLevel unificationLevel = IWDS_UNIFICATION_NONE;
    const std::string localeSuffixOrderValue = values["locale-suffix-order"].as<std::string>();
    if(!ParseLocaleSuffixFallbackOrder(localeSuffixOrderValue, queryOptions.filter.localeSuffixFallbackOrder)) {
        std::cerr << "Invalid --locale-suffix-order value: " << localeSuffixOrderValue << std::endl;
        return 2;
    }
    const std::string    unificationValue = values["unification-level"].as<std::string>();
    if(!ParseIWDSUnificationLevel(unificationValue, unificationLevel)) {
        std::cerr << "Invalid --unification-level value: " << unificationValue << std::endl;
        return 2;
    }

    IDSdatabase database(databaseName);
    database.config.fuzzyMatch.defaultRegion    = values["default-region"].as<std::string>();
    database.config.fuzzyMatch.unificationLevel = unificationLevel;
    database.config.fuzzyMatch.excludeNonEquivalentSameIDS = values.count("disable-same-ids-exclusion") == 0;
    database.config.fuzzyMatch.localeSuffixFallbackOrder = queryOptions.filter.localeSuffixFallbackOrder;
    if(database.isEmpty()) {
        const std::string& error = database.GetLastError();
        std::cerr << (error.empty() ? "Database is empty or unavailable." : error) << std::endl;
        return 3;
    }

    IDSParseError parseError;
    IDSOwner      query = ParseIDSOwned(values["query"].as<std::string>(), &parseError);
    if(query == nullptr) {
        std::cerr << "Illegal query expression";
        if(!parseError.message.empty()) {
            if(parseError.hasPosition) std::cerr << " at character #" << (parseError.position + 1);
            std::cerr << ": " << parseError.message;
        } else
            std::cerr << ". Please check.";
        std::cerr << std::endl;
        return 2;
    }

    CLIOutputFormat outputFormat = CLIOutputFormat::TEXT;
    if(!ParseCLIOutputFormat(values["output-format"].as<std::string>(), outputFormat)) {
        std::cerr << "Invalid --output-format value: " << values["output-format"].as<std::string>() << std::endl;
        return 2;
    }
    bool escapeJsonUnicode = false;
    if(!ParseCLIJsonUnicodeMode(values["json-unicode"].as<std::string>(), escapeJsonUnicode)) {
        std::cerr << "Invalid --json-unicode value: " << values["json-unicode"].as<std::string>() << std::endl;
        return 2;
    }

    const std::vector<std::string> equivalentQueries = database.GetEquivalentQueries(query.get());
    if(outputFormat == CLIOutputFormat::TEXT) {
        std::cout << "Equivalent Syntax: ";
        for(const std::string& equivalentQuery: equivalentQueries)
            std::cout << equivalentQuery << (equivalentQuery == equivalentQueries.back() ? "" : ", ");
        std::cout << std::endl;
    }

    const bool                  explain      = values.count("explain") != 0 || values.count("show-ids") != 0;
    const bool                  needsDetails = explain || outputFormat != CLIOutputFormat::TEXT;
    const auto                  started      = std::chrono::steady_clock::now();
    std::vector<Ideograph>      matches;
    std::vector<IDSMatchDetail> details;
    if(needsDetails) {
        details = database.MatchDetailed(query.get(), queryOptions);
        matches.reserve(details.size());
        for(const IDSMatchDetail& detail: details)
            matches.push_back(detail.glyph);
    } else {
        matches = database.MatchQuery(query.get(), queryOptions);
    }
    const auto ended = std::chrono::steady_clock::now();

    std::sort(matches.begin(), matches.end(), IdeographCmp);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ended - started);
    if(outputFormat == CLIOutputFormat::CSV || outputFormat == CLIOutputFormat::TSV) {
        std::cout << "equivalent_syntax" << std::endl;
        for(size_t index = 0; index < equivalentQueries.size(); index++)
            std::cout << equivalentQueries[index] << std::endl;
        PrintDelimitedResults(details, outputFormat);
    } else if(outputFormat == CLIOutputFormat::JSON) {
        PrintJsonResults(
            values["query"].as<std::string>(), equivalentQueries, details, elapsed.count(), escapeJsonUnicode);
    } else if(explain) {
        for(const IDSMatchDetail& detail: details) {
            std::cout << detail.glyph.toString() << "\tkind=" << IDSMatchKindName(detail.matchKind)
                      << "\tsource=" << IDSMatchSourceName(detail.matchSource)
                      << "\tpreprocess=" << PreprocessRulesText(detail)
                      << "\tpaths=" << MatchPathsText(detail.matchPaths) << "\tIDS=" << detail.matchedIDS
                      << "\traw=" << detail.rawIDS;
            std::cout << std::endl;
        }
    } else {
        std::string matchString;
        for(const Ideograph& match: matches)
            matchString += match.toString() + " ";
        if(!matchString.empty()) matchString.pop_back();
        std::cout << matchString << std::endl;
    }
    std::cerr << matches.size() << " match" << (matches.size() == 1 ? "" : "es") << " in " << elapsed.count() << " ms."
              << std::endl;
    return 0;
}
