#include "ids4c/idsconst.h"
#include "idsdb_internal.h"

#include <tinyxml2.h>

#include <array>
#include <cstring>
#include <exception>
#include <sstream>

const char* IWDSUnificationLevelName(IWDSUnificationLevel level) {
    switch(level) {
    case IWDS_UNIFICATION_NONE:                   return "none";
    case IWDS_UNIFICATION_SOURCE_CODE_SEPARATION: return "srcseparation";
    case IWDS_UNIFICATION_LV1:                    return "lv1";
    case IWDS_UNIFICATION_LV2:                    return "lv2";
    default:                                      return "none";
    }
}

bool ParseIWDSUnificationLevel(const std::string& value, IWDSUnificationLevel& level) {
    if(value == "none")
        level = IWDS_UNIFICATION_NONE;
    else if(value == "srcseparation")
        level = IWDS_UNIFICATION_SOURCE_CODE_SEPARATION;
    else if(value == "lv1")
        level = IWDS_UNIFICATION_LV1;
    else if(value == "lv2")
        level = IWDS_UNIFICATION_LV2;
    else
        return false;
    return true;
}


static bool IsIWDSVariationSelector(char32_t codepoint) {
    return (codepoint >= 0xFE00 && codepoint <= 0xFE0F) || (codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

static bool IsIWDSWhitespace(char32_t codepoint) {
    return codepoint == U' ' || codepoint == U'\t' || codepoint == U'\r' || codepoint == U'\n';
}

static bool AppendUniqueIdeograph(std::vector<Ideograph>& destination, const Ideograph& ideograph) {
    for(const Ideograph& existing: destination)
        if(existing == ideograph) return false;
    destination.push_back(ideograph);
    return true;
}

static bool ParseIWDSGlyphGroup(const std::string& value, std::vector<Ideograph>& glyphs, std::string& errorMessage) {
    std::u32string codepoints;
    try {
        codepoints = utf8::utf8to32(value);
    } catch(const std::exception& error) {
        errorMessage = std::string("invalid UTF-8: ") + error.what();
        return false;
    }

    glyphs.clear();
    for(char32_t codepoint: codepoints) {
        if(IsIWDSWhitespace(codepoint)) continue;
        if(IsIWDSVariationSelector(codepoint)) {
            if(glyphs.empty()) {
                errorMessage = "a variation selector has no preceding glyph";
                return false;
            }
            const Ideograph& base = glyphs.back();
            glyphs.back()         = Ideograph(base.GetIdeo(), static_cast<uint32_t>(codepoint));
            continue;
        }

        Ideograph glyph(static_cast<uint32_t>(codepoint));
        if(!glyph.inUnicode()) {
            errorMessage = "contains an invalid Unicode scalar value";
            return false;
        }
        AppendUniqueIdeograph(glyphs, glyph);
    }

    if(glyphs.size() < 2) {
        errorMessage = "must contain at least two glyphs";
        return false;
    }
    return true;
}


static size_t IWDSExpressionEnd(const std::u32string& text, size_t start) {
    if(start >= text.size()) return std::u32string::npos;

    const IDCtype idc = IDCchar2type(utf8::utf32to8({text[start]}));
    if(idc == IDC_UNKNOWN) {
        size_t end = start + 1;
        while(end < text.size()) {
            const char32_t character = text[end];
            if(character == U'.' || character == U'_' || (character >= U'0' && character <= U'9') ||
                (character >= U'A' && character <= U'Z') || (character >= U'a' && character <= U'z'))
                end++;
            else if(IsIWDSVariationSelector(character))
                end++;
            else
                break;
        }
        return end;
    }

    // Arrangement IDCs have a parenthesized, variable number of children.
    if(idc == IDC_HORIZONAL_ARRANGE || idc == IDC_VERTICAL_ARRANGE) {
        if(start + 1 >= text.size() || text[start + 1] != U'(') return std::u32string::npos;
        size_t cursor   = start + 2;
        size_t children = 0;
        while(cursor < text.size() && text[cursor] != U')') {
            if(text[cursor] == U'|') {
                cursor++;
                continue;
            }
            const size_t childEnd = IWDSExpressionEnd(text, cursor);
            if(childEnd == std::u32string::npos || childEnd <= cursor) return std::u32string::npos;
            cursor = childEnd;
            children++;
        }
        return cursor < text.size() && children >= 2 ? cursor + 1 : std::u32string::npos;
    }

    size_t argumentCount = 2;
    if(idc == IDC_LEFT_MIDDLE_RIGHT || idc == IDC_ABOVE_MIDDLE_BELOW || idc == IDC_REPLACE)
        argumentCount = 3;
    else if(idc == IDC_HORIZONAL_REFLECT || idc == IDC_ROTATE || idc == IDC_SUBTRACT || idc == IDC_VARIANT)
        argumentCount = 1;

    size_t end = start + 1;
    for(size_t argument = 0; argument < argumentCount; argument++) {
        end = IWDSExpressionEnd(text, end);
        if(end == std::u32string::npos) return std::u32string::npos;
    }
    return end;
}


static bool       IsValidIWDSComponent(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsVariable(ids)) return true;
    if(IsIdeograph(ids)) {
        Ideograph* ideograph = AsIdeograph(ids);
        return !ideograph->inUnicode() || !ideograph->inPUA();
    }
    if(!IsPattern(ids)) return false;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(IsValidIWDSComponent(child)) return true;
    return false;
}

static std::string IWDSPlaceholderName(size_t index) {
    std::string name;
    do {
        name.push_back(static_cast<char>('a' + index % 26));
        if(index < 26) break;
        index = index / 26 - 1;
    } while(true);
    std::reverse(name.begin(), name.end());
    // IWDS 模板变量使用独立命名空间，避免与用户查询中的 <var=...> 冲突。
    return "_" + name;
}
// IWDS XML uses U+2B1A and fullwidth ASCII letters/digits as local placeholders.
// This conversion is intentionally limited to XML import; database IDS keeps its original characters.
static std::string NormalizeIWDSComponentExpression(const std::string& value) {
    const std::u32string codepoints = utf8::utf8to32(value);
    std::string          normalized;
    size_t               placeholderIndex = 0;
    for(const char32_t codepoint: codepoints) {
        if(codepoint == U'\u2B1A') {
            normalized += "<var=" + IWDSPlaceholderName(placeholderIndex++) + ">";
            continue;
        }

        char32_t ascii     = codepoint;
        bool     fullwidth = false;
        if(codepoint >= 0xFF10 && codepoint <= 0xFF19) {
            ascii     = U'0' + (codepoint - 0xFF10);
            fullwidth = true;
        } else if(codepoint >= 0xFF21 && codepoint <= 0xFF3A) {
            ascii     = U'A' + (codepoint - 0xFF21);
            fullwidth = true;
        } else if(codepoint >= 0xFF41 && codepoint <= 0xFF5A) {
            ascii     = U'a' + (codepoint - 0xFF41);
            fullwidth = true;
        }

        if(fullwidth)
            normalized += "<var=_" + utf8::utf32to8({ascii}) + ">";
        else
            normalized += utf8::utf32to8({codepoint});
    }
    return normalized;
}

static bool ContainsIWDSPrivateUse(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsIdeograph(ids)) {
        Ideograph* ideograph = AsIdeograph(ids);
        return ideograph->inUnicode() && ideograph->inPUA();
    }
    if(!IsPattern(ids)) return false;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(ContainsIWDSPrivateUse(child)) return true;
    return false;
}
static bool ParseIWDSComponents(
    const std::string& value, std::vector<std::string>& expressions, std::string& errorMessage) {
    std::u32string codepoints;
    try {
        codepoints = utf8::utf8to32(value);
    } catch(const std::exception& error) {
        errorMessage = std::string("invalid UTF-8: ") + error.what();
        return false;
    }

    expressions.clear();
    size_t cursor = 0;
    while(cursor < codepoints.size()) {
        if(IsIWDSWhitespace(codepoints[cursor])) {
            cursor++;
            continue;
        }
        const size_t end = IWDSExpressionEnd(codepoints, cursor);
        if(end == std::u32string::npos || end <= cursor) {
            errorMessage = "cannot determine an IDS component boundary";
            return false;
        }
        const std::string rawExpression = utf8::utf32to8(codepoints.substr(cursor, end - cursor));
        const std::string expression    = NormalizeIWDSComponentExpression(rawExpression);
        IDSOwner          parsed        = ParseIDSOwned(expression);
        if(parsed == nullptr) {
            errorMessage = "contains an invalid IDS expression: " + expression;
            return false;
        }
        // A private-use code point makes the whole XML IDS expression unusable.
        if(ContainsIWDSPrivateUse(parsed.get())) {
            cursor = end;
            continue;
        }
        if(IsValidIWDSComponent(parsed.get())) {
            bool duplicate = false;
            for(const std::string& existing: expressions)
                if(existing == parsed->toString()) duplicate = true;
            if(!duplicate) expressions.push_back(parsed->toString());
        }
        cursor = end;
    }
    return true;
}
std::string IDSimportReport::Summary() const {
    std::ostringstream output;
    output << "Imported " << acceptedExpressions << "/" << sourceExpressions << " IDS expressions";
    output << "; rejected " << rejectedExpressions;
    output << "; generated " << queryCacheEntries << " HV query-cache entries";
    if(rebuiltCacheGlyphs != 0) output << "; rebuilt cache for " << rebuiltCacheGlyphs << " glyphs";
    if(cacheTruncations != 0) output << "; truncated " << cacheTruncations << " ambiguous HV expansions";
    if(!issues.empty()) {
        const IDSimportIssue& first = issues.front();
        output << ". First issue at line " << first.line;
        if(first.idsIndex != 0) output << ", IDS #" << first.idsIndex;
        if(first.characterIndex != 0) {
            output << ", character #" << first.characterIndex;
            if(!first.unexpectedCharacter.empty()) output << " ('" << first.unexpectedCharacter << "')";
        }
        output << ": " << first.message;
    }
    return output.str();
}


bool IDSdatabase::ImportIWDSXml(std::string filename) {
    tinyxml2::XMLDocument    document;
    const tinyxml2::XMLError loadResult = document.LoadFile(filename.c_str());
    if(loadResult != tinyxml2::XML_SUCCESS) {
        _lastError = "Cannot parse IWDS XML: " + std::string(document.ErrorStr());
        return false;
    }

    tinyxml2::XMLElement* root = document.RootElement();
    if(root == nullptr || std::strcmp(root->Name(), "ucv") != 0) {
        _lastError = "The IWDS XML root element must be <ucv>.";
        return false;
    }

    UnificationGroupBuilder                                                     sourceGroups;
    std::array<UnificationGroupBuilder, IWDS_UNIFICATION_LEVEL_COUNT>           componentIdeographGroups;
    std::array<UnificationExpressionGroupBuilder, IWDS_UNIFICATION_LEVEL_COUNT> componentIDSGroups;
    std::vector<tinyxml2::XMLElement*>                                          pending(1, root);
    size_t                                                                      sourceGroupCount    = 0;
    size_t                                                                      componentGroupCount = 0;
    while(!pending.empty()) {
        tinyxml2::XMLElement* element = pending.back();
        pending.pop_back();

        if(std::strcmp(element->Name(), "entry") == 0) {
            tinyxml2::XMLElement* source = element->FirstChildElement("SourceCodeSeparation");
            if(source != nullptr && source->GetText() != nullptr) {
                const std::vector<std::string> sourceTerms = StringSplit(source->GetText(), U',');
                for(const std::string& sourceTerm: sourceTerms) {
                    std::vector<Ideograph> glyphs;
                    std::string            parseError;
                    if(!ParseIWDSGlyphGroup(sourceTerm, glyphs, parseError)) {
                        _lastError = "Invalid SourceCodeSeparation at XML line " +
                            std::to_string(source->GetLineNum()) + ": " + parseError + ".";
                        return false;
                    }
                    sourceGroups.AddGroup(glyphs);
                    sourceGroupCount++;
                }
            }

            tinyxml2::XMLElement* components = element->FirstChildElement("components");
            if(components != nullptr && components->GetText() != nullptr && element->Attribute("kind") != nullptr &&
                std::strcmp(element->Attribute("kind"), "unifiable") == 0) {
                const int xmlLevel = element->IntAttribute("level", 0);
                if(xmlLevel < 1 || xmlLevel > 2) {
                    _lastError = "Invalid IWDS components level at XML line " +
                        std::to_string(components->GetLineNum()) + ": expected 1 or 2.";
                    return false;
                }
                std::vector<std::string> expressions;
                std::string              parseError;
                if(!ParseIWDSComponents(components->GetText(), expressions, parseError)) {
                    _lastError = "Invalid IWDS components at XML line " + std::to_string(components->GetLineNum()) +
                        ": " + parseError + ".";
                    return false;
                }
                // A single effective component is not an equivalence relation.
                if(expressions.size() >= 2) {
                    const size_t           level         = xmlLevel == 1 ? IWDS_UNIFICATION_LV1 : IWDS_UNIFICATION_LV2;
                    bool                   allIdeographs = true;
                    std::vector<Ideograph> ideographs;
                    for(const std::string& expression: expressions) {
                        IDSOwner parsed = ParseIDSOwned(expression);
                        if(parsed == nullptr || !IsIdeograph(parsed.get())) {
                            allIdeographs = false;
                            break;
                        }
                        ideographs.push_back(*AsIdeograph(parsed.get()));
                    }
                    if(allIdeographs)
                        componentIdeographGroups[level].AddGroup(ideographs);
                    else
                        componentIDSGroups[level].AddGroup(expressions);
                    componentGroupCount++;
                }
            }
        }

        for(tinyxml2::XMLElement* child = element->FirstChildElement(); child != nullptr;
            child                       = child->NextSiblingElement())
            pending.push_back(child);
    }

    if(sourceGroupCount == 0 && componentGroupCount == 0) {
        _lastError = "The IWDS XML does not contain usable unification data.";
        return false;
    }

    const auto previousIdeoGroups = _unifiableIdeoGroups;
    const auto previousIDSGroup   = _unifiableIDSGroups;
    _unifiableIdeoGroups.fill(UnifiableGroups());
    _unifiableIDSGroups.fill(UnifiableIDSGroups());
    _unifiableIdeoGroups[IWDS_UNIFICATION_SOURCE_CODE_SEPARATION] = sourceGroups.BuildGroups();
    for(size_t level = IWDS_UNIFICATION_LV1; level < IWDS_UNIFICATION_LEVEL_COUNT; level++) {
        _unifiableIdeoGroups[level] = componentIdeographGroups[level].BuildGroups();
        _unifiableIDSGroups[level]  = componentIDSGroups[level].BuildGroups();
    }
    BuildUnificationGroupIndexes();
    BuildUnificationIDSGroupIndexes();
    if(!SaveUnifiableSqlite("db/unifiable.sqlite")) {
        _unifiableIdeoGroups = previousIdeoGroups;
        _unifiableIDSGroups  = previousIDSGroup;
        BuildUnificationGroupIndexes();
        BuildUnificationIDSGroupIndexes();
        _lastError = "Cannot save IWDS unification data to db/unifiable.sqlite.";
        return false;
    }

    _lastError.clear();
    return true;
}
