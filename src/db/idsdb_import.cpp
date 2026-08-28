#include "ids4c/idsdb.h"

#include "idsdb_internal.h"

#include <fstream>
#include <unordered_set>
#include <utility>

namespace {

    // 提取 IDS.pdf 7.1 的根部唯一化分隔符；普通部件中的 {抽象名} 不属于此标记。
    static std::string ExtractLeadingUniqueSeparator(const std::string& expression) {
        std::u32string expressionU32;
        try {
            expressionU32 = utf8::utf8to32(expression);
        } catch(const std::exception&) {
            return "";
        }
        if(expressionU32.size() < 3 || expressionU32.front() != U'{') return "";
        const size_t closing = expressionU32.find(U'}', 1);
        if(closing == std::u32string::npos || closing + 1 >= expressionU32.size()) return "";
        return utf8::utf32to8(expressionU32.substr(0, closing + 1));
    }

    static IDSImportRecord MakeInvalidTextRecord(size_t lineNumber, const std::string& line) {
        IDSImportRecord record;
        record.line       = lineNumber;
        record.expression = line;
        return record;
    }

    static bool EmitRecord(const IDSImportRecordCallback& emit, const IDSImportRecord& record) {
        return !emit || emit(record);
    }

    // 统一支持 LF、CRLF 和旧式 Macintosh 使用的单独 CR 换行，且保持流式读取。
    static bool ReadTextLine(std::istream& file, std::string& line) {
        line.clear();
        bool hasCharacter = false;
        char character = 0;
        while(file.get(character)) {
            hasCharacter = true;
            if(character == '\n') return true;
            if(character == '\r') {
                if(file.peek() == '\n') file.get();
                return true;
            }
            line.push_back(character);
        }
        return hasCharacter;
    }

    static bool ReadTextIDS(
        const std::string& filename, IDSdbFormat dbformat, const IDSImportRecordCallback& emit, std::string& error) {
        std::ifstream file(filename, std::ios::in);
        if(!file) {
            error = "Cannot open IDS source file: " + filename;
            return false;
        }

        std::string              line;
        std::vector<std::string> lineSplited, lineSplited2, jSuffixSplit;
        size_t                   lineNumber = 0;
        while(ReadTextLine(file, line)) {
            lineNumber++;
            if(line.empty()) continue;

            std::u32string lineU32;
            try {
                lineU32 = utf8::utf8to32(line);
            } catch(const std::exception&) {
                error = "Invalid UTF-8 sequence at line " + std::to_string(lineNumber) + ".";
                return false;
            }
            if(!lineU32.empty() && lineU32.front() == U'\uFEFF') line = line.substr(3);
            if(line.empty() || line[0] == '#') continue;

            lineSplited = StringSplit(line, U'\t');
            if(lineSplited.size() < 2 || lineSplited[0].empty()) {
                if(!EmitRecord(emit, MakeInvalidTextRecord(lineNumber, line))) {
                    error = "The IDS reader stopped before completing the import.";
                    return false;
                }
                continue;
            }

            const std::string lineKey = lineSplited[0];
            size_t            idsIndex = 0;
            if(dbformat == IDSDB_YIBAI) {
                for(size_t index = 1; index < lineSplited.size(); index++) {
                    lineSplited2 = StringSplit(lineSplited[index], U';');
                    for(const std::string& expression: lineSplited2) {
                        if(expression.empty()) continue;
                        idsIndex++;

                        std::u32string jU32;
                        try {
                            jU32 = utf8::utf8to32(expression);
                        } catch(const std::exception&) {
                            error = "Invalid UTF-8 sequence at line " + std::to_string(lineNumber) +
                                ", IDS #" + std::to_string(idsIndex) + ".";
                            return false;
                        }

                        const size_t jU32pos = jU32.find_last_of(U'(');
                        const bool   isStrokeGroup =
                            jU32pos != std::u32string::npos && jU32pos > 0 && jU32[jU32pos - 1] == U'#';
                        std::string              suffix;
                        std::string              parseExpression = expression;
                        std::vector<std::string> glyphs(1, lineKey);
                        if(jU32pos != std::u32string::npos && !isStrokeGroup) {
                            suffix = utf8::utf32to8(jU32.substr(jU32pos + 1));
                            if(!suffix.empty()) suffix.pop_back();
                            if(suffix != "" && suffix != ".") {
                                parseExpression = utf8::utf32to8(jU32.substr(0, jU32pos));
                                jSuffixSplit     = StringSplit(suffix, U';');
                                glyphs.clear();
                                for(const std::string& glyphSuffix: jSuffixSplit)
                                    glyphs.push_back(lineKey + glyphSuffix);
                            } else if(suffix == ".") {
                                parseExpression = utf8::utf32to8(jU32.substr(0, jU32pos));
                            }
                        }

                        IDSImportRecord record;
                        record.line       = lineNumber;
                        record.idsIndex   = idsIndex;
                        record.glyphs     = std::move(glyphs);
                        record.expression = expression;
                        record.ids        = std::move(parseExpression);
                        if(!EmitRecord(emit, record)) {
                            error = "The IDS reader stopped before completing the import.";
                            return false;
                        }
                    }
                }
            } else {
                for(size_t index = 1; index < lineSplited.size(); index++) {
                    IDSImportRecord record;
                    record.line       = lineNumber;
                    record.idsIndex   = ++idsIndex;
                    record.glyphs.push_back(lineKey);
                    record.expression = lineSplited[index];
                    record.ids        = lineSplited[index];
                    if(!EmitRecord(emit, record)) {
                        error = "The IDS reader stopped before completing the import.";
                        return false;
                    }
                }
            }
        }
        return true;
    }

} // namespace

IDSImportReader MakeTextIDSReader(const std::string& filename, IDSdbFormat dbformat) {
    return [filename, dbformat](const IDSImportRecordCallback& emit, std::string& error) {
        return ReadTextIDS(filename, dbformat, emit, error);
    };
}

bool IDSdatabase::ParseIDSReader(
    const IDSImportReader& reader, IDSStorage& output, IDSUniqueSeparatorStorage& uniqueSeparators) {
    if(!reader) {
        _lastError = "The IDS import reader is empty.";
        return false;
    }

    auto addIssue = [&](size_t lineNumber, size_t idsIndex, const std::string& glyph,
                        const std::string& expression, const std::string& message,
                        const IDSParseError* parseError) {
        if(_lastImportReport.issues.size() < 16) {
            IDSimportIssue issue;
            issue.line       = lineNumber;
            issue.idsIndex   = idsIndex;
            issue.glyph      = glyph;
            issue.expression = expression;
            issue.message    = message;
            if(parseError != nullptr && !parseError->message.empty()) {
                issue.message = parseError->message;
                if(parseError->hasPosition) {
                    issue.characterIndex = parseError->position + 1;
                    issue.unexpectedCharacter =
                        utf8::utf32to8(std::u32string(1, parseError->character));
                }
            }
            _lastImportReport.issues.push_back(issue);
        }
    };

    std::unordered_set<size_t> countedDataLines;
    auto                       consume = [&](const IDSImportRecord& record) {
        const std::string expression = record.expression.empty() ? record.ids : record.expression;
        const std::string parseText  = record.ids.empty() ? record.expression : record.ids;
        const std::string firstGlyph = record.glyphs.empty() ? "" : record.glyphs.front();
        const std::string uniqueSeparator = record.uniqueSeparator.empty()
            ? ExtractLeadingUniqueSeparator(parseText)
            : record.uniqueSeparator;

        if(record.glyphs.empty()) {
            addIssue(record.line, 0, "", expression,
                "expected a tab-separated glyph and IDS expression", nullptr);
            return true;
        }
        if(record.line != 0) countedDataLines.insert(record.line);
        _lastImportReport.sourceExpressions++;

        IDSParseError parseError;
        IDSOwner      parsed = ParseIDSOwned(parseText, &parseError);
        if(parsed == nullptr) {
            _lastImportReport.rejectedExpressions++;
            addIssue(record.line, record.idsIndex, firstGlyph, expression,
                "cannot parse IDS expression", &parseError);
            return true;
        }
        if(ContainsQueryOnlyOperator(parsed.get())) {
            _lastImportReport.rejectedExpressions++;
            addIssue(record.line, record.idsIndex, firstGlyph, expression,
                "query-only or empty IDS expression", nullptr);
            return true;
        }

        bool accepted = false;
        for(const std::string& glyphName: record.glyphs) {
            if(glyphName.empty()) continue;
            IDSOwner entry = parsed->Clone();
            if(entry == nullptr) continue;
            output[Ideograph(glyphName)].push_back(std::move(entry));
            uniqueSeparators[Ideograph(glyphName)].push_back(uniqueSeparator);
            accepted = true;
        }
        if(accepted)
            _lastImportReport.acceptedExpressions++;
        else {
            _lastImportReport.rejectedExpressions++;
            addIssue(record.line, record.idsIndex, firstGlyph, expression,
                "record did not contain a valid glyph", nullptr);
        }
        return true;
    };

    std::string readerError;
    bool        readerResult = false;
    try {
        readerResult = reader(consume, readerError);
    } catch(const std::exception& exception) {
        _lastError = "IDS reader failed: " + std::string(exception.what());
        return false;
    } catch(...) {
        _lastError = "IDS reader failed with an unknown exception.";
        return false;
    }
    if(!readerResult) {
        _lastError = readerError.empty() ? "IDS reader failed." : readerError;
        return false;
    }
    _lastImportReport.dataLines += countedDataLines.size();
    return true;
}

bool IDSdatabase::ParseIDSFile(const std::string& filename, IDSdbFormat dbformat, IDSStorage& output,
    IDSUniqueSeparatorStorage& uniqueSeparators) {
    return ParseIDSReader(MakeTextIDSReader(filename, dbformat), output, uniqueSeparators);
}
