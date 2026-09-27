#include "ids4c/idsconst.h"
#include "ids4c/idsdb.h"

#include "idsdb_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

static bool IsCodepointInUnicodeBlock(uint32_t codepoint, IDSunicodeBlock block);

const char* IDSImportStageName(IDSimportStage stage) {
    switch(stage) {
    case IDSimportStage::Reading: return "Reading and validating IDS";
    case IDSimportStage::HVCache: return "Building HV query cache";
    case IDSimportStage::StrokeNeutralCache: return "Building stroke-neutral cache";
    case IDSimportStage::ComponentIndex: return "Building component index";
    case IDSimportStage::StrokeCache: return "Calculating stroke counts";
    case IDSimportStage::Saving: return "Saving SQLite database";
    case IDSimportStage::Complete: return "Import complete";
    }
    return "Importing";
}

// 只有{汉字}是IDS.pdf 7.1的唯一化标记；{?0}到{?3}等前缀仅表示来源信息。
static bool IsExplicitGlyphUniqueSeparator(const std::string& separator) {
    std::u32string value;
    try {
        value = utf8::utf8to32(separator);
    } catch(const std::exception&) {
        return false;
    }
    if(value.size() != 3 || value.front() != U'{' || value.back() != U'}') return false;

    const uint32_t codepoint = static_cast<uint32_t>(value[1]);
    return IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK) ||
        IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_COMPATIBILITY);
}
bool ParseLocaleSuffixFallbackOrder(const std::string& value, std::vector<std::string>& order) {
    order.clear();
    if(value.empty()) return true;

    size_t start = 0;
    std::vector<std::string> seenSuffixes;
    while(start <= value.size()) {
        const size_t separator = value.find('>', start);
        const size_t end       = separator == std::string::npos ? value.size() : separator;
        size_t first           = start;
        size_t last            = end;
        while(first < last && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
        while(last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
        if(first == last) {
            order.clear();
            return false;
        }

        const std::string groupText = value.substr(first, last - first);
        std::vector<std::string> group;
        size_t groupStart = 0;
        while(groupStart <= groupText.size()) {
            const size_t sameLevelSeparator = groupText.find('=', groupStart);
            const size_t groupEnd = sameLevelSeparator == std::string::npos
                ? groupText.size()
                : sameLevelSeparator;
            size_t suffixFirst = groupStart;
            size_t suffixLast  = groupEnd;
            while(suffixFirst < suffixLast &&
                std::isspace(static_cast<unsigned char>(groupText[suffixFirst])))
                ++suffixFirst;
            while(suffixLast > suffixFirst &&
                std::isspace(static_cast<unsigned char>(groupText[suffixLast - 1])))
                --suffixLast;
            if(suffixFirst == suffixLast) {
                order.clear();
                return false;
            }

            std::string suffix = groupText.substr(suffixFirst, suffixLast - suffixFirst);
            if(suffix == ".") suffix.clear();
            if(std::find(seenSuffixes.begin(), seenSuffixes.end(), suffix) != seenSuffixes.end()) {
                order.clear();
                return false;
            }
            seenSuffixes.push_back(suffix);
            group.push_back(std::move(suffix));

            if(sameLevelSeparator == std::string::npos) break;
            groupStart = sameLevelSeparator + 1;
        }

        if(group.size() == 1) {
            order.push_back(std::move(group.front()));
        } else {
            std::string encodedGroup;
            for(size_t index = 0; index < group.size(); ++index) {
                if(index != 0) encodedGroup.push_back('=');
                encodedGroup += group[index].empty() ? "." : group[index];
            }
            order.push_back(std::move(encodedGroup));
        }

        if(separator == std::string::npos) break;
        start = separator + 1;
    }
    return true;
}

// 将公开的扁平配置项还原为回退级别。一个元素中的 '=' 表示同级后缀。
// 例如 {"C=G", ".=H"} 表示 C/G 同级，空后缀/H 同级。
static std::vector<std::vector<std::string>> LocaleSuffixFallbackGroups(
    const std::vector<std::string>& order) {
    std::vector<std::vector<std::string>> groups;
    for(const std::string& token: order) {
        std::vector<std::string> group;
        size_t start = 0;
        while(start <= token.size()) {
            const size_t separator = token.find('=', start);
            const size_t end       = separator == std::string::npos ? token.size() : separator;
            std::string suffix     = token.substr(start, end - start);
            if(suffix == ".") suffix.clear();
            group.push_back(std::move(suffix));
            if(separator == std::string::npos) break;
            start = separator + 1;
        }
        if(!group.empty()) groups.push_back(std::move(group));
    }
    return groups;
}

static void AppendBorrowedIDS(std::vector<IDS*>& out, const IDSOwnerList& ids) {
    for(const auto& i: ids)
        out.push_back(i.get());
}

static bool AppendUniqueOwned(IDSOwnerList& out, IDSOwner ids, size_t maximum, bool& truncated) {
    if(ids == nullptr) return false;
    for(const auto& existing: out)
        if(IDSequal(existing.get(), ids.get())) return false;
    if(out.size() >= maximum) {
        truncated = true;
        return false;
    }
    out.push_back(std::move(ids));
    return true;
}

static void AppendClonedOwners(IDSOwnerList& out, const IDSOwnerList& source) {
    for(const auto& ids: source)
        out.push_back(ids->Clone());
}

static void AppendEquivalentQuerySemanticText(std::string& key, const std::string& value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key += value;
}

static void AppendEquivalentQuerySemanticKey(IDS* ids, std::string& key) {
    if(ids == nullptr) {
        key += "null;";
        return;
    }
    if(IsIdeograph(ids)) {
        key += "ideo;";
        AppendEquivalentQuerySemanticText(key, ids->toString());
        return;
    }
    if(IsStroke(ids)) {
        key += "stroke;";
        AppendEquivalentQuerySemanticText(key, ids->toString());
        return;
    }
    if(IsSearchParam(ids)) {
        key += "param;";
        AppendEquivalentQuerySemanticText(key, ids->toString());
        return;
    }
    if(IsVariable(ids)) {
        key += "variable;";
        AppendEquivalentQuerySemanticText(key, ids->toString());
        return;
    }
    if(IsSearchExpression(ids)) {
        const SearchExpression* search = AsSearchExpression(ids);
        key                           += "search;" + std::to_string(static_cast<int>(search->GetMode())) + ";";
        key                           += "terms;" + std::to_string(search->GetTerms().size()) + ";";
        for(const auto& term: search->GetTerms())
            AppendEquivalentQuerySemanticKey(term.get(), key);
        key += "except;" + std::to_string(search->GetExceptTerms().size()) + ";";
        for(const auto& term: search->GetExceptTerms())
            AppendEquivalentQuerySemanticKey(term.get(), key);
        return;
    }
    if(IsPattern(ids)) {
        const Pattern* pattern = AsPattern(ids);
        const IDCtype  idc     = pattern->GetIDC();
        key                   += "pattern;" + std::to_string(static_cast<int>(idc)) + ";";
        if(idc == IDC_OVERLAY) {
            int range[2] = {0, 0};
            pattern->GetOverlayRange(range);
            key += "range;" + std::to_string(range[0]) + ";" + std::to_string(range[1]) + ";";
            const std::vector<std::string> overlayType = pattern->GetOverlayType();
            key                                       += "overlay;" + std::to_string(overlayType.size()) + ";";
            for(const std::string& value: overlayType)
                AppendEquivalentQuerySemanticText(key, value);
        } else {
            key += "optional;" + std::to_string(pattern->GetOptionalInt()) + ";";
            // 当前匹配器对排列结构只按子节点序列匹配，不使用 preferSplitPoint。
            // 因此 ▥(AB) 与 ▥(A|B) 在等价查询层面可以合并。
        }
        key += "children;" + std::to_string(pattern->GetpIDSRef().size()) + ";";
        for(const auto& child: pattern->GetpIDSRef())
            AppendEquivalentQuerySemanticKey(child.get(), key);
        return;
    }
    key += "other;";
    AppendEquivalentQuerySemanticText(key, ids->toString());
}

static std::string EquivalentQuerySemanticKey(IDS* ids) {
    std::string key;
    AppendEquivalentQuerySemanticKey(ids, key);
    return key;
}

static IDSPreprocessRule IWDSRuleForLevel(size_t level) {
    switch(level) {
    case IWDS_UNIFICATION_SOURCE_CODE_SEPARATION: return IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION;
    case IWDS_UNIFICATION_LV1:                    return IDS_PREPROCESS_IWDS_LV1_COMPONENT;
    case IWDS_UNIFICATION_LV2:                    return IDS_PREPROCESS_IWDS_LV2_COMPONENT;
    default:                                      return IDS_PREPROCESS_NONE;
    }
}

static void AppendUniquePreprocessRule(std::vector<IDSPreprocessRule>& rules, IDSPreprocessRule rule) {
    if(rule == IDS_PREPROCESS_NONE) return;
    if(std::find(rules.begin(), rules.end(), rule) == rules.end()) rules.push_back(rule);
}

const char* IDSPreprocessRuleName(IDSPreprocessRule rule) {
    switch(rule) {
    case IDS_PREPROCESS_CJK_SYMBOL_FALLBACK:         return "cjk-symbol-fallback";
    case IDS_PREPROCESS_SAME_IDS:                    return "same-ids";
    case IDS_PREPROCESS_HV_EXTRACT:                  return "hv-extract";
    case IDS_PREPROCESS_YIBAI_COMPATIBILITY:         return "yibai-compatibility";
    case IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION: return "iwds-srcseparation";
    case IDS_PREPROCESS_IWDS_LV1_COMPONENT:          return "iwds-lv1-component";
    case IDS_PREPROCESS_IWDS_LV2_COMPONENT:          return "iwds-lv2-component";
    case IDS_PREPROCESS_NONE:                        return "none";
    default:                                         return "unknown";
    }
}

const char* IDSMatchKindName(IDSMatchKind kind) {
    switch(kind) {
    case IDS_MATCH_KIND_EXACT_IDS:        return "exact IDS";
    case IDS_MATCH_KIND_STRUCTURAL_IDS:   return "structural IDS match";
    case IDS_MATCH_KIND_COMPONENT_SEARCH: return "component search";
    case IDS_MATCH_KIND_SAME_IDS:         return "same IDS expression";
    case IDS_MATCH_KIND_EXCEPT_CONDITION: return "except condition";
    case IDS_MATCH_KIND_MATCHED:          return "matched";
    default:                              return "unknown";
    }
}

const char* IDSMatchSourceName(IDSMatchSource source) {
    switch(source) {
    case IDS_MATCH_SOURCE_RAW_IDS:  return "raw IDS";
    case IDS_MATCH_SOURCE_HV_CACHE: return "HV cache";
    case IDS_MATCH_SOURCE_NONE:     return "";
    default:                        return "unknown";
    }
}
const char* IDSMatchPathKindName(IDSMatchPathKind kind) {
    switch(kind) {
    case IDS_MATCH_PATH_NODE:     return "node";
    case IDS_MATCH_PATH_ALL_TERM: return "all";
    case IDS_MATCH_PATH_ANY_TERM: return "any";
    case IDS_MATCH_PATH_RESIDUE:  return "residue";
    default:                      return "unknown";
    }
}

class ScopedBoolean {
public:
    ScopedBoolean(bool& value, bool replacement): _value(value), _previous(value) { _value = replacement; }
    ~ScopedBoolean() { _value = _previous; }

private:
    bool& _value;
    bool  _previous;
};

template <typename Bindings>
class VariableBindingScope {
private:
    Bindings& _bindings;
    Bindings  _saved;
    bool                                          _committed = false;

public:
    explicit VariableBindingScope(Bindings& bindings):
        _bindings(bindings),
        _saved(bindings) {}

    ~VariableBindingScope() {
        if(!_committed) _bindings = std::move(_saved);
    }

    const Bindings& GetSaved() const { return _saved; }

    bool Finish(bool matched) {
        _committed = matched;
        return matched;
    }
};

void IDSdatabase::RecordQueryPreprocessRule(const std::string& expression, IDSPreprocessRule rule) {
    if(expression.empty() || rule == IDS_PREPROCESS_NONE) return;
    AppendUniquePreprocessRule(_queryPreprocessRules[expression], rule);
}

void IDSdatabase::MergeQueryPreprocessRules(const std::string& target, const std::string& source) {
    if(target.empty() || source.empty()) return;
    const auto found = _queryPreprocessRules.find(source);
    if(found == _queryPreprocessRules.end()) return;
    for(const IDSPreprocessRule rule: found->second)
        RecordQueryPreprocessRule(target, rule);
}

void IDSdatabase::MergeQueryPreprocessRulesTree(const std::string& target, IDS* source) {
    if(target.empty() || source == nullptr) return;
    MergeQueryPreprocessRules(target, source->toString());

    if(IsSearchExpression(source)) {
        SearchExpression* search = AsSearchExpression(source);
        for(const auto& term: search->GetTerms())
            MergeQueryPreprocessRulesTree(target, term.get());
        for(const auto& term: search->GetExceptTerms())
            MergeQueryPreprocessRulesTree(target, term.get());
    } else if(IsPattern(source)) {
        for(IDS* child: AsPattern(source)->GetpIDS())
            MergeQueryPreprocessRulesTree(target, child);
    }
}

void IDSdatabase::CollectQueryPreprocessRulesTree(IDS* source, std::vector<IDSPreprocessRule>& rules) const {
    if(source == nullptr) return;
    const auto found = _queryPreprocessRules.find(source->toString());
    if(found != _queryPreprocessRules.end())
        for(const IDSPreprocessRule rule: found->second)
            AppendUniquePreprocessRule(rules, rule);

    if(IsSearchExpression(source)) {
        SearchExpression* search = AsSearchExpression(source);
        for(const auto& term: search->GetTerms())
            CollectQueryPreprocessRulesTree(term.get(), rules);
        for(const auto& term: search->GetExceptTerms())
            CollectQueryPreprocessRulesTree(term.get(), rules);
    } else if(IsPattern(source)) {
        for(IDS* child: AsPattern(source)->GetpIDS())
            CollectQueryPreprocessRulesTree(child, rules);
    }
}

static bool IsWildcard(IDS* ids) {
    return IsSearchParam(ids) && AsSearchParam(ids)->GetParam() == SPARAM_QUESTIONMARK;
}

static bool IsStrictIDSExpression(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsIdeograph(ids) || IsStroke(ids)) return true;
    if(!IsPattern(ids)) return false;
    for(const auto& child: AsPattern(ids)->GetpIDSRef())
        if(!IsStrictIDSExpression(child.get())) return false;
    return true;
}

static bool IsCJKSingleStroke(uint32_t codepoint) {
    return codepoint >= 0x31C0 && codepoint <= 0x31E5;
}

static bool IsCJKRadicalSymbol(uint32_t codepoint) {
    return codepoint >= 0x2E80 && codepoint <= 0x2FDF;
}

static bool IsCJKRadicalStrokeAlias(const Ideograph& left, const Ideograph& right) {
    if(!left.inUnicode() || !right.inUnicode()) return false;
    return (IsCJKRadicalSymbol(left.GetIdeo()) && IsCJKSingleStroke(right.GetIdeo())) ||
        (IsCJKSingleStroke(left.GetIdeo()) && IsCJKRadicalSymbol(right.GetIdeo()));
}

static bool ContainsIWDSVariable(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsVariable(ids)) return true;
    if(!IsPattern(ids)) return false;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(ContainsIWDSVariable(child)) return true;
    return false;
}
static bool ContainsIWDSQueryFlexible(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsVariable(ids) || IsWildcard(ids)) return true;
    if(!IsPattern(ids)) return false;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(ContainsIWDSQueryFlexible(child)) return true;
    return false;
}
static bool ContainsIWDSFixedAtom(IDS* ids) {
    if(ids == nullptr || IsVariable(ids) || IsWildcard(ids)) return false;
    if(IsIdeograph(ids) || IsStroke(ids)) return true;
    if(!IsPattern(ids)) return true;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(ContainsIWDSFixedAtom(child)) return true;
    return false;
}
static bool IsStrokeCountQuery(IDS* ids) {
    return IsSearchParam(ids) && AsSearchParam(ids)->GetParam() == SPARAM_STROKE_COUNT;
}

static bool IsResidueCountQuery(IDS* ids) {
    return IsSearchParam(ids) && AsSearchParam(ids)->GetParam() == SPARAM_RESIDUE_STROKE_COUNT;
}

// A fixed IDS term has no bindings whose result can depend on another search term.
static bool IsSearchTermMemoSafe(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsVariable(ids) || IsSearchExpression(ids) || IsSearchParam(ids)) return false;
    if(!IsPattern(ids)) return true;
    for(IDS* child: AsPattern(ids)->GetpIDS())
        if(!IsSearchTermMemoSafe(child)) return false;
    return true;
}

// 多项 <search> 可以先做“各项是否存在”的必要条件检查。简单 <any>
// 没有变量、排除项或嵌套 search 时，递归结果可以安全复用；最终仍需
// IDSsearch 检查各条件是否占用了合法的不同节点。
static bool IsIndependentSearchTermMemoSafe(IDS* ids) {
    if(ids == nullptr) return false;
    if(!IsSearchExpression(ids)) return IsIdeograph(ids);

    SearchExpression* expression = AsSearchExpression(ids);
    if(expression->GetMode() != SEARCH_EXPRESSION_ANY || !expression->GetExceptTerms().empty() ||
        expression->GetTerms().empty())
        return false;
    for(const auto& term: expression->GetTerms())
        if(IsSearchExpression(term.get()) || !IsIdeograph(term.get()))
            return false;
    return true;
}

static bool IsHorizontalArrangeIDC(IDCtype idc) {
    return idc == IDC_HORIZONAL_ARRANGE;
}

static bool IsVerticalArrangeIDC(IDCtype idc) {
    return idc == IDC_VERTICAL_ARRANGE;
}

static bool IsArrangeIDC(IDCtype idc) {
    return IsHorizontalArrangeIDC(idc) || IsVerticalArrangeIDC(idc);
}

static bool IsReplaceIDC(IDCtype idc) {
    return idc == IDC_REPLACE;
}

static bool IsSubtractIDC(IDCtype idc) {
    return idc == IDC_SUBTRACT;
}

static bool IsReplaceQuery(IDS* ids) {
    return IsPattern(ids) && IsReplaceIDC(AsPattern(ids)->GetIDC());
}

static bool IsSubtractQuery(IDS* ids) {
    return IsPattern(ids) && IsSubtractIDC(AsPattern(ids)->GetIDC());
}

static bool ContainsReplaceSubtractOperator(IDS* ids) {
    if(ids == nullptr) return false;
    if(IsReplaceQuery(ids) || IsSubtractQuery(ids)) return true;
    if(IsSearchExpression(ids)) {
        SearchExpression* search = AsSearchExpression(ids);
        for(const auto& term: search->GetTerms())
            if(ContainsReplaceSubtractOperator(term.get())) return true;
        for(const auto& term: search->GetExceptTerms())
            if(ContainsReplaceSubtractOperator(term.get())) return true;
        return false;
    }
    if(!IsPattern(ids)) return false;
    for(const auto& child: AsPattern(ids)->GetpIDSRef())
        if(ContainsReplaceSubtractOperator(child.get())) return true;
    return false;
}

static std::vector<IDS*> BorrowIDSList(const IDSOwnerList& ids) {
    std::vector<IDS*> out;
    out.reserve(ids.size());
    for(const auto& i: ids)
        out.push_back(i.get());
    return out;
}

static IDS* PatternChild(const Pattern* pattern, size_t index) {
    return pattern->GetpIDSRef()[index].get();
}

static size_t PatternChildCount(const Pattern* pattern) {
    return pattern->GetpIDSRef().size();
}

static std::vector<IDS*> BorrowPatternIDS(const Pattern* pattern) {
    return BorrowIDSList(pattern->GetpIDSRef());
}

static size_t FindArrangementFragmentLength(
    const IDSOwnerList& children, size_t start, IDCtype arrangement, const IDSOwnerList& componentVariants) {
    size_t matchedLength = 0;
    for(const auto& variant: componentVariants) {
        if(!IsPattern(variant.get())) continue;

        Pattern* componentPattern = AsPattern(variant.get());
        if(componentPattern->GetIDC() != arrangement) continue;

        const IDSOwnerList& componentChildren = componentPattern->GetpIDSRef();
        if(componentChildren.empty() || componentChildren.size() > children.size() - start) continue;

        bool matches = true;
        for(size_t i = 0; i < componentChildren.size(); i++)
            if(!IDSequal(children[start + i].get(), componentChildren[i].get())) {
                matches = false;
                break;
            }
        if(matches && componentChildren.size() > matchedLength) matchedLength = componentChildren.size();
    }
    return matchedLength;
}

static std::vector<IDS*> ReleaseIDSList(IDSOwnerList ids) {
    std::vector<IDS*> out;
    out.reserve(ids.size());
    for(auto& i: ids)
        out.push_back(i.release());
    return out;
}

static bool EraseMarkedTerms(std::vector<IDS*>& terms, std::vector<size_t>& indices) {
    if(indices.empty()) return false;
    for(size_t i = indices.size(); i > 0; i--)
        terms.erase(terms.begin() + indices[i - 1]);
    indices.clear();
    return true;
}

void IDSdatabase::MakeStrokeRevDB() {
    _strokeRevDB.clear();
    for(auto i: _strokeDB) {
        std::string combinedStr, combinedStr2;
        for(auto j: i.second)
            combinedStr += j, combinedStr2 = "-" + j + combinedStr2;
        if(_strokeRevDB.find(combinedStr) == _strokeRevDB.end()) {
            _strokeRevDB.insert({combinedStr, i.first});
            _strokeRevDB.insert({combinedStr2, "-" + i.first});
        } else if(_strokeRevDB[combinedStr] > i.first)
            _strokeRevDB[combinedStr] = i.first, _strokeRevDB[combinedStr2] = "-" + i.first;
    }
}
static bool IsCurveStrokeFragment(const std::string& stroke) {
    return stroke == u8"\u25DC" || stroke == u8"\u25DD" || stroke == u8"\u25DE" || stroke == u8"\u25DF";
}

static bool IsAsciiLetter(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

static bool IsPureLetterStroke(Stroke* stroke) {
    const std::vector<Stroke_Data> strokes = stroke->GetStroke();
    if(strokes.empty() || !stroke->GetBreakPos().empty() || !stroke->GetCrossData().empty() || stroke->isEnclosed())
        return false;

    for(const Stroke_Data& item: strokes) {
        if(item.neg || item.stroke.empty()) return false;
        for(const char character: item.stroke)
            if(!IsAsciiLetter(character)) return false;
    }
    return true;
}

static StrokeCountSet AddStrokeCountSets(const StrokeCountSet& left, const StrokeCountSet& right) {
    StrokeCountSet out;
    for(const uint32_t leftCount: left)
        for(const uint32_t rightCount: right)
            if(leftCount <= std::numeric_limits<uint32_t>::max() - rightCount) out.insert(leftCount + rightCount);
    return out;
}

void IDSdatabase::MakeStrokeCountCache() {
    for(const auto& entry: _rawIDSDB) {
        if(_strokeCountCache.find(entry.first) != _strokeCountCache.end()) continue;
        std::unordered_set<Ideograph, Ideograph_Hash> recursionStack;
        IDSOwner                                      root(new Ideograph(entry.first));
        CalculateStrokeCounts(root.get(), recursionStack);
    }
}

StrokeCountSet IDSdatabase::CalculateStrokeCounts(
    IDS* ids, std::unordered_set<Ideograph, Ideograph_Hash>& recursionStack) {
    if(ids == nullptr) return StrokeCountSet();

    if(IsStroke(ids)) {
        Stroke*                        stroke  = AsStroke(ids);
        const std::vector<Stroke_Data> strokes = stroke->GetStroke();
        if(IsPureLetterStroke(stroke)) return StrokeCountSet({1});

        const std::vector<size_t> breaks           = stroke->GetBreakPos();
        uint32_t                  count            = 0;
        bool                      previousWasCurve = false;
        for(size_t index = 0; index < strokes.size(); index++) {
            bool breakBeforeCurrent = false;
            for(const size_t breakPosition: breaks)
                if(breakPosition + 1 == index) {
                    breakBeforeCurrent = true;
                    break;
                }

            const bool isCurve = IsCurveStrokeFragment(strokes[index].stroke);

            const bool connectedByCurve = index != 0 && !breakBeforeCurrent && (previousWasCurve || isCurve);
            if(!connectedByCurve) {
                if(count == std::numeric_limits<uint32_t>::max()) return StrokeCountSet();
                count++;
            }
            previousWasCurve = isCurve;
        }
        return StrokeCountSet({count});
    }

    if(IsPattern(ids)) {
        StrokeCountSet totals({0});
        for(const auto& child: AsPattern(ids)->GetpIDSRef()) {
            const StrokeCountSet childCounts = CalculateStrokeCounts(child.get(), recursionStack);
            if(childCounts.empty()) return StrokeCountSet();
            totals = AddStrokeCountSets(totals, childCounts);
            if(totals.empty()) return StrokeCountSet();
        }
        return totals;
    }

    if(!IsIdeograph(ids)) return StrokeCountSet();
    const Ideograph ideograph = *AsIdeograph(ids);
    const auto      cached    = _strokeCountCache.find(ideograph);
    if(cached != _strokeCountCache.end()) return cached->second;
    if(recursionStack.find(ideograph) != recursionStack.end()) return StrokeCountSet();

    const auto entries = _rawIDSDB.find(ideograph);
    if(entries == _rawIDSDB.end()) return StrokeCountSet();

    recursionStack.insert(ideograph);
    StrokeCountSet counts;
    for(const auto& entry: entries->second) {
        const StrokeCountSet entryCounts = CalculateStrokeCounts(entry.get(), recursionStack);
        counts.insert(entryCounts.begin(), entryCounts.end());
    }
    recursionStack.erase(ideograph);

    if(!counts.empty()) {
        _strokeCountCache[ideograph] = counts;
        _pendingStrokeCountCache.insert(ideograph);
    }
    return counts;
}

StrokeCountSet IDSdatabase::GetStrokeCounts(IDS* ids) {
    std::unordered_set<Ideograph, Ideograph_Hash> recursionStack;
    return CalculateStrokeCounts(ids, recursionStack);
}

IDSdatabase::StrokeCountRange IDSdatabase::GetStrokeRange(IDS* ids) {
    StrokeCountRange unknown;
    if(ids == nullptr) return unknown;

    const std::string cacheKey = ids->toString();
    const auto        cached   = _strokeRangeCache.find(cacheKey);
    if(cached != _strokeRangeCache.end()) return cached->second;

    const uint32_t maximumValue = std::numeric_limits<uint32_t>::max();
    auto           fromCounts   = [](const StrokeCountSet& counts) {
        StrokeCountRange range;
        if(counts.empty()) return range;

        range.minimum = *std::min_element(counts.begin(), counts.end());
        range.maximum = *std::max_element(counts.begin(), counts.end());
        range.bounded = true;
        return range;
    };
    auto addRanges = [maximumValue](StrokeCountRange& total, const StrokeCountRange& child) {
        if(total.minimum > maximumValue - child.minimum)
            total.minimum = maximumValue;
        else
            total.minimum += child.minimum;

        if(!total.bounded || !child.bounded || total.maximum > maximumValue - child.maximum) {
            total.maximum = maximumValue;
            total.bounded = false;
        } else {
            total.maximum += child.maximum;
        }
    };

    StrokeCountRange range;
    if(IsWildcard(ids) || IsVariable(ids)) {
        // 通配符和变量的实际部件未知：最少可以贡献 0 画，不能假定上界。
    } else if(IsStroke(ids) || IsIdeograph(ids)) {
        range = fromCounts(GetStrokeCounts(ids));
    } else if(IsSearchParam(ids)) {
        SearchParam* param = AsSearchParam(ids);
        if(param->GetParam() == SPARAM_STROKE_COUNT) {
            range.minimum = param->GetStrokeMinimum();
            range.maximum = param->GetStrokeMaximum();
            range.bounded = true;
        }
    } else if(IsSearchExpression(ids)) {
        SearchExpression* search = AsSearchExpression(ids);
        if(search->GetMode() == SEARCH_EXPRESSION_ANY) {
            bool haveAlternative = false;
            for(const auto& term: search->GetTerms()) {
                const StrokeCountRange child = GetStrokeRange(term.get());
                if(!haveAlternative) {
                    range           = child;
                    haveAlternative = true;
                    continue;
                }

                range.minimum = std::min(range.minimum, child.minimum);
                if(!range.bounded || !child.bounded)
                    range.bounded = false, range.maximum = maximumValue;
                else
                    range.maximum = std::max(range.maximum, child.maximum);
            }
            // <any> 可以在更大的候选树中命中一个部件，不能使用上界剪枝。
            if(haveAlternative) range.bounded = false, range.maximum = maximumValue;
        } else if(search->GetMode() == SEARCH_EXPRESSION_ALL) {
            range.minimum   = 0;
            range.maximum   = 0;
            range.bounded   = true;
            bool hasResidue = false;
            for(const auto& term: search->GetTerms()) {
                StrokeCountRange child;
                if(IsResidueCountQuery(term.get())) {
                    if(hasResidue) {
                        range.bounded = false;
                        range.maximum = maximumValue;
                        continue;
                    }
                    SearchParam* residue = AsSearchParam(term.get());
                    child.minimum        = residue->GetStrokeMinimum();
                    child.maximum        = residue->GetStrokeMaximum();
                    child.bounded        = true;
                    hasResidue           = true;
                } else {
                    child = GetStrokeRange(term.get());
                }
                addRanges(range, child);
            }

            // 没有 residue 时，search 只约束部件，候选字形仍可有额外笔画。
            if(!hasResidue) {
                range.bounded = false;
                range.maximum = maximumValue;
            }
        }
    } else if(IsPattern(ids)) {
        range.minimum = 0;
        range.maximum = 0;
        range.bounded = true;
        for(const auto& child: AsPattern(ids)->GetpIDSRef())
            addRanges(range, GetStrokeRange(child.get()));
    }

    _strokeRangeCache.insert({cacheKey, range});
    return range;
}

bool IDSdatabase::HasDisjointStrokeRange(IDS* candidate, IDS* query) {
    if(candidate == nullptr || query == nullptr) return false;

    // 字形节点还可能通过后缀互认、同 IDS 或 CJK 符号回退命中，
    // 这些等价关系不要求两个字形的笔画缓存相同。
    if(IsIdeograph(candidate) && IsIdeograph(query)) return false;

    const StrokeCountRange candidateRange = GetStrokeRange(candidate);
    const StrokeCountRange queryRange     = GetStrokeRange(query);

    if(candidateRange.bounded && queryRange.minimum > candidateRange.maximum) return true;
    if(queryRange.bounded && candidateRange.minimum > queryRange.maximum) return true;
    return false;
}
bool IDSdatabase::MatchStrokeCount(IDS* ids, SearchParam* query) {
    if(query == nullptr ||
        (query->GetParam() != SPARAM_STROKE_COUNT && query->GetParam() != SPARAM_RESIDUE_STROKE_COUNT))
        return false;
    const StrokeCountSet counts = GetStrokeCounts(ids);
    for(const uint32_t count: counts)
        if(count >= query->GetStrokeMinimum() && count <= query->GetStrokeMaximum()) return true;
    return false;
}

void IDSdatabase::BuildUnificationGroupIndexes() {
    for(auto& index: _unifiableIdeoGroupIndex)
        index.clear();
    for(size_t level = 0; level < _unifiableIdeoGroups.size(); level++) {
        for(size_t groupIndex = 0; groupIndex < _unifiableIdeoGroups[level].size(); groupIndex++) {
            for(const Ideograph& glyph: _unifiableIdeoGroups[level][groupIndex])
                _unifiableIdeoGroupIndex[level].insert({glyph, groupIndex});
        }
    }
}

static std::string IWDSIDSShapeKey(IDS* ids) {
    if(IsIdeograph(ids)) return "I";
    if(IsPattern(ids)) {
        Pattern* pattern = AsPattern(ids);
        return "P:" + std::to_string(static_cast<int>(pattern->GetIDC())) + ":" +
            std::to_string(PatternChildCount(pattern));
    }
    return "";
}

// 当前 IWDS 数据中的纯占位符结构变换只有两组：
// ⿱ＸＹ ↔ ⿸ＸＹ，以及 ⿰ＸＹ ↔ ⿺ＸＹ。
// 其它包围结构即使出现在 IWDS 中，也可能带有固定部件，不能按纯变量模板放宽。
static bool IsIWDSVariableStructureTransformIDC(IDCtype idc) {
    return idc == IDC_LEFT_RIGHT || idc == IDC_ABOVE_BELOW || idc == IDC_SURROUND_UPPERLEFT ||
        idc == IDC_SURROUND_LOWERLEFT;
}

void IDSdatabase::BuildUnificationIDSGroupIndexes() {
    for(auto& index: _unifiableIDSGroupIndex)
        index.clear();
    for(auto& groups: _unifiableIDSParsedGroups)
        groups.clear();
    for(auto& index: _unifiableIDSShapeIndex)
        index.clear();

    for(size_t level = 0; level < _unifiableIDSGroups.size(); level++) {
        for(size_t groupIndex = 0; groupIndex < _unifiableIDSGroups[level].size(); groupIndex++) {
            UnifiableIDSParsedGroup parsedGroup;
            for(const std::string& expression: _unifiableIDSGroups[level][groupIndex]) {
                _unifiableIDSGroupIndex[level].insert({expression, groupIndex});
                IDSOwner parsed = ParseIDSOwned(expression);
                if(parsed == nullptr) continue;

                const std::string shape = IWDSIDSShapeKey(parsed.get());
                if(!shape.empty()) {
                    std::vector<size_t>& groupIndexes = _unifiableIDSShapeIndex[level][shape];
                    if(std::find(groupIndexes.begin(), groupIndexes.end(), groupIndex) == groupIndexes.end())
                        groupIndexes.push_back(groupIndex);
                }
                parsedGroup.push_back(std::move(parsed));
            }
            _unifiableIDSParsedGroups[level].push_back(std::move(parsedGroup));
        }
    }
}

IDSdatabase::IDSdatabase(std::string name): IDSdatabase(std::move(name), IDSdbOpenMode::LoadExisting) {}

IDSdatabase::IDSdatabase(std::string name, IDSdbOpenMode mode) {
    if(name == "unifiable") return;
    this->name = name;
    if(mode == IDSdbOpenMode::StartEmpty) return;
    if(!LoadSqliteDatabase("db/" + name + ".sqlite")) return;

    LoadUnifiableSqlite("db/unifiable.sqlite");
    MakeStrokeDB();
    MakeStrokeRevDB();
    MakeCache();
}

IDSdatabase::~IDSdatabase() = default;

bool IDSdatabase::RegisterGlyphRange(const IDSCustomGlyphRange& range) {
    if(range.name.empty()) return false;
    if(range.codepointRanges.empty() && range.exactGlyphs.empty() && range.abstractGlyphs.empty() &&
        range.suffixes.empty())
        return false;
    for(const IDSCodepointRange& interval: range.codepointRanges)
        if(interval.first > interval.last || interval.last > 0x10FFFF) return false;
    _customGlyphRanges[range.name] = range;
    return true;
}

bool IDSdatabase::RegisterUnicodeRange(const std::string& name, uint32_t first, uint32_t last) {
    IDSCustomGlyphRange range;
    range.name = name;
    range.codepointRanges.push_back({first, last});
    return RegisterGlyphRange(range);
}

bool IDSdatabase::UnregisterGlyphRange(const std::string& name) {
    return _customGlyphRanges.erase(name) != 0;
}

std::vector<IDSCustomGlyphRange> IDSdatabase::GetGlyphRanges() const {
    std::vector<IDSCustomGlyphRange> ranges;
    ranges.reserve(_customGlyphRanges.size());
    for(const auto& entry: _customGlyphRanges)
        ranges.push_back(entry.second);
    std::sort(ranges.begin(), ranges.end(),
        [](const IDSCustomGlyphRange& left, const IDSCustomGlyphRange& right) { return left.name < right.name; });
    return ranges;
}

bool IDSdatabase::MatchesCustomGlyphRange(const Ideograph& ideograph, const IDSCustomGlyphRange& range) const {
    for(const IDSCodepointRange& interval: range.codepointRanges)
        if(ideograph.inUnicode() && ideograph.GetIdeo() >= interval.first && ideograph.GetIdeo() <= interval.last)
            return true;
    if(std::find(range.exactGlyphs.begin(), range.exactGlyphs.end(), ideograph.toString()) != range.exactGlyphs.end())
        return true;
    if(std::find(range.abstractGlyphs.begin(), range.abstractGlyphs.end(), ideograph.GetAbstractName()) !=
        range.abstractGlyphs.end())
        return true;
    if(std::find(range.suffixes.begin(), range.suffixes.end(), ideograph.GetSuffix()) != range.suffixes.end())
        return true;
    return false;
}

void IDSdatabase::MakeCache() {
    _cache_BasicIdeo.clear();
    _cache_IdeoSuffixLookup.clear();
    for(const auto& i: _idsDB) {
        // _cache_BasicIdeo
        bool allBasic = true;
        for(const auto& j: i.second) {
            if(allBasic &&
                !((IsIdeograph(j.get()) && *AsIdeograph(j.get()) == i.first) ||
                    (IsStroke(j.get()) && AsStroke(j.get())->isBasicStroke())))
                allBasic = false;
        }
        if(allBasic) _cache_BasicIdeo.insert(i.first);
        // _cache_IdeoSuffixLookup
        Ideograph pureIdeo = i.first.inUnicode() ? Ideograph(i.first.GetIdeo()) : Ideograph(i.first.GetAbstractName());
        if(_cache_IdeoSuffixLookup.find(pureIdeo) != _cache_IdeoSuffixLookup.end())
            _cache_IdeoSuffixLookup[pureIdeo].push_back((VSandSuffix){i.first.GetVS(), i.first.GetSuffix()});
        else
            _cache_IdeoSuffixLookup.insert({pureIdeo, {(VSandSuffix){i.first.GetVS(), i.first.GetSuffix()}}});

    }
}

void IDSdatabase::ResetRuntimeCaches() {
    _idsDB.clear();
    _privateGlyphs.clear();
    _sameIDSHashIndex.clear();
    _sameIDSAllVariantIndex.clear();
    _sameIDSVariantIndex.clear();
    _strokeDB.clear();
    _strokeRevDB.clear();
    _strokeCountCache.clear();
    _pendingStrokeCountCache.clear();
    _cache_BasicIdeo.clear();
    _cache_IdeoSuffixLookup.clear();
    _strokeMaximumCache.clear();
    _strokeRangeCache.clear();
    _iterStack.clear();
    _strokeNeutralCompositionDB.clear();
    for(auto& source: _directComponentIndex) source.clear();
    _directComponentIndexReady = false;
    _idsDataSignature.clear();
}

bool IDSdatabase::AddRawIDS(Ideograph ideo, IDSOwner ids, std::string uniqueSeparator) {
    if(ids == nullptr || ContainsQueryOnlyOperator(ids.get())) return false;
    _rawIDSDB[ideo].push_back(std::move(ids));
    _rawIDSUniqueSeparators[ideo].push_back(std::move(uniqueSeparator));
    return true;
}

void IDSdatabase::BuildSameIDSHashIndex() {
    _sameIDSHashIndex.clear();
    _sameIDSAllVariantIndex.clear();
    _sameIDSVariantIndex.clear();

    SameIDSHashIndex candidates;
    for(const auto& glyphEntry: _rawIDSDB) {
        const auto uniqueSeparators = _rawIDSUniqueSeparators.find(glyphEntry.first);
        for(size_t ordinal = 0; ordinal < glyphEntry.second.size(); ordinal++) {
            const auto& ids = glyphEntry.second[ordinal];
            if(ids == nullptr) continue;
            const bool hasExplicitGlyphUniqueSeparator =
                uniqueSeparators != _rawIDSUniqueSeparators.end() &&
                ordinal < uniqueSeparators->second.size() &&
                IsExplicitGlyphUniqueSeparator(uniqueSeparators->second[ordinal]);
            const std::string expression = ids->toString();
            const uint64_t    hash       = IDSExpressionHash(expression);

            std::vector<SameIDSHashGroup>& groups = candidates[hash];
            SameIDSHashGroup*              group  = nullptr;
            for(auto& candidate: groups) {
                if(candidate.expression == expression) {
                    group = &candidate;
                    break;
                }
            }
            if(group == nullptr) {
                groups.push_back(SameIDSHashGroup());
                groups.back().expression = expression;
                group                    = &groups.back();
            }
            if(hasExplicitGlyphUniqueSeparator) group->notEquivalentGlyphs.insert(glyphEntry.first);

            if(std::find(group->glyphs.begin(), group->glyphs.end(), glyphEntry.first) == group->glyphs.end())
                group->glyphs.push_back(glyphEntry.first);
        }
    }

    for(auto& hashEntry: candidates) {
        for(auto& group: hashEntry.second) {
            if(group.glyphs.size() < 2) continue;
            std::sort(group.glyphs.begin(), group.glyphs.end(), IdeographCmp);
            _sameIDSHashIndex[hashEntry.first].push_back(std::move(group));
        }
    }

    // 反向索引只从“至少两个字形共享同一完整表达式”的集合生成。
    // 不同表达式即使拥有相同成员，也必须保持为不同集合；这里仅把
    // 每个字形在其所属集合中的其他成员合并到候选列表。
    for(const auto& hashEntry: _sameIDSHashIndex) {
        for(const SameIDSHashGroup& group: hashEntry.second) {
            for(const Ideograph& glyph: group.glyphs) {
                for(const Ideograph& variant: group.glyphs) {
                    std::vector<Ideograph>& allVariants = _sameIDSAllVariantIndex[glyph];
                    if(std::find(allVariants.begin(), allVariants.end(), variant) == allVariants.end())
                        allVariants.push_back(variant);

                    if(group.notEquivalentGlyphs.find(glyph) != group.notEquivalentGlyphs.end() ||
                        group.notEquivalentGlyphs.find(variant) != group.notEquivalentGlyphs.end())
                        continue;
                    std::vector<Ideograph>& variants = _sameIDSVariantIndex[glyph];
                    if(std::find(variants.begin(), variants.end(), variant) == variants.end())
                        variants.push_back(variant);
                }
            }
        }
    }
    for(auto& entry: _sameIDSAllVariantIndex)
        std::sort(entry.second.begin(), entry.second.end(), IdeographCmp);
    for(auto& entry: _sameIDSVariantIndex)
        std::sort(entry.second.begin(), entry.second.end(), IdeographCmp);
}

const IDSdatabase::SameIDSVariantIndex& IDSdatabase::GetSameIDSVariantIndex() const {
    return config.fuzzyMatch.excludeNonEquivalentSameIDS ? _sameIDSVariantIndex : _sameIDSAllVariantIndex;
}

void IDSdatabase::AppendSameIDSMatches(IDS* ids, const IDSqueryOptions& options, std::vector<Ideograph>& output) {
    if(!IsStrictIDSExpression(ids)) return;

    const std::string expression = ids->toString();
    const auto        found      = _sameIDSHashIndex.find(IDSExpressionHash(expression));
    if(found == _sameIDSHashIndex.end()) return;

    for(const SameIDSHashGroup& group: found->second) {
        if(group.expression != expression) continue;
        for(const Ideograph& glyph: group.glyphs) {
            if(config.fuzzyMatch.excludeNonEquivalentSameIDS &&
                group.notEquivalentGlyphs.find(glyph) != group.notEquivalentGlyphs.end())
                continue;
            if(!ShouldIncludeResult(glyph, options)) continue;
            if(options.filter.ignoreOverlayStructure && ContainsOverlay(ids)) continue;
            if(std::find(output.begin(), output.end(), glyph) == output.end()) output.push_back(glyph);
        }
    }
}

bool IDSdatabase::IsHVAmbiguousOrigin(Ideograph glyph) const {
    const auto rawFound = _rawIDSDB.find(glyph);
    if(rawFound == _rawIDSDB.end()) return false;

    for(const IDSOwner& idsOwner: rawFound->second) {
        if(idsOwner == nullptr) continue;
        const std::string expression = idsOwner->toString();
        const auto         found      = _sameIDSHashIndex.find(IDSExpressionHash(expression));
        if(found == _sameIDSHashIndex.end()) continue;
        for(const SameIDSHashGroup& group: found->second) {
            if(group.notEquivalentGlyphs.empty() || group.expression != expression) continue;
            if(std::find(group.glyphs.begin(), group.glyphs.end(), glyph) != group.glyphs.end()) return true;
        }
    }
    return false;
}

bool IDSdatabase::IsNonEquivalentSameIDS(Ideograph first, Ideograph second) const {
    if(first == second) return false;
    for(const auto& hashEntry: _sameIDSHashIndex)
        for(const SameIDSHashGroup& group: hashEntry.second) {
            if(group.notEquivalentGlyphs.find(first) == group.notEquivalentGlyphs.end() &&
                group.notEquivalentGlyphs.find(second) == group.notEquivalentGlyphs.end())
                continue;
            if(std::find(group.glyphs.begin(), group.glyphs.end(), first) == group.glyphs.end() ||
                std::find(group.glyphs.begin(), group.glyphs.end(), second) == group.glyphs.end())
                continue;
            if(group.expression.empty()) continue;
            return true;
        }
    return false;
}

IDSOwnerList IDSdatabase::ExpandSameIDSQuery(IDS* ids, size_t maximum) {
    IDSOwnerList out;
    if(ids == nullptr || maximum == 0) return out;

    auto append = [&](IDSOwnerList& target, IDSOwner value, IDSPreprocessRule rule = IDS_PREPROCESS_NONE) {
        if(value == nullptr) return;
        const std::string expression = value->toString();
        bool              truncated  = false;
        AppendUniqueOwned(target, std::move(value), maximum, truncated);
        if(!truncated) RecordQueryPreprocessRule(expression, rule);
    };

    std::function<IDSOwnerList(IDS*)>                             expandNode;
    std::function<std::vector<IDSOwnerList>(const IDSOwnerList&)> expandList;

    expandList = [&](const IDSOwnerList& source) {
        std::vector<IDSOwnerList> result(1);
        for(const auto& child: source) {
            const IDSOwnerList choices = expandNode(child.get());
            if(choices.empty()) return std::vector<IDSOwnerList>();

            std::vector<IDSOwnerList> next;
            for(const auto& prefix: result) {
                for(const auto& choice: choices) {
                    if(next.size() >= maximum) break;
                    IDSOwnerList combined;
                    for(const auto& item: prefix)
                        combined.push_back(item->Clone());
                    combined.push_back(choice->Clone());
                    next.push_back(std::move(combined));
                }
                if(next.size() >= maximum) break;
            }
            result = std::move(next);
            if(result.empty()) break;
        }
        return result;
    };

    expandNode = [&](IDS* value) {
        IDSOwnerList result;
        if(value == nullptr) return result;

        if(IsIdeograph(value)) {
            const Ideograph ideograph = *AsIdeograph(value);
            append(result, value->Clone());

            // 查询预处理也要展开 CJK 符号回退，否则 <search=㇤> 无法进入
            // ⺄ 的同 IDS 集合；这里与 IdeoEqual 保持“数据库已有定义时优先直连”的规则。
            if(config.misc.symFallback && ideograph.inUnicode() && _idsDB.find(ideograph) == _idsDB.end()) {
                const uint32_t fallbackCp = CJKsymFallback(ideograph.GetIdeo());
                if(fallbackCp != 0)
                    append(result, IDSOwner(new Ideograph(fallbackCp, ideograph.GetVS(), ideograph.GetSuffix())),
                        IDS_PREPROCESS_CJK_SYMBOL_FALLBACK);
            }

            const auto& variantIndex = GetSameIDSVariantIndex();
            const auto found = variantIndex.find(ideograph);
            if(found != variantIndex.end()) {
                const bool strictStrokeSuffix = IsSingleStrokeIdeograph(ideograph);
                for(const Ideograph& variant: found->second) {
                    // 单笔画字形的后缀表示具体字形，不应被同 IDS 展开忽略；
                    // 无后缀的笔画字符/汉字仍然可以互相保留等价关系。
                    if(strictStrokeSuffix && variant.GetSuffix() != ideograph.GetSuffix() &&
                        !IsCJKRadicalStrokeAlias(ideograph, variant))
                        continue;
                    append(result, IDSOwner(new Ideograph(variant)), IDS_PREPROCESS_SAME_IDS);
                }
            }
            return result;
        }

        // Replace/subtract 的三个子项有专用预处理逻辑，不能在这里改写
        // 它们的树，否则会改变“替换源”和“替换部件”的语义。
        if(IsReplaceQuery(value) || IsSubtractQuery(value)) {
            append(result, value->Clone());
            return result;
        }

        if(IsSearchExpression(value)) {
            SearchExpression* search = AsSearchExpression(value);

            std::vector<IDSOwnerList> termChoices;
            termChoices.reserve(search->GetTerms().size());
            for(const auto& term: search->GetTerms()) {
                IDSOwnerList choices = expandNode(term.get());
                if(choices.empty()) return result;
                termChoices.push_back(std::move(choices));
            }

            // 异常条件是“任一项命中即排除”，所以同一个项的
            // 相同 IDS 变体可以直接展开到同一个候选列表中。
            IDSOwnerList expandedExceptTerms;
            for(const auto& term: search->GetExceptTerms()) {
                const IDSOwnerList choices = expandNode(term.get());
                if(choices.empty()) return result;
                for(const auto& choice: choices)
                    expandedExceptTerms.push_back(choice->Clone());
            }

            if(search->GetMode() == SEARCH_EXPRESSION_ANY || search->GetMode() == SEARCH_EXPRESSION_EXCEPT) {
                IDSOwnerList expandedTerms;
                for(const IDSOwnerList& choices: termChoices)
                    for(const auto& choice: choices)
                        expandedTerms.push_back(choice->Clone());
                append(result,
                    IDSOwner(new SearchExpression(
                        std::move(expandedTerms), search->GetMode(), std::move(expandedExceptTerms))));
                return result;
            }

            // ALL 关系必须保留每个条件同时满足，因此对各条件的
            // 同 IDS 变体做笛卡尔展开，而不是把它们变成 OR。
            std::vector<IDSOwnerList> combinations(1);
            for(const IDSOwnerList& choices: termChoices) {
                std::vector<IDSOwnerList> next;
                for(const auto& prefix: combinations) {
                    for(const auto& choice: choices) {
                        if(next.size() >= maximum) break;
                        IDSOwnerList combined;
                        for(const auto& item: prefix)
                            combined.push_back(item->Clone());
                        combined.push_back(choice->Clone());
                        next.push_back(std::move(combined));
                    }
                    if(next.size() >= maximum) break;
                }
                combinations = std::move(next);
                if(combinations.empty()) break;
            }
            for(const auto& combination: combinations) {
                IDSOwnerList terms;
                for(const auto& term: combination)
                    terms.push_back(term->Clone());
                IDSOwnerList exceptTerms;
                for(const auto& term: expandedExceptTerms)
                    exceptTerms.push_back(term->Clone());
                append(result,
                    IDSOwner(new SearchExpression(std::move(terms), search->GetMode(), std::move(exceptTerms))));
                if(result.size() >= maximum) break;
            }
            return result;
        }

        if(!IsPattern(value)) {
            append(result, value->Clone());
            return result;
        }

        Pattern*                        pattern      = AsPattern(value);
        const std::vector<IDSOwnerList> childChoices = expandList(pattern->GetpIDSRef());
        for(const IDSOwnerList& children: childChoices) {
            std::vector<IDS*> childPointers;
            for(const auto& child: children)
                childPointers.push_back(child.get());

            int overlayRange[2] = {0, 0};
            pattern->GetOverlayRange(overlayRange);
            append(result,
                IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(), overlayRange,
                    pattern->GetOverlayType(), pattern->GetOptionalInt())));
            if(result.size() >= maximum) break;
        }
        return result;
    };

    return expandNode(ids);
}

namespace {

bool HasLowercaseStrokeSuffix(const Ideograph& glyph) {
    return glyph.GetSuffix().find_first_of(ASCII_LOWERCASE) != std::string::npos;
}

IDSOwner CloneStrokeNeutralNode(IDS* value) {
    if(value == nullptr) return nullptr;
    // 输出缓存必须保留小写后缀；后缀中性只用于下面的比较 key。
    if(IsIdeograph(value)) return value->Clone();
    if(!IsPattern(value)) return value->Clone();

    Pattern* pattern = AsPattern(value);
    IDSOwnerList children;
    std::vector<IDS*> childPointers;
    for(const auto& child: pattern->GetpIDSRef()) {
        children.push_back(CloneStrokeNeutralNode(child.get()));
        childPointers.push_back(children.back().get());
    }
    int overlayRange[2] = {0, 0};
    pattern->GetOverlayRange(overlayRange);
    // HV origin ranges are intentionally omitted: they describe the source
    // tree and are not valid after a component has been replaced.
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(), overlayRange,
        pattern->GetOverlayType(), pattern->GetOptionalInt()));
}

IDSOwner CloneStrokeNeutralKeyNode(IDS* value) {
    if(value == nullptr) return nullptr;
    if(IsIdeograph(value)) {
        const Ideograph glyph = *AsIdeograph(value);
        if(!HasLowercaseStrokeSuffix(glyph)) return value->Clone();
        if(glyph.inUnicode())
            return IDSOwner(new Ideograph(glyph.GetIdeo(), glyph.GetVS()));
        return IDSOwner(new Ideograph(glyph.GetAbstractName()));
    }
    if(!IsPattern(value)) return value->Clone();

    Pattern* pattern = AsPattern(value);
    IDSOwnerList children;
    std::vector<IDS*> childPointers;
    for(const auto& child: pattern->GetpIDSRef()) {
        children.push_back(CloneStrokeNeutralKeyNode(child.get()));
        childPointers.push_back(children.back().get());
    }
    int overlayRange[2] = {0, 0};
    pattern->GetOverlayRange(overlayRange);
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(), overlayRange,
        pattern->GetOverlayType(), pattern->GetOptionalInt()));
}

std::string StrokeNeutralExpression(IDS* value) {
    IDSOwner normalized = CloneStrokeNeutralKeyNode(value);
    return normalized == nullptr ? std::string() : normalized->toString();
}

void AppendUniqueStrokeNeutralOwner(IDSOwnerList& output, IDSOwner value, size_t maximum) {
    if(value == nullptr || output.size() >= maximum) return;
    const std::string expression = value->toString();
    for(const auto& existing: output)
        if(existing != nullptr && existing->toString() == expression) return;
    output.push_back(std::move(value));
}

IDSOwner MakeStrokeNeutralPattern(Pattern* source, const IDSOwnerList& children) {
    std::vector<IDS*> childPointers;
    childPointers.reserve(children.size());
    for(const auto& child: children) childPointers.push_back(child.get());
    int overlayRange[2] = {0, 0};
    source->GetOverlayRange(overlayRange);
    return IDSOwner(new Pattern(source->GetIDC(), childPointers, source->GetPreferSplitPoint(), overlayRange,
        source->GetOverlayType(), source->GetOptionalInt()));
}

IDSOwnerList ComposeStrokeNeutralNode(
    IDS* value, const std::unordered_map<std::string, std::vector<Ideograph>>& componentIndex,
    const Ideograph& targetGlyph, size_t maximum, bool allowWholePattern) {
    IDSOwnerList output;
    if(value == nullptr || maximum == 0) return output;
    if(!IsPattern(value)) {
        AppendUniqueStrokeNeutralOwner(output, CloneStrokeNeutralNode(value), maximum);
        return output;
    }

    Pattern* pattern = AsPattern(value);
    std::vector<IDSOwnerList> choicesByChild;
    choicesByChild.reserve(pattern->GetpIDSRef().size());
    for(const auto& child: pattern->GetpIDSRef()) {
        IDSOwnerList choices = ComposeStrokeNeutralNode(child.get(), componentIndex, targetGlyph, maximum, true);
        if(choices.empty()) return output;
        choicesByChild.push_back(std::move(choices));
    }

    std::vector<IDSOwnerList> combinations(1);
    for(const IDSOwnerList& choices: choicesByChild) {
        std::vector<IDSOwnerList> next;
        for(const auto& prefix: combinations) {
            for(const auto& choice: choices) {
                if(next.size() >= maximum) break;
                IDSOwnerList combined;
                for(const auto& item: prefix) combined.push_back(item->Clone());
                combined.push_back(choice->Clone());
                next.push_back(std::move(combined));
            }
            if(next.size() >= maximum) break;
        }
        combinations = std::move(next);
        if(combinations.empty()) return output;
    }

    for(const IDSOwnerList& children: combinations) {
        bool unchanged = children.size() == pattern->GetpIDSRef().size();
        if(unchanged) {
            for(size_t index = 0; index < children.size(); index++) {
                if(children[index] == nullptr ||
                    children[index]->toString() != pattern->GetpIDSRef()[index]->toString()) {
                    unchanged = false;
                    break;
                }
            }
        }
        // 没有替换任何子树时保留原 HVOriginRange；它可能包含 IDS.pdf 7.1 的唯一化边界。
        IDSOwner composed = unchanged ? value->Clone() : MakeStrokeNeutralPattern(pattern, children);
        const std::string key = allowWholePattern ? StrokeNeutralExpression(composed.get()) : std::string();
        AppendUniqueStrokeNeutralOwner(output, std::move(composed), maximum);

        if(!allowWholePattern) continue;
        const auto found = componentIndex.find(key);
        if(found == componentIndex.end()) continue;
        for(const Ideograph& component: found->second) {
            // Never fold the whole target expression back into its owner glyph.
            if(component == targetGlyph) continue;
            AppendUniqueStrokeNeutralOwner(output, IDSOwner(new Ideograph(component)), maximum);
        }
    }
    return output;
}

} // namespace

void IDSdatabase::BuildStrokeNeutralCompositionCacheForGlyphs(
    const IdeographSet& glyphs, IDSStorage& output) {
    output.clear();
    std::unordered_map<std::string, std::vector<Ideograph>> componentIndex;

    // Index only suffixless glyphs. Locale variants and lowercase stroke
    // variants remain available in the exact/HV cache instead of becoming
    // ambiguous component names here.
    for(const auto& entry: _rawIDSDB) {
        if(!entry.first.GetSuffix().empty()) continue;
        const auto separators = _rawIDSUniqueSeparators.find(entry.first);
        bool hasExplicitSeparator = false;
        if(separators != _rawIDSUniqueSeparators.end()) {
            for(const std::string& separator: separators->second)
                if(IsExplicitGlyphUniqueSeparator(separator)) {
                    hasExplicitSeparator = true;
                    break;
                }
        }
        if(hasExplicitSeparator) continue;
        for(const auto& ids: entry.second) {
            if(!IsPattern(ids.get())) continue;
            const std::string key = StrokeNeutralExpression(ids.get());
            if(key.empty()) continue;
            std::vector<Ideograph>& candidates = componentIndex[key];
            if(std::find(candidates.begin(), candidates.end(), entry.first) == candidates.end())
                candidates.push_back(entry.first);
        }
    }

    for(const Ideograph& glyph: glyphs) {
        const auto cached = _idsDB.find(glyph);
        if(cached == _idsDB.end()) continue;
        IDSOwnerList& composedEntries = output[glyph];
        for(const auto& source: cached->second) {
            IDSOwnerList alternatives = ComposeStrokeNeutralNode(source.get(), componentIndex, glyph, 64, false);
            for(auto& alternative: alternatives) {
                if(alternative == nullptr || alternative->toString() == source->toString()) continue;
                alternative->SetUniqueSeparator(source->GetUniqueSeparator());
                bool ignored = false;
                AppendUniqueOwned(composedEntries, std::move(alternative), std::numeric_limits<size_t>::max(), ignored);
            }
        }
        if(composedEntries.empty()) output.erase(glyph);
    }
}

void IDSdatabase::BuildQueryCacheForGlyphs(const IdeographSet& glyphs, IDSStorage& output) {
    for(const Ideograph& glyph: glyphs) {
        const auto rawEntries = _rawIDSDB.find(glyph);
        if(rawEntries == _rawIDSDB.end()) continue;

        IDSOwnerList& cachedEntries = output[glyph];
        _lastImportReport.rebuiltCacheGlyphs++;
        const auto uniqueSeparators = _rawIDSUniqueSeparators.find(glyph);
        for(size_t ordinal = 0; ordinal < rawEntries->second.size(); ordinal++) {
            const auto& rawIds = rawEntries->second[ordinal];
            const std::string uniqueSeparator =
                uniqueSeparators != _rawIDSUniqueSeparators.end() && ordinal < uniqueSeparators->second.size()
                    ? uniqueSeparators->second[ordinal]
                    : "";
            bool         truncated    = false;
            IDSOwnerList alternatives = HVExtractOwned(rawIds.get(), true, false, 64, &truncated);
            if(alternatives.empty()) alternatives.push_back(rawIds->Clone());
            if(truncated) _lastImportReport.cacheTruncations++;
            for(auto& alternative: alternatives) {
                if(!uniqueSeparator.empty()) alternative->SetUniqueSeparator(uniqueSeparator);
                bool ignored = false;
                if(AppendUniqueOwned(
                       cachedEntries, std::move(alternative), std::numeric_limits<size_t>::max(), ignored))
                    _lastImportReport.queryCacheEntries++;
            }
        }
    }
}

bool IDSdatabase::BuildQueryCacheFromRaw(const IDSImportStageCallback& progress) {
    if(_rawIDSDB.empty()) {
        _lastError = "Cannot build an HV query cache without raw IDS entries.";
        return false;
    }

    // HV extraction resolves components through FindIDS(). Temporarily expose the
    // raw definitions so every alternative starts from the exact lv0 source.
    _idsDB.clear();
    for(const auto& entry: _rawIDSDB)
        for(const auto& ids: entry.second)
            _idsDB[entry.first].push_back(ids->Clone());
    MakeCache();

    IdeographSet allGlyphs;
    for(const auto& entry: _rawIDSDB)
        allGlyphs.insert(entry.first);

    IDSStorage queryCache;
    _lastImportReport.queryCacheEntries  = 0;
    _lastImportReport.rebuiltCacheGlyphs = 0;
    _lastImportReport.cacheTruncations   = 0;
    if(progress) progress(IDSimportStage::HVCache);
    BuildQueryCacheForGlyphs(allGlyphs, queryCache);

    _idsDB = std::move(queryCache);
    _iterStack.clear();
    MakeCache();
    _strokeNeutralCompositionDB.clear();
    if(progress) progress(IDSimportStage::StrokeNeutralCache);
    BuildStrokeNeutralCompositionCacheForGlyphs(allGlyphs, _strokeNeutralCompositionDB);
    if(progress) progress(IDSimportStage::ComponentIndex);
    BuildDirectComponentIndex();
    return true;
}

bool IDSdatabase::RebuildQueryCache() {
    return BuildQueryCacheFromRaw();
}

void IDSdatabase::CollectReferencedIdeographs(IDS* ids, IdeographSet& references) const {
    if(ids == nullptr) return;
    if(IsIdeograph(ids)) {
        references.insert(*AsIdeograph(ids));
        return;
    }
    if(!IsPattern(ids)) return;
    for(const auto& child: AsPattern(ids)->GetpIDSRef())
        CollectReferencedIdeographs(child.get(), references);
}

IDSdatabase::IdeographSet IDSdatabase::FindAffectedCacheGlyphs(const IdeographSet& changedGlyphs) const {
    std::unordered_map<Ideograph, std::vector<Ideograph>, Ideograph_Hash> dependents;
    for(const auto& entry: _rawIDSDB) {
        IdeographSet references;
        for(const auto& ids: entry.second)
            CollectReferencedIdeographs(ids.get(), references);
        for(const Ideograph& reference: references)
            dependents[reference].push_back(entry.first);
    }

    IdeographSet           affected;
    std::vector<Ideograph> pending;
    pending.reserve(changedGlyphs.size());
    for(const Ideograph& glyph: changedGlyphs)
        pending.push_back(glyph);

    while(!pending.empty()) {
        const Ideograph glyph = pending.back();
        pending.pop_back();
        if(!affected.insert(glyph).second) continue;

        const auto dependent = dependents.find(glyph);
        if(dependent == dependents.end()) continue;
        for(const Ideograph& owner: dependent->second)
            pending.push_back(owner);
    }
    return affected;
}

void IDSdatabase::RebuildAffectedQueryCache(
    const IdeographSet& changedGlyphs, const IDSImportStageCallback& progress) {
    const IdeographSet affectedGlyphs = FindAffectedCacheGlyphs(changedGlyphs);
    if(affectedGlyphs.empty()) return;

    // 生成局部缓存时必须从完整的原始 IDS 视图展开，随后仅替换受影响条目。
    IDSStorage previousCache = std::move(_idsDB);
    _idsDB.clear();
    for(const auto& entry: _rawIDSDB)
        for(const auto& ids: entry.second)
            _idsDB[entry.first].push_back(ids->Clone());
    MakeCache();

    IDSStorage refreshedEntries;
    _lastImportReport.queryCacheEntries  = 0;
    _lastImportReport.rebuiltCacheGlyphs = 0;
    _lastImportReport.cacheTruncations   = 0;
    if(progress) progress(IDSimportStage::HVCache);
    BuildQueryCacheForGlyphs(affectedGlyphs, refreshedEntries);

    _idsDB = std::move(previousCache);
    for(const Ideograph& glyph: affectedGlyphs) {
        const auto refreshed = refreshedEntries.find(glyph);
        if(refreshed == refreshedEntries.end())
            _idsDB.erase(glyph);
        else
            _idsDB[glyph] = std::move(refreshed->second);
    }
    _iterStack.clear();
    MakeCache();

    IDSStorage previousComposed = std::move(_strokeNeutralCompositionDB);
    IDSStorage refreshedComposed;
    if(progress) progress(IDSimportStage::StrokeNeutralCache);
    BuildStrokeNeutralCompositionCacheForGlyphs(affectedGlyphs, refreshedComposed);
    _strokeNeutralCompositionDB = std::move(previousComposed);
    for(const Ideograph& glyph: affectedGlyphs) {
        const auto refreshed = refreshedComposed.find(glyph);
        if(refreshed == refreshedComposed.end())
            _strokeNeutralCompositionDB.erase(glyph);
        else
            _strokeNeutralCompositionDB[glyph] = std::move(refreshed->second);
    }
    if(progress) progress(IDSimportStage::ComponentIndex);
    BuildDirectComponentIndex();
}

int IDSdatabase::ImportDB(std::string filename, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    return ImportDB(MakeTextIDSReader(filename, dbformat), dbformat, std::move(progress));
}

int IDSdatabase::ImportDB(IDSImportReader reader, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    _rawIDSDB.clear();
    ResetRuntimeCaches();
    _lastImportReport = IDSimportReport();
    _lastError.clear();
    _format = dbformat;
    ConfigureDatabaseFormat(config, _format);

    if(progress) progress(IDSimportStage::Reading);
    if(!ParseIDSReader(reader, _rawIDSDB, _rawIDSUniqueSeparators)) return -1;
    if(_rawIDSDB.empty()) {
        _lastError = "No valid IDS expressions were imported.";
        return -1;
    }
    BuildSameIDSHashIndex();
    if(!BuildQueryCacheFromRaw(progress)) return -1;
    if(progress) progress(IDSimportStage::StrokeCache);
    MakeStrokeDB();
    MakeStrokeRevDB();
    MakeStrokeCountCache();
    if(progress) progress(IDSimportStage::Saving);
    if(!SaveSqliteDatabase("db/" + name + ".sqlite")) return -2;
    if(progress) progress(IDSimportStage::Complete);
    return 0;
}

int IDSdatabase::ImportPrivateDB(std::string filename, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    return ImportPrivateDB(MakeTextIDSReader(filename, dbformat), dbformat, std::move(progress));
}

int IDSdatabase::ImportPrivateDB(IDSImportReader reader, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    return ImportPrivateDBImpl(std::move(reader), dbformat, false, progress);
}

int IDSdatabase::ReimportPrivateDB(std::string filename, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    return ReimportPrivateDB(MakeTextIDSReader(filename, dbformat), dbformat, std::move(progress));
}

int IDSdatabase::ReimportPrivateDB(IDSImportReader reader, IDSdbFormat dbformat, IDSImportStageCallback progress) {
    return ImportPrivateDBImpl(std::move(reader), dbformat, true, progress);
}

int IDSdatabase::ImportPrivateDBImpl(
    IDSImportReader reader, IDSdbFormat dbformat, bool replaceExisting, const IDSImportStageCallback& progress) {
    (void)dbformat; // 自定义读取器已经负责其格式解析，参数保留用于接口一致性和后续扩展。
    if(_rawIDSDB.empty() || _idsDB.empty()) {
        _lastError = "Cannot import a private IDS library into an empty database.";
        return -1;
    }

    _lastImportReport = IDSimportReport();
    _lastError.clear();
    IDSStorage privateEntries;
    IDSUniqueSeparatorStorage privateUniqueSeparators;
    if(progress) progress(IDSimportStage::Reading);
    if(!ParseIDSReader(reader, privateEntries, privateUniqueSeparators)) return -1;
    if(_lastImportReport.HasIssues() || _lastImportReport.rejectedExpressions != 0) {
        _lastError = "Private IDS import was rejected; no database changes were written.";
        return -1;
    }
    if(privateEntries.empty()) {
        _lastError = "No valid private IDS expressions were imported.";
        return -1;
    }

    bool hasBaseConflict = false;
    for(const auto& entry: privateEntries) {
        if(_rawIDSDB.find(entry.first) == _rawIDSDB.end() || _privateGlyphs.find(entry.first) != _privateGlyphs.end())
            continue;
        hasBaseConflict = true;
        if(_lastImportReport.issues.size() < 16) {
            IDSimportIssue issue;
            issue.glyph   = entry.first.toString();
            issue.message = "private IDS entries cannot replace a base-library glyph";
            _lastImportReport.issues.push_back(issue);
        }
    }
    if(hasBaseConflict) {
        _lastError = "Private IDS import conflicts with one or more base-library glyphs.";
        return -1;
    }

    IdeographSet changedGlyphs;
    if(replaceExisting) {
        for(const Ideograph& glyph: _privateGlyphs) {
            changedGlyphs.insert(glyph);
            _rawIDSDB.erase(glyph);
            _rawIDSUniqueSeparators.erase(glyph);
        }
        _privateGlyphs.clear();
    }

    for(auto& entry: privateEntries) {
        changedGlyphs.insert(entry.first);
        _rawIDSDB[entry.first] = std::move(entry.second);
        _rawIDSUniqueSeparators[entry.first] = std::move(privateUniqueSeparators[entry.first]);
        _privateGlyphs.insert(entry.first);
    }

    RebuildAffectedQueryCache(changedGlyphs, progress);
    if(progress) progress(IDSimportStage::StrokeCache);
    MakeStrokeDB();
    MakeStrokeRevDB();
    _strokeCountCache.clear();
    _strokeRangeCache.clear();
    _pendingStrokeCountCache.clear();
    MakeStrokeCountCache();
    BuildSameIDSHashIndex();
    if(progress) progress(IDSimportStage::Saving);
    if(!SaveSqliteDatabase("db/" + name + ".sqlite")) return -2;
    if(progress) progress(IDSimportStage::Complete);
    return 0;
}
void IDSdatabase::MakeStrokeDB() {
    _strokeDB.clear();
    for(const auto& i: _rawIDSDB) {
        std::vector<std::string> strokeVec;
        const auto&              pIDSVec = i.second;
        if(i.first.GetSuffix() == "" && pIDSVec.size() == 1 && IsStroke(pIDSVec[0].get()) &&
            AsStroke(pIDSVec[0].get())->isBasicStroke()) {
            for(auto j: AsStroke(pIDSVec[0].get())->GetStroke())
                strokeVec.push_back((j.neg ? "-" : "") + j.stroke);
            _strokeDB.insert({i.first.toString(), strokeVec});
        }
    }
}

std::vector<IDS*> IDSdatabase::FindIDS(Ideograph ideo, bool ignoreSuffix, bool noFallback) const {
    std::vector<IDS*> found;
    std::vector<IDS*> out;
    // Search IDS by codepoint of ideograph
    if(ignoreSuffix && !noFallback) {
        Ideograph  pureIdeo       = ideo.inUnicode() ? Ideograph(ideo.GetIdeo()) : Ideograph(ideo.GetAbstractName());
        const auto suffixVariants = _cache_IdeoSuffixLookup.find(pureIdeo);
        if(suffixVariants != _cache_IdeoSuffixLookup.end()) {
            for(auto i: suffixVariants->second) {
                Ideograph  fullIdeo  = ideo.inUnicode() ? Ideograph(ideo.GetIdeo(), i.vs, i.suffix)
                                                        : Ideograph(ideo.GetAbstractName() + "." + i.suffix);
                const auto fullFound = _idsDB.find(fullIdeo);
                if(fullFound != _idsDB.end()) AppendBorrowedIDS(found, fullFound->second);
            }
        } else if(_cache_IdeoSuffixLookup.empty())
            for(const auto& i: _idsDB)
                if(i.first.GetIdeo() == ideo.GetIdeo()) AppendBorrowedIDS(found, i.second);
    }
    // Generic: region/suffix fallback
    else {
        auto foundIt = _idsDB.find(ideo);
        if(foundIt != _idsDB.end()) {
            AppendBorrowedIDS(found, foundIt->second);
            if(config.misc.suffixRisAltForm) {
                Ideograph rIdeo  = ideo.inUnicode() ? Ideograph(ideo.GetIdeo(), ideo.GetVS(), ideo.GetSuffix() + "r")
                                                    : Ideograph(ideo.GetAbstractName() + "." + ideo.GetSuffix() + "r");
                auto      foundR = _idsDB.find(rIdeo);
                if(foundR != _idsDB.end()) AppendBorrowedIDS(found, foundR->second);
            }
        } else {
            const Ideograph pureIdeo = ideo.inUnicode() ? Ideograph(ideo.GetIdeo()) : Ideograph(ideo.GetAbstractName());
            auto          appendSuffix = [&](const std::string& suffix) {
                const Ideograph regionIdeo = suffix.empty()
                    ? pureIdeo
                    : (ideo.inUnicode() ? Ideograph(ideo.GetIdeo(), 0, suffix)
                                        : Ideograph(ideo.GetAbstractName() + "." + suffix));
                const auto foundRegion = _idsDB.find(regionIdeo);
                if(foundRegion == _idsDB.end()) return false;
                AppendBorrowedIDS(found, foundRegion->second);
                return true;
            };

            // 有序回退是显式配置才启用的；默认仍保持 lv0 精确后缀和旧回退逻辑。
            if(!config.fuzzyMatch.localeSuffixFallbackOrder.empty()) {
                const auto fallbackGroups =
                    LocaleSuffixFallbackGroups(config.fuzzyMatch.localeSuffixFallbackOrder);
                for(const auto& group: fallbackGroups) {
                    bool foundAtThisLevel = false;
                    for(const std::string& suffix: group)
                        foundAtThisLevel = appendSuffix(suffix) || foundAtThisLevel;
                    if(foundAtThisLevel) break;
                }
            } else if(!config.fuzzyMatch.defaultRegion.empty()) {
                appendSuffix(config.fuzzyMatch.defaultRegion);
            }
            if(found.empty()) {
                const auto foundPure = _idsDB.find(pureIdeo);
                if(foundPure != _idsDB.end())
                    AppendBorrowedIDS(found, foundPure->second);
                else if(!noFallback)
                    return FindIDS(ideo, true);
            }
        }
    }
    // Fallback
    if(config.misc.symFallback && found.empty()) {
        uint32_t fallbackCp = CJKsymFallback(ideo.GetIdeo());
        if(fallbackCp != 0) return FindIDS(Ideograph(fallbackCp), ignoreSuffix, noFallback);
    }
    // Remove duplicate IDSes without cloning borrowed database nodes.
    out.reserve(found.size());
    for(auto i: found) {
        // Recursive component expansion must not use an overlay-composed definition.
        if(_ignoreOverlayStructureForMatch && ContainsOverlay(i)) continue;
        bool foundDup = false;
        for(auto j: out)
            if(IDSequal(i, j)) foundDup = true;
        if(!foundDup) out.push_back(i);
    }
    return out;
}

IDSOwnerList IDSdatabase::GetRawIDSOwned(Ideograph ideo, bool ignoreSuffix, bool noFallback) {
    IDSOwnerList out;
    auto         append = [&](const IDSOwnerList& entries) {
        for(const auto& entry: entries)
            out.push_back(entry->Clone());
    };

    const auto direct = _rawIDSDB.find(ideo);
    if(direct != _rawIDSDB.end()) append(direct->second);
    if(out.empty() && ignoreSuffix) {
        const Ideograph pure = ideo.inUnicode() ? Ideograph(ideo.GetIdeo()) : Ideograph(ideo.GetAbstractName());
        for(const auto& entry: _rawIDSDB)
            if(entry.first.GetPured() == pure) append(entry.second);
    }
    if(out.empty() && !ignoreSuffix && !noFallback) return GetRawIDSOwned(ideo, true, true);
    return out;
}

IDSOwnerList IDSdatabase::GetIDSOwned(Ideograph ideo, bool ignoreSuffix, bool noFallback) {
    std::vector<IDS*> found = FindIDS(ideo, ignoreSuffix, noFallback);
    IDSOwnerList      out;
    out.reserve(found.size());
    for(auto i: found)
        out.push_back(i->Clone());
    return out;
}

std::vector<IDS*> IDSdatabase::GetIDS(Ideograph ideo, bool ignoreSuffix, bool noFallback) {
    return ReleaseIDSList(GetIDSOwned(ideo, ignoreSuffix, noFallback));
}

std::vector<Ideograph> IDSdatabase::GetContainingCharacters(Ideograph ideo, const IDSqueryOptions& options) {
    IDSOwnerList terms;
    terms.push_back(IDSOwner(new Ideograph(ideo)));
    std::vector<Ideograph> result = Search(terms);
    result.erase(std::remove(result.begin(), result.end(), ideo), result.end());
    result.erase(std::remove_if(result.begin(), result.end(),
                     [this, &options](const Ideograph& value) { return !ShouldIncludeResult(value, options); }),
        result.end());
    ApplyResultFilter(result, options);
    return result;
}

std::vector<Ideograph> IDSdatabase::GetSameIDSCharacters(Ideograph ideo) const {
    std::vector<Ideograph> result;
    for(const auto& hashEntry: _sameIDSHashIndex) {
        for(const SameIDSHashGroup& group: hashEntry.second) {
            if(config.fuzzyMatch.excludeNonEquivalentSameIDS &&
                group.notEquivalentGlyphs.find(ideo) != group.notEquivalentGlyphs.end())
                continue;
            if(std::find(group.glyphs.begin(), group.glyphs.end(), ideo) == group.glyphs.end()) continue;
            for(const Ideograph& glyph: group.glyphs)
                if((!config.fuzzyMatch.excludeNonEquivalentSameIDS ||
                    group.notEquivalentGlyphs.find(glyph) == group.notEquivalentGlyphs.end()) &&
                    std::find(result.begin(), result.end(), glyph) == result.end())
                    result.push_back(glyph);
        }
    }
    std::sort(result.begin(), result.end(), IdeographCmp);
    return result;
}
std::vector<Ideograph> IDSdatabase::GetChar(IDS* ids) {
    std::vector<Ideograph> out;
    for(const auto& i: _idsDB)
        for(const auto& j: i.second)
            if(IDSequal(ids, j.get())) {
                out.push_back(i.first);
                break;
            }
    return out;
}
typedef struct {
    std::string stroke;
    size_t      idx;
} StrokeAndIndex;

template<typename T>
size_t VectorFind(std::vector<T> vec, T find) {
    for(size_t i = 0; i < vec.size(); i++)
        if(find == vec[i]) return i;
    return SIZE_MAX;
}

Stroke IDSdatabase::StrokeSimplify(Stroke stroke) {
    if(stroke.isBasicStroke()) return stroke;
    // Convert stroke-ideograph to String
    std::vector<std::string> strokeVec;
    // strokeVec.reserve(stroke.GetStroke().size());
    for(auto i: stroke.GetStroke()) {
        std::string strokeFound;
        if(_strokeDB.find(i.stroke) != _strokeDB.end())
            for(auto j: _strokeDB[i.stroke]) {
                if(!i.neg)
                    strokeFound += j;
                else
                    strokeFound = "-" + j + strokeFound;
            }
        else
            strokeFound = (i.neg ? "-" : "") + i.stroke;
        strokeVec.push_back(strokeFound);
    }
    // Simplifying
    std::string                 buffer;
    size_t                      tempIdx = SIZE_MAX;
    std::vector<StrokeAndIndex> newVecTemp;
    std::vector<size_t>         crossIdxList;
    // crossIdxList.reserve(2 * stroke.GetCrossData().size());
    for(auto i: stroke.GetCrossData())
        crossIdxList.push_back(i.pos), crossIdxList.push_back(i.cross);
    for(size_t i = 0; i < strokeVec.size(); i++) {
        if(buffer == "") tempIdx = i;
        buffer += strokeVec[i];
        if(i == strokeVec.size() - 1 || VectorFind<size_t>(stroke.GetBreakPos(), i) != SIZE_MAX ||
            VectorFind<size_t>(crossIdxList, i) != SIZE_MAX || VectorFind<size_t>(crossIdxList, i + 1) != SIZE_MAX ||
            _strokeRevDB.find(buffer + strokeVec[i + 1]) == _strokeRevDB.end()) {
            if(_strokeRevDB.find(buffer) != _strokeRevDB.end())
                newVecTemp.push_back({_strokeRevDB[buffer], tempIdx});
            else
                newVecTemp.push_back({buffer, tempIdx});
            buffer = "", tempIdx = SIZE_MAX;
        }
    }
    // Construct new data for new Stroke()
    std::vector<Stroke_Data>      newVec;
    std::vector<size_t>           newBreakPos;
    std::vector<Stroke_CrossData> newCrossData;
    // newVec.reserve(newVecTemp.size());
    for(size_t i = 0; i < newVecTemp.size(); i++) {
        std::string newStroke = newVecTemp[i].stroke;
        newVec.push_back({newStroke[0] == '-', newStroke.substr(newStroke[0] == '-')});
        if(VectorFind(stroke.GetBreakPos(), newVecTemp[i].idx) != SIZE_MAX) newBreakPos.push_back(i);
        for(size_t j = 0; j < crossIdxList.size(); j++)
            if(newVecTemp[i].idx == crossIdxList[j]) crossIdxList[j] = i;
    }
    // newCrossData.reserve((1 + crossIdxList.size()) / 2);
    for(size_t i = 0; i < crossIdxList.size(); i += 2)
        newCrossData.push_back({crossIdxList[i], crossIdxList[i + 1]});
    return Stroke(newVec, newBreakPos, newCrossData, stroke.isEnclosed());
}

bool IDSdatabase::HasSuffixVariants(Ideograph ideo) {
    if(!ideo.inUnicode() || ideo.GetVS() != 0 || !ideo.GetSuffix().empty()) return false;

    const Ideograph pureIdeo(ideo.GetIdeo());
    if(!_cache_IdeoSuffixLookup.empty()) {
        const auto found = _cache_IdeoSuffixLookup.find(pureIdeo);
        if(found == _cache_IdeoSuffixLookup.end()) return false;
        for(const VSandSuffix& variant: found->second)
            if(variant.vs != 0 || !variant.suffix.empty()) return true;
        return false;
    }

    for(const auto& entry: _idsDB)
        if(entry.first.inUnicode() && entry.first.GetIdeo() == ideo.GetIdeo() &&
            (entry.first.GetVS() != 0 || !entry.first.GetSuffix().empty()))
            return true;
    return false;
}
static IDCtype HVArrangementType(const Pattern* pattern) {
    if(pattern == nullptr) return IDC_UNKNOWN;
    if(pattern->GetIDC() == IDC_LEFT_RIGHT || pattern->GetIDC() == IDC_LEFT_MIDDLE_RIGHT ||
        pattern->GetIDC() == IDC_HORIZONAL_ARRANGE)
        return IDC_HORIZONAL_ARRANGE;
    if(pattern->GetIDC() == IDC_ABOVE_BELOW || pattern->GetIDC() == IDC_ABOVE_MIDDLE_BELOW ||
        pattern->GetIDC() == IDC_VERTICAL_ARRANGE)
        return IDC_VERTICAL_ARRANGE;
    return IDC_UNKNOWN;
}

IDSOwnerList IDSdatabase::HVExtractAlternativesInternal(
    IDS* ids, bool firstLayer, bool preserveAmbiguousGlyphs, size_t maximum, bool& truncated) {
    IDSOwnerList out;
    if(ids == nullptr || maximum == 0) return out;

    if(IsSearchExpression(ids)) {
        SearchExpression* search = AsSearchExpression(ids);
        IDSOwnerList      terms;
        IDSOwnerList      exceptTerms;
        for(const auto& term: search->GetTerms()) {
            IDSOwnerList alternatives =
                HVExtractAlternativesInternal(term.get(), false, preserveAmbiguousGlyphs, maximum, truncated);
            if(alternatives.empty()) return IDSOwnerList();
            terms.push_back(std::move(alternatives.front()));
        }
        for(const auto& term: search->GetExceptTerms()) {
            IDSOwnerList alternatives =
                HVExtractAlternativesInternal(term.get(), false, preserveAmbiguousGlyphs, maximum, truncated);
            if(alternatives.empty()) return IDSOwnerList();
            exceptTerms.push_back(std::move(alternatives.front()));
        }
        out.push_back(IDSOwner(new SearchExpression(std::move(terms), search->GetMode(), std::move(exceptTerms))));
        return out;
    }

    if(!IsPattern(ids)) {
        out.push_back(ids->Clone());
        return out;
    }

    struct HVSegment {
        IDSOwnerList               children;
        std::vector<HVOriginRange> originRanges;
        bool                       fromExpansion = false;
    };
    typedef std::vector<HVSegment> SegmentChoices;
    struct HVState {
        IDSOwnerList children;
        std::vector<HVOriginRange> originRanges;
        size_t       firstSegmentLength = 0;
    };

    Pattern*                    pattern        = AsPattern(ids);
    const IDCtype               splitType      = HVArrangementType(pattern);
    const IDSOwnerList&         sourceChildren = pattern->GetpIDSRef();
    std::vector<SegmentChoices> choicesByChild;
    choicesByChild.reserve(sourceChildren.size());

    auto appendSegment = [&](SegmentChoices& choices, HVSegment segment) {
        for(const HVSegment& existing: choices) {
            if(existing.children.size() != segment.children.size()) continue;
            bool equal = true;
            for(size_t index = 0; index < existing.children.size(); index++)
                if(!IDSequal(existing.children[index].get(), segment.children[index].get())) {
                    equal = false;
                    break;
                }
            if(equal) {
                if(existing.originRanges.size() != segment.originRanges.size()) continue;
                for(size_t index = 0; index < existing.originRanges.size(); index++)
                    if(existing.originRanges[index].glyph != segment.originRanges[index].glyph ||
                        existing.originRanges[index].first != segment.originRanges[index].first ||
                        existing.originRanges[index].last != segment.originRanges[index].last) {
                        equal = false;
                        break;
                    }
                if(equal) return;
        }
        }
        if(choices.size() >= maximum) {
            truncated = true;
            return;
        }
        choices.push_back(std::move(segment));
    };
    auto appendSingleton = [&](SegmentChoices& choices, IDS* child) {
        HVSegment segment;
        segment.children.push_back(child->Clone());
        appendSegment(choices, std::move(segment));
    };
    auto isAmbiguousOrigin = [&](const Ideograph& ideograph) { return IsHVAmbiguousOrigin(ideograph); };


    for(const auto& childOwner: sourceChildren) {
        IDS*           child = childOwner.get();
        SegmentChoices choices;

        if(splitType == IDC_UNKNOWN) {
            IDSOwnerList alternatives = IsPattern(child)
                ? HVExtractAlternativesInternal(child, false, preserveAmbiguousGlyphs, maximum, truncated)
                : IDSOwnerList();
            if(alternatives.empty()) {
                appendSingleton(choices, child);
            } else {
                for(const auto& alternative: alternatives) {
                    HVSegment segment;
                    segment.children.push_back(alternative->Clone());
                    appendSegment(choices, std::move(segment));
                }
            }
            choicesByChild.push_back(std::move(choices));
            continue;
        }

        if(IsIdeograph(child)) {
            const Ideograph ideograph       = *AsIdeograph(child);
            const bool      preserveLiteral = preserveAmbiguousGlyphs && HasSuffixVariants(ideograph);
            if(_iterStack.find(ideograph) == _iterStack.end()) {
                _iterStack.insert(ideograph);
                for(IDS* expansion: FindIDS(ideograph)) {
                    if(!IsPattern(expansion) || HVArrangementType(AsPattern(expansion)) != splitType) continue;
                    IDSOwnerList expandedAlternatives =
                        HVExtractAlternativesInternal(expansion, false, preserveAmbiguousGlyphs, maximum, truncated);
                    for(const auto& expanded: expandedAlternatives) {
                        if(!IsPattern(expanded.get()) || HVArrangementType(AsPattern(expanded.get())) != splitType)
                            continue;
                        HVSegment segment;
                        segment.fromExpansion = true;
                        AppendClonedOwners(segment.children, AsPattern(expanded.get())->GetpIDSRef());
                        segment.originRanges = AsPattern(expanded.get())->GetHVOriginRanges();
                        appendSegment(choices, std::move(segment));
                    }
                }
                _iterStack.erase(ideograph);
            }

            // 带字形后缀的部件既要保留字面匹配，也要生成其 HV 展开。
            // 否则查询中的“合”会保留为一个节点，而缓存中的“合”已展开为“人一口”，
            // 使 ⿱⬚合 等基础结构查询无法命中。
            if(preserveLiteral) appendSingleton(choices, child);
            if(choices.empty()) appendSingleton(choices, child);
        } else if(IsPattern(child)) {
            IDSOwnerList alternatives =
                HVExtractAlternativesInternal(child, false, preserveAmbiguousGlyphs, maximum, truncated);
            for(const auto& alternative: alternatives) {
                HVSegment segment;
                if(IsPattern(alternative.get()) && HVArrangementType(AsPattern(alternative.get())) == splitType) {
                    segment.originRanges = AsPattern(alternative.get())->GetHVOriginRanges();
                    AppendClonedOwners(segment.children, AsPattern(alternative.get())->GetpIDSRef());
                } else
                    segment.children.push_back(alternative->Clone());
                appendSegment(choices, std::move(segment));
            }
            if(choices.empty()) appendSingleton(choices, child);
        } else
            appendSingleton(choices, child);

        choicesByChild.push_back(std::move(choices));
    }

    std::vector<HVState> states(1);
    for(size_t childIndex = 0; childIndex < choicesByChild.size(); childIndex++) {
        std::vector<HVState> next;
        for(const HVState& state: states) {
            for(const HVSegment& segment: choicesByChild[childIndex]) {
                if(next.size() >= maximum) {
                    truncated = true;
                    break;
                }
                HVState combined;
                combined.originRanges = state.originRanges;
                AppendClonedOwners(combined.children, state.children);
                const size_t segmentOffset = combined.children.size();
                AppendClonedOwners(combined.children, segment.children);
                for(const HVOriginRange& origin: segment.originRanges) {
                    HVOriginRange rebased = origin;
                    rebased.first += segmentOffset;
                    rebased.last += segmentOffset;
                    combined.originRanges.push_back(std::move(rebased));
                }
                if(childIndex < sourceChildren.size() && IsIdeograph(sourceChildren[childIndex].get()) &&
                    isAmbiguousOrigin(*AsIdeograph(sourceChildren[childIndex].get())) &&
                    !segment.children.empty()) {
                    // 即使 HV 没有展开该部件，也要记录它的唯一化来源；否则
                    // 后续递归会把 {曰} 当成普通的“日”同式部件。
                    HVOriginRange origin{AsIdeograph(sourceChildren[childIndex].get())->toString(), segmentOffset,
                        segmentOffset + segment.children.size()};
                    combined.originRanges.push_back(std::move(origin));
                }
                combined.firstSegmentLength = childIndex == 0 ? segment.children.size() : state.firstSegmentLength;
                next.push_back(std::move(combined));
            }
            if(next.size() >= maximum) break;
        }
        states = std::move(next);
        if(states.empty()) return out;
    }

    auto annotateAmbiguousOrigins = [&](const IDSOwnerList& children, std::vector<HVOriginRange>& ranges) {
        auto appendRange = [&](const Ideograph& glyph, size_t first, size_t last) {
            for(const HVOriginRange& existing: ranges)
                if(existing.first == first && existing.last == last) return;
            ranges.push_back(HVOriginRange{glyph.toString(), first, last});
        };
        for(const auto& hashEntry: _sameIDSHashIndex)
            for(const SameIDSHashGroup& group: hashEntry.second) {
                if(group.notEquivalentGlyphs.empty()) continue;
                // The ordinary glyphs in a {glyph} group remain possible HV origins.
                for(const Ideograph& originGlyph: group.glyphs) {
                    if(group.notEquivalentGlyphs.find(originGlyph) != group.notEquivalentGlyphs.end()) continue;
                    const auto rawFound = _rawIDSDB.find(originGlyph);
                    if(rawFound == _rawIDSDB.end()) continue;
                    for(const IDSOwner& raw: rawFound->second) {
                        if(!IsPattern(raw.get()) || raw->toString() != group.expression) continue;
                        const auto& originChildren = AsPattern(raw.get())->GetpIDSRef();
                        if(originChildren.empty() || originChildren.size() > children.size()) continue;
                        for(size_t first = 0; first + originChildren.size() <= children.size(); first++) {
                            bool equal = true;
                            for(size_t offset = 0; offset < originChildren.size(); offset++)
                                if(!IDSequal(originChildren[offset].get(), children[first + offset].get())) {
                                    equal = false;
                                    break;
                                }
                            if(equal) appendRange(originGlyph, first, first + originChildren.size());
                        }
                    }
                }
            }
    };
    int overlayRange[2] = {0, 0};
    pattern->GetOverlayRange(overlayRange);
    for(const HVState& state: states) {
        std::vector<IDS*> children = BorrowIDSList(state.children);
        std::vector<HVOriginRange> originRanges = state.originRanges;
        if(firstLayer) annotateAmbiguousOrigins(state.children, originRanges);
        IDSOwner          alternative;
        if(splitType != IDC_UNKNOWN) {
            const bool   isPrimarySplit = pattern->GetIDC() == IDC_LEFT_RIGHT || pattern->GetIDC() == IDC_ABOVE_BELOW;
            const size_t preferSplit =
                firstLayer && isPrimarySplit && state.firstSegmentLength != 0 ? state.firstSegmentLength - 1 : SIZE_MAX;
            alternative.reset(new Pattern(splitType, children, preferSplit, nullptr, {}, pattern->GetOptionalInt(),
                originRanges));
        } else
            alternative.reset(new Pattern(pattern->GetIDC(), children, SIZE_MAX, overlayRange,
                pattern->GetOverlayType(), pattern->GetOptionalInt(), pattern->GetHVOriginRanges()));
        AppendUniqueOwned(out, std::move(alternative), maximum, truncated);
    }
    return out;
}

IDSOwnerList IDSdatabase::HVExtractOwned(
    IDS* ids, bool firstLayer, bool preserveAmbiguousGlyphs, size_t maximum, bool* truncated) {
    bool localTruncated = false;
    _iterStack.clear();
    IDSOwnerList out = HVExtractAlternativesInternal(ids, firstLayer, preserveAmbiguousGlyphs, maximum, localTruncated);
    _iterStack.clear();
    if(truncated != nullptr) *truncated = localTruncated;
    return out;
}
uint32_t IDSdatabase::CJKsymFallback(uint32_t cp) const {
    if(!((cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x31C0 && cp <= 0x31EE) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x2F800 && cp <= 0x2FAFF)))
        return 0;
    return CJKsymFallbackTable.find(cp) == CJKsymFallbackTable.end() ? 0 : CJKsymFallbackTable.at(cp);
}

bool IDSdatabase::IsSingleStrokeIdeograph(const Ideograph& ideograph) {
    if(IsCJKSingleStroke(ideograph.GetIdeo())) return true;

    auto hasOneStroke = [this](const Ideograph& value) {
        const auto cached = _strokeCountCache.find(value);
        if(cached != _strokeCountCache.end())
            return cached->second.size() == 1 && cached->second.find(1) != cached->second.end();

        if(_rawIDSDB.find(value) == _rawIDSDB.end()) return false;

        Ideograph            copy   = value;
        const StrokeCountSet counts = GetStrokeCounts(&copy);
        return counts.size() == 1 && counts.find(1) != counts.end();
    };

    // 带后缀的字形通常没有独立 IDS，继承无后缀基字形的笔画数。
    return hasOneStroke(ideograph) || (!ideograph.GetSuffix().empty() && hasOneStroke(ideograph.GetPured()));
}
bool IDSdatabase::MatchSingleStrokeToken(const Stroke_Data& token, const Ideograph& query) {
    const std::string queryText = query.toString();
    // 负号只属于笔画序列标记；匹配单笔画时使用无符号 token。
    if(token.stroke == queryText) return true;
    IDSOwner tokenIDS = ParseIDSOwned(token.stroke);
    return tokenIDS != nullptr && IsIdeograph(tokenIDS.get()) &&
        IdeoEqual(*AsIdeograph(tokenIDS.get()), query, true);
}

bool IDSdatabase::MatchSingleStrokeSearchTerm(Stroke* stroke, const Ideograph& query) {
    if(stroke == nullptr) return false;
    for(const Stroke_Data& token: stroke->GetStroke())
        if(MatchSingleStrokeToken(token, query)) return true;
    return false;
}

bool IDSdatabase::MatchIWDSUnificationPattern(IDS* candidate, IDS* pattern, bool querySide) {
    if(candidate == nullptr || pattern == nullptr) return false;
    // 只有包含可展开复合字形的固定查询才屏蔽纯变量模板；简单结构仍需支持 IWDS 变量代入。
    std::function<bool(IDS*)> containsExpandableIdeograph = [&](IDS* value) {
        if(value == nullptr) return false;
        if(IsIdeograph(value)) return _rawIDSDB.find(*AsIdeograph(value)) != _rawIDSDB.end();
        if(!IsPattern(value)) return false;
        for(IDS* child: AsPattern(value)->GetpIDS())
            if(containsExpandableIdeograph(child)) return true;
        return false;
    };
    if(querySide && IsPattern(pattern) && !ContainsIWDSFixedAtom(pattern) && ContainsIWDSFixedAtom(candidate) &&
        containsExpandableIdeograph(candidate))
        return false;

    // 查询侧的字形部件也要尝试原始 IDS；例如“睢”需要展开为“⿰目隹”。
    if(querySide && IsIdeograph(candidate) && IsPattern(pattern)) {
        const Ideograph queryIdeograph = *AsIdeograph(candidate);
        if(_iterStack.find(queryIdeograph) != _iterStack.end()) return false;

        _iterStack.insert(queryIdeograph);
        std::vector<IDS*> expansions     = FindIDS(queryIdeograph);
        const auto        rawDefinitions = _rawIDSDB.find(queryIdeograph);
        if(rawDefinitions != _rawIDSDB.end()) {
            for(const auto& raw: rawDefinitions->second) {
                bool duplicate = false;
                for(IDS* expansion: expansions)
                    if(IDSequal(expansion, raw.get())) {
                        duplicate = true;
                        break;
                    }
                if(!duplicate) expansions.push_back(raw.get());
            }
        }
        for(IDS* expansion: expansions) {
            VariableBindingScope expansionBindings(_variableBindings);
            if(MatchIWDSUnificationPattern(expansion, pattern, querySide)) {
                expansionBindings.Finish(true);
                _iterStack.erase(queryIdeograph);
                return true;
            }
        }
        _iterStack.erase(queryIdeograph);
        return false;
    }

    if(IsVariable(pattern)) {
        if(querySide && ContainsIWDSQueryFlexible(candidate) && IsPattern(candidate) &&
            ContainsIWDSFixedAtom(candidate))
            return false;
        return MatchVariable(candidate, AsVariable(pattern));
    }
    if(IsWildcard(pattern)) return true;
    if(IsIdeograph(pattern))
        return IsIdeograph(candidate) && IdeoEqual(*AsIdeograph(candidate), *AsIdeograph(pattern), true);
    if(IsStroke(pattern)) return IsStroke(candidate) && (*AsStroke(candidate) == *AsStroke(pattern));
    if(IsSearchParam(pattern) || IsSearchExpression(pattern)) return IDSequal(candidate, pattern);
    if(!IsPattern(candidate) || !IsPattern(pattern)) return false;

    Pattern* candidatePattern = AsPattern(candidate);
    Pattern* patternTemplate  = AsPattern(pattern);
    if(candidatePattern->GetIDC() != patternTemplate->GetIDC() ||
        PatternChildCount(candidatePattern) != PatternChildCount(patternTemplate) ||
        candidatePattern->GetOptionalInt() != patternTemplate->GetOptionalInt())
        return false;

    if(IsArrangeIDC(patternTemplate->GetIDC()) &&
        candidatePattern->GetPreferSplitPoint() != patternTemplate->GetPreferSplitPoint())
        return false;

    int candidateRange[2] = {0, 0};
    int patternRange[2]   = {0, 0};
    candidatePattern->GetOverlayRange(candidateRange);
    patternTemplate->GetOverlayRange(patternRange);
    if(candidateRange[0] != patternRange[0] || candidateRange[1] != patternRange[1] ||
        candidatePattern->GetOverlayType() != patternTemplate->GetOverlayType())
        return false;

    for(size_t index = 0; index < PatternChildCount(patternTemplate); index++)
        if(!MatchIWDSUnificationPattern(
               PatternChild(candidatePattern, index), PatternChild(patternTemplate, index), querySide))
            return false;
    return true;
}

bool IDSdatabase::IdeoEqual(Ideograph ideo1, Ideograph ideo2, bool cpOnly, bool strictSuffix) {
    // CJK 笔画字符会通过 CJKsymFallback 回退到普通字形。
    // 这条回退路径不能再忽略后缀，否则“㇐”会把“一t”等地域/字形
    // 后缀也一并匹配进来。
    const bool strictStrokeSuffix =
        strictSuffix && cpOnly && (IsSingleStrokeIdeograph(ideo1) || IsSingleStrokeIdeograph(ideo2));

    if(!cpOnly && !(ideo1.GetSuffix() == ideo2.GetSuffix()))
        return false;
    else if(ideo1.inUnicode() ^ ideo2.inUnicode())
        return false;
    else if(!ideo1.inUnicode() && !ideo2.inUnicode())
        return ideo1.GetAbstractName() == ideo2.GetAbstractName();
    // 2 Unicode ideographs
    if(config.misc.symFallback) {
        if(CJKsymFallback(ideo1.GetIdeo()) && _idsDB.find(ideo1) == _idsDB.end())
            ideo1 = Ideograph(CJKsymFallback(ideo1.GetIdeo()), ideo1.GetVS(), ideo1.GetSuffix());
        if(CJKsymFallback(ideo2.GetIdeo()) && _idsDB.find(ideo2) == _idsDB.end())
            ideo2 = Ideograph(CJKsymFallback(ideo2.GetIdeo()), ideo2.GetVS(), ideo2.GetSuffix());
    }
    if(ideo1.GetIdeo() == ideo2.GetIdeo() && (cpOnly ? 1 : ideo1.GetVS() == ideo2.GetVS()) &&
        (!strictStrokeSuffix || ideo1.GetSuffix() == ideo2.GetSuffix()))
        return true;
    // SourceCodeSeparation、lv1、lv2 都在查询预处理阶段展开；
    // 匹配循环只比较已经展开的查询，避免同一规则重复执行。
    return false;
}

bool IDSdatabase::ContainsOverlay(IDS* ids) const {
    if(!IsPattern(ids)) return false;
    Pattern* pattern = AsPattern(ids);
    if(pattern->GetIDC() == IDC_OVERLAY) return true;
    for(IDS* child: BorrowPatternIDS(pattern))
        if(ContainsOverlay(child)) return true;
    return false;
}

const char* IDSglyphDomainName(IDSglyphDomain domain) {
    switch(domain) {
    case IDS_GLYPH_DOMAIN_ALL:      return "all";
    case IDS_GLYPH_DOMAIN_UNICODE:  return "unicode";
    case IDS_GLYPH_DOMAIN_PRIVATE:  return "private";
    case IDS_GLYPH_DOMAIN_ABSTRACT: return "abstract";
    default:                        return "all";
    }
}

bool ParseIDSglyphDomain(const std::string& value, IDSglyphDomain& domain) {
    if(value == "all")
        domain = IDS_GLYPH_DOMAIN_ALL;
    else if(value == "unicode")
        domain = IDS_GLYPH_DOMAIN_UNICODE;
    else if(value == "private")
        domain = IDS_GLYPH_DOMAIN_PRIVATE;
    else if(value == "abstract")
        domain = IDS_GLYPH_DOMAIN_ABSTRACT;
    else
        return false;
    return true;
}

const char* IDSunicodeBlockName(IDSunicodeBlock block) {
    switch(block) {
    case IDS_UNICODE_BLOCK_ALL:               return "all";
    case IDS_UNICODE_BLOCK_CJK:               return "cjk";
    case IDS_UNICODE_BLOCK_CJK_BASIC:         return "basic";
    case IDS_UNICODE_BLOCK_CJK_EXT_A:         return "ext-a";
    case IDS_UNICODE_BLOCK_CJK_EXT_B:         return "ext-b";
    case IDS_UNICODE_BLOCK_CJK_EXT_C:         return "ext-c";
    case IDS_UNICODE_BLOCK_CJK_EXT_D:         return "ext-d";
    case IDS_UNICODE_BLOCK_CJK_EXT_E:         return "ext-e";
    case IDS_UNICODE_BLOCK_CJK_EXT_F:         return "ext-f";
    case IDS_UNICODE_BLOCK_CJK_EXT_G:         return "ext-g";
    case IDS_UNICODE_BLOCK_CJK_EXT_H:         return "ext-h";
    case IDS_UNICODE_BLOCK_CJK_EXT_I:         return "ext-i";
    case IDS_UNICODE_BLOCK_CJK_EXT_J:         return "ext-j";
    case IDS_UNICODE_BLOCK_CJK_COMPATIBILITY: return "compatibility";
    case IDS_UNICODE_BLOCK_CJK_RADICALS:      return "radicals";
    case IDS_UNICODE_BLOCK_CJK_STROKES:       return "strokes";
    case IDS_UNICODE_BLOCK_PRIVATE_BMP:       return "private-bmp";
    case IDS_UNICODE_BLOCK_PRIVATE_PLANE15:   return "private-plane15";
    case IDS_UNICODE_BLOCK_PRIVATE_PLANE16:   return "private-plane16";
    case IDS_UNICODE_BLOCK_ABSTRACT:          return "abstract";
    case IDS_UNICODE_BLOCK_OTHER:             return "other";
    default:                                  return "all";
    }
}

bool ParseIDSunicodeBlock(const std::string& value, IDSunicodeBlock& block) {
    for(int raw = IDS_UNICODE_BLOCK_ALL; raw <= IDS_UNICODE_BLOCK_OTHER; raw++) {
        const IDSunicodeBlock candidate = static_cast<IDSunicodeBlock>(raw);
        if(value == IDSunicodeBlockName(candidate)) {
            block = candidate;
            return true;
        }
    }
    return false;
}
bool ParseIDSunicodeBlocks(const std::string& value, std::vector<IDSunicodeBlock>& blocks) {
    blocks.clear();
    std::stringstream stream(value);
    std::string       token;
    while(std::getline(stream, token, ',')) {
        const size_t first = token.find_first_not_of(" 	");
        if(first == std::string::npos) return false;
        const size_t last     = token.find_last_not_of(" 	");
        token                 = token.substr(first, last - first + 1);
        IDSunicodeBlock block = IDS_UNICODE_BLOCK_ALL;
        if(!ParseIDSunicodeBlock(token, block)) return false;
        if(std::find(blocks.begin(), blocks.end(), block) == blocks.end()) blocks.push_back(block);
    }
    return !blocks.empty();
}

static bool IsUnicodeRange(uint32_t codepoint, uint32_t minimum, uint32_t maximum) {
    return codepoint >= minimum && codepoint <= maximum;
}

static bool IsCodepointInUnicodeBlock(uint32_t codepoint, IDSunicodeBlock block) {
    switch(block) {
    case IDS_UNICODE_BLOCK_ALL: return true;
    case IDS_UNICODE_BLOCK_CJK:
        return IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_BASIC) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_A) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_B) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_C) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_D) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_E) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_F) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_G) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_H) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_I) ||
            IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_EXT_J);
    case IDS_UNICODE_BLOCK_CJK_BASIC: return IsUnicodeRange(codepoint, 0x4E00, 0x9FFF);
    case IDS_UNICODE_BLOCK_CJK_EXT_A: return IsUnicodeRange(codepoint, 0x3400, 0x4DBF);
    case IDS_UNICODE_BLOCK_CJK_EXT_B: return IsUnicodeRange(codepoint, 0x20000, 0x2A6DF);
    case IDS_UNICODE_BLOCK_CJK_EXT_C: return IsUnicodeRange(codepoint, 0x2A700, 0x2B73F);
    case IDS_UNICODE_BLOCK_CJK_EXT_D: return IsUnicodeRange(codepoint, 0x2B740, 0x2B81F);
    case IDS_UNICODE_BLOCK_CJK_EXT_E: return IsUnicodeRange(codepoint, 0x2B820, 0x2CEAF);
    case IDS_UNICODE_BLOCK_CJK_EXT_F: return IsUnicodeRange(codepoint, 0x2CEB0, 0x2EBEF);
    case IDS_UNICODE_BLOCK_CJK_EXT_G: return IsUnicodeRange(codepoint, 0x30000, 0x3134F);
    case IDS_UNICODE_BLOCK_CJK_EXT_H: return IsUnicodeRange(codepoint, 0x31350, 0x323AF);
    case IDS_UNICODE_BLOCK_CJK_EXT_I: return IsUnicodeRange(codepoint, 0x2EBF0, 0x2EE5F);
    case IDS_UNICODE_BLOCK_CJK_EXT_J: return IsUnicodeRange(codepoint, 0x323B0, 0x3347F);
    case IDS_UNICODE_BLOCK_CJK_COMPATIBILITY:
        return IsUnicodeRange(codepoint, 0xF900, 0xFAFF) || IsUnicodeRange(codepoint, 0x2F800, 0x2FA1F);
    case IDS_UNICODE_BLOCK_CJK_RADICALS:
        return IsUnicodeRange(codepoint, 0x2E80, 0x2EFF) || IsUnicodeRange(codepoint, 0x2F00, 0x2FDF);
    case IDS_UNICODE_BLOCK_CJK_STROKES:     return IsUnicodeRange(codepoint, 0x31C0, 0x31EF);
    case IDS_UNICODE_BLOCK_PRIVATE_BMP:     return IsUnicodeRange(codepoint, 0xE000, 0xF8FF);
    case IDS_UNICODE_BLOCK_PRIVATE_PLANE15: return IsUnicodeRange(codepoint, 0xF0000, 0xFFFFD);
    case IDS_UNICODE_BLOCK_PRIVATE_PLANE16: return IsUnicodeRange(codepoint, 0x100000, 0x10FFFD);
    case IDS_UNICODE_BLOCK_ABSTRACT:        return false;
    case IDS_UNICODE_BLOCK_OTHER:
        return codepoint != 0 && !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_COMPATIBILITY) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_RADICALS) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_CJK_STROKES) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_PRIVATE_BMP) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_PRIVATE_PLANE15) &&
            !IsCodepointInUnicodeBlock(codepoint, IDS_UNICODE_BLOCK_PRIVATE_PLANE16);
    default: return false;
    }
}

static bool MatchesUnicodeBlock(const Ideograph& ideograph, IDSunicodeBlock block) {
    if(block == IDS_UNICODE_BLOCK_ABSTRACT) return !ideograph.inUnicode();
    if(!ideograph.inUnicode()) return false;
    return IsCodepointInUnicodeBlock(ideograph.GetIdeo(), block);
}
static bool KeepIVSAsIndependentResult(IDSresultFilter filter) {
    return filter == IDS_RESULT_IGNORE_OTHER_LOCALES_KEEP_IVS;
}

static Ideograph LocaleIdentity(const Ideograph& ideograph, bool keepIVS) {
    if(!ideograph.inUnicode()) return Ideograph(ideograph.GetAbstractName());
    return Ideograph(ideograph.GetIdeo(), keepIVS ? ideograph.GetVS() : 0);
}

bool IDSdatabase::ShouldIncludeResult(Ideograph ideograph, const IDSqueryOptions& options) const {
    switch(options.filter.glyphDomain) {
    case IDS_GLYPH_DOMAIN_UNICODE:
        if(!ideograph.inUnicode() || ideograph.inPUA()) return false;
        break;
    case IDS_GLYPH_DOMAIN_PRIVATE:
        if(!ideograph.inPUA()) return false;
        break;
    case IDS_GLYPH_DOMAIN_ABSTRACT:
        if(ideograph.inUnicode()) return false;
        break;
    default: break;
    }
    const IDSFilterOptions& filter = options.filter;
    if(!filter.unicodeBlocks.empty() || !filter.customRanges.empty()) {
        bool matchesRange = false;
        for(const IDSunicodeBlock block: filter.unicodeBlocks) {
            if(block == IDS_UNICODE_BLOCK_ALL || MatchesUnicodeBlock(ideograph, block)) {
                matchesRange = true;
                break;
            }
        }
        if(!matchesRange) {
            for(const std::string& rangeName: filter.customRanges) {
                const auto range = _customGlyphRanges.find(rangeName);
                if(range != _customGlyphRanges.end() && MatchesCustomGlyphRange(ideograph, range->second)) {
                    matchesRange = true;
                    break;
                }
            }
        }
        if(!matchesRange) return false;
    }
    if(options.filter.resultFilter == IDS_RESULT_ALL) return true;

    const std::string suffix = ideograph.GetSuffix();
    if(suffix == "r" && config.misc.suffixRisAltForm) return true;
    // Locale variants are filtered after the complete matched-result set is
    // available, so a non-matching base glyph cannot make a locale variant survive.
    if(suffix.find_first_of(ASCII_LOWERCASE) != std::string::npos) return false;
    return true;
}

void IDSdatabase::ApplyResultFilter(std::vector<Ideograph>& result, const IDSqueryOptions& options) const {
    const IDSresultFilter filter = options.filter.resultFilter;
    if(filter != IDS_RESULT_IGNORE_OTHER_LOCALES_BASE_ONLY &&
        filter != IDS_RESULT_IGNORE_OTHER_LOCALES_KEEP_IVS) {
        std::sort(result.begin(), result.end(), IdeographCmp);
        return;
    }

    const bool keepIVS = KeepIVSAsIndependentResult(filter);
    // alternative 阶段使用 ALL 时，小写后缀也会进入合并结果；严格模式仍应排除它们。
    result.erase(std::remove_if(result.begin(), result.end(),
                     [this](const Ideograph& ideograph) {
                         const std::string suffix = ideograph.GetSuffix();
                         return !(suffix == "r" && config.misc.suffixRisAltForm) &&
                             suffix.find_first_of(ASCII_LOWERCASE) != std::string::npos;
                     }),
        result.end());
    // 严格 locale 筛选只接受本次查询实际命中的无后缀基字形。
    // 这样即使设置了回退顺序，也不会从另一个 IDS 表达式借来 locale 变体。
    std::unordered_set<Ideograph, Ideograph_Hash> matchedBases;
    for(const Ideograph& ideograph: result)
        if(ideograph.GetSuffix().empty())
            matchedBases.insert(LocaleIdentity(ideograph, keepIVS));

    const auto hasMatchedBase = [&matchedBases, keepIVS](const Ideograph& ideograph) {
        const Ideograph identity = LocaleIdentity(ideograph, keepIVS);
        if(matchedBases.find(identity) != matchedBases.end()) return true;
        // keep-ivs 下，带 VS 的字形本身就是独立身份，即使没有无后缀记录也可保留。
        return keepIVS && ideograph.GetVS() != 0;
    };
    const std::vector<std::string>& localeOrder = options.filter.localeSuffixFallbackOrder;
    if(!localeOrder.empty()) {
        const auto localeGroups = LocaleSuffixFallbackGroups(localeOrder);
        std::sort(result.begin(), result.end(),
            [&localeGroups, keepIVS](const Ideograph& left, const Ideograph& right) {
                if(LocaleIdentity(left, keepIVS) == LocaleIdentity(right, keepIVS)) {
                    const auto rank = [&localeGroups](const Ideograph& ideograph) {
                        const std::string suffix = ideograph.GetSuffix();
                        for(size_t index = 0; index < localeGroups.size(); ++index)
                            if(std::find(localeGroups[index].begin(), localeGroups[index].end(), suffix) !=
                                localeGroups[index].end())
                                return index;
                        return localeGroups.size();
                    };
                    const size_t leftRank = rank(left);
                    const size_t rightRank = rank(right);
                    if(leftRank != rightRank) return leftRank < rightRank;
                }
                return IdeographCmp(left, right);
            });
        std::unordered_set<Ideograph, Ideograph_Hash> seen;
        result.erase(std::remove_if(result.begin(), result.end(),
                         [&seen, &hasMatchedBase, keepIVS](const Ideograph& ideograph) {
                             const Ideograph identity = LocaleIdentity(ideograph, keepIVS);
                             if(!hasMatchedBase(ideograph)) return true;
                             return !seen.insert(identity).second;
                         }),
            result.end());
        return;
    }

    // 未指定回退顺序时，每个身份只保留一个结果。
    // base-only 按基码位折叠 IVS，并因排序优先保留无 VS 的字形；
    // keep-ivs 则按“基码位 + VS”分别保留。
    std::sort(result.begin(), result.end(), IdeographCmp);
    std::unordered_set<Ideograph, Ideograph_Hash> seen;
    result.erase(std::remove_if(result.begin(), result.end(),
                     [&matchedBases, &seen, &hasMatchedBase, keepIVS](const Ideograph& ideograph) {
                         const Ideograph identity = LocaleIdentity(ideograph, keepIVS);
                         if(!hasMatchedBase(ideograph)) return true;
                         if(!keepIVS) return !seen.insert(identity).second;
                         if(matchedBases.find(identity) == matchedBases.end())
                             return !seen.insert(identity).second;
                         return !ideograph.GetSuffix().empty();
                     }),
        result.end());
}

bool IDSdatabase::IDSarrayMatch(
    const std::vector<IDS*>& s, const std::vector<IDS*>& p, IDCtype arrangement, size_t sIdx, size_t pIdx) {
    VariableBindingScope bindings(_variableBindings);
    if(sIdx == s.size() && pIdx == p.size()) return bindings.Finish(true);
    if(sIdx == s.size() || pIdx == p.size()) return false;

    IDS* currentPatternIDS = p[pIdx];
    if(IsWildcard(currentPatternIDS)) {
        for(size_t i = 1; i < s.size() - sIdx + 1; i++)
            if(IDSarrayMatch(s, p, arrangement, sIdx + i, pIdx + 1)) return bindings.Finish(true);
        return false;
    }

    if(IsVariable(currentPatternIDS)) {
        const size_t minimumRemaining  = p.size() - pIdx - 1;
        const size_t candidateRemaining = s.size() - sIdx;
        // Avoid unsigned underflow before calculating the width available to this variable.
        if(candidateRemaining <= minimumRemaining) return false;
        const size_t maximumWidth = candidateRemaining - minimumRemaining;

        IDSVariable* variable = AsVariable(currentPatternIDS);
        const auto existing          = _variableBindings.find(variable->GetName());
        const size_t widthLimit = existing == _variableBindings.end() ? maximumWidth : 1;
        for(size_t width = 1; width <= widthLimit; width++) {
            _variableBindings = bindings.GetSaved();
            const std::vector<IDS*> range(s.begin() + sIdx, s.begin() + sIdx + width);
            if(MatchVariableRange(range, variable, arrangement) &&
                IDSarrayMatch(s, p, arrangement, sIdx + width, pIdx + 1))
                return bindings.Finish(true);
        }
        return false;
    }

    if(!IDSmatch(s[sIdx], currentPatternIDS)) return false;
    return bindings.Finish(IDSarrayMatch(s, p, arrangement, sIdx + 1, pIdx + 1));
}
typedef struct {
    IDCtype arrange;
    IDCtype surround;
    bool    surroundedOnFirst;
} SurroundEqualSyntax;

const SurroundEqualSyntax SurroundEqualTable[] = {
    { IDC_VERTICAL_ARRANGE,      IDC_SURROUND_ABOVE, false},
    { IDC_VERTICAL_ARRANGE,      IDC_SURROUND_BELOW,  true},
    {IDC_HORIZONAL_ARRANGE,       IDC_SURROUND_LEFT, false},
    {IDC_HORIZONAL_ARRANGE,      IDC_SURROUND_RIGHT,  true},
    {IDC_HORIZONAL_ARRANGE,  IDC_SURROUND_UPPERLEFT, false},
    { IDC_VERTICAL_ARRANGE,  IDC_SURROUND_UPPERLEFT, false},
    {IDC_HORIZONAL_ARRANGE, IDC_SURROUND_UPPERRIGHT,  true},
    { IDC_VERTICAL_ARRANGE, IDC_SURROUND_UPPERRIGHT, false},
    {IDC_HORIZONAL_ARRANGE,  IDC_SURROUND_LOWERLEFT, false},
    { IDC_VERTICAL_ARRANGE,  IDC_SURROUND_LOWERLEFT,  true},
    {IDC_HORIZONAL_ARRANGE, IDC_SURROUND_LOWERRIGHT,  true},
    { IDC_VERTICAL_ARRANGE, IDC_SURROUND_LOWERRIGHT,  true}
};

bool IDSdatabase::IDSmatch(IDS* idsInDB, IDS* ids, bool surroundEqual) {
    if(idsInDB == nullptr || ids == nullptr) return false;
    VariableBindingScope bindings(_variableBindings);
    return bindings.Finish(MatchIDSUnscoped(idsInDB, ids, surroundEqual));
}

bool IDSdatabase::MatchIDSUnscoped(IDS* idsInDB, IDS* ids, bool surroundEqual) {
    if(idsInDB == nullptr || ids == nullptr) return false;
    if(!IsSearchExpression(ids) && HasDisjointStrokeRange(idsInDB, ids)) return false;

    bool matched = false;
    if(IsVariable(ids))
        matched = MatchVariable(idsInDB, AsVariable(ids));
    else if(IsSearchExpression(ids))
        matched = MatchSearchExpression(idsInDB, AsSearchExpression(ids));
    else if(IsStrokeCountQuery(ids))
        matched = MatchStrokeCount(idsInDB, AsSearchParam(ids));
    else if(IsIdeograph(idsInDB))
        matched = MatchIdeograph(idsInDB, ids);
    else if(IsStroke(idsInDB))
        matched = MatchStroke(idsInDB, ids);
    else if(IsPattern(idsInDB))
        matched = MatchPattern(idsInDB, ids, surroundEqual);
    return matched;
}

bool IDSdatabase::MatchVariable(IDS* idsInDB, IDSVariable* variable) {
    if(idsInDB == nullptr || variable == nullptr) return false;

    const std::string& name     = variable->GetName();
    const auto         existing = _variableBindings.find(name);
    if(existing == _variableBindings.end()) {
        VariableBinding binding;
        binding.nodes.push_back(idsInDB->toString());
        _variableBindings.emplace(name, std::move(binding));
        return true;
    }

    return VariableBindingEquivalent(existing->second, idsInDB);
}

bool IDSdatabase::MatchVariableRange(
    const std::vector<IDS*>& candidates, IDSVariable* variable, IDCtype arrangement) {
    if(candidates.empty() || variable == nullptr || !IsArrangeIDC(arrangement)) return false;

    const auto existing = _variableBindings.find(variable->GetName());
    if(existing != _variableBindings.end())
        return VariableBindingEquivalent(existing->second, candidates, arrangement);

    VariableBinding binding;
    binding.arrangement = candidates.size() > 1 ? arrangement : IDC_UNKNOWN;
    binding.nodes.reserve(candidates.size());
    for(IDS* candidate: candidates) {
        if(candidate == nullptr) return false;
        binding.nodes.push_back(candidate->toString());
    }
    _variableBindings.emplace(variable->GetName(), std::move(binding));
    return true;
}

bool IDSdatabase::VariableBindingEquivalent(const VariableBinding& binding, IDS* candidate) {
    if(candidate == nullptr || binding.nodes.empty()) return false;
    return VariableBindingEquivalent(binding, std::vector<IDS*>({candidate}), IDC_UNKNOWN);
}

bool IDSdatabase::VariableBindingEquivalent(
    const VariableBinding& binding, const std::vector<IDS*>& candidates, IDCtype arrangement) {
    if(binding.nodes.empty() || candidates.empty()) return false;

    auto matchesNodes = [&](const std::vector<IDS*>& values) {
        if(values.size() != binding.nodes.size()) return false;
        for(size_t index = 0; index < values.size(); index++) {
            IDSOwner bound = ParseIDSOwned(binding.nodes[index]);
            if(bound == nullptr || !VariableEquivalent(bound.get(), values[index])) return false;
        }
        return true;
    };

    if(binding.arrangement == IDC_UNKNOWN)
        return candidates.size() == 1 && matchesNodes(candidates);
    if(arrangement == binding.arrangement && matchesNodes(candidates)) return true;
    if(candidates.size() != 1 || candidates.front() == nullptr) return false;

    IDS* candidate = candidates.front();
    auto matchesExpansion = [&](IDS* expansion) {
        return IsPattern(expansion) && HVArrangementType(AsPattern(expansion)) == binding.arrangement &&
            matchesNodes(BorrowPatternIDS(AsPattern(expansion)));
    };

    if(matchesExpansion(candidate)) return true;
    if(IsIdeograph(candidate)) {
        for(IDS* expansion: FindIDS(*AsIdeograph(candidate)))
            if(matchesExpansion(expansion)) return true;
    } else if(IsPattern(candidate)) {
        bool truncated = false;
        for(const IDSOwner& expansion: HVExtractAlternativesInternal(candidate, true, false, 64, truncated))
            if(matchesExpansion(expansion.get())) return true;
    }
    return false;
}

bool IDSdatabase::VariableEquivalent(IDS* bound, IDS* candidate) {
    if(bound == nullptr || candidate == nullptr || bound->GetType() != candidate->GetType()) return false;

    if(IsIdeograph(bound))
        // Glyph suffixes and variation selectors describe glyph forms, not the
        // component identity represented by an IWDS-style variable.
        return IdeoEqual(*AsIdeograph(bound), *AsIdeograph(candidate), true, false);
    if(IsStroke(bound)) return *AsStroke(bound) == *AsStroke(candidate);
    if(IsSearchParam(bound) || IsSearchExpression(bound) || IsVariable(bound)) return IDSequal(bound, candidate);
    if(!IsPattern(bound)) return IDSequal(bound, candidate);

    Pattern* boundPattern     = AsPattern(bound);
    Pattern* candidatePattern = AsPattern(candidate);
    if(boundPattern->GetIDC() != candidatePattern->GetIDC() ||
        PatternChildCount(boundPattern) != PatternChildCount(candidatePattern) ||
        boundPattern->GetOptionalInt() != candidatePattern->GetOptionalInt())
        return false;

    if(IsArrangeIDC(boundPattern->GetIDC()) &&
        boundPattern->GetPreferSplitPoint() != candidatePattern->GetPreferSplitPoint())
        return false;

    int boundRange[2]     = {0, 0};
    int candidateRange[2] = {0, 0};
    boundPattern->GetOverlayRange(boundRange);
    candidatePattern->GetOverlayRange(candidateRange);
    if(boundRange[0] != candidateRange[0] || boundRange[1] != candidateRange[1] ||
        boundPattern->GetOverlayType() != candidatePattern->GetOverlayType())
        return false;

    for(size_t index = 0; index < PatternChildCount(boundPattern); index++)
        if(!VariableEquivalent(PatternChild(boundPattern, index), PatternChild(candidatePattern, index))) return false;
    return true;
}

bool IDSdatabase::MatchesSameIDS(IDS* idsInDB, Ideograph query, bool ignoreSuffix) {
    if(idsInDB == nullptr) return false;
    if(IsIdeograph(idsInDB) && IdeoEqual(*AsIdeograph(idsInDB), query, true)) return true;

    for(IDS* definition: FindIDS(query, ignoreSuffix))
        if(IDSequal(idsInDB, definition)) return true;
    return false;
}

bool IDSdatabase::MatchSearchExpression(IDS* idsInDB, SearchExpression* search) {
    if(search == nullptr) return false;
    if(search->GetMode() == SEARCH_EXPRESSION_EXCEPT) return !MatchExceptTerms(idsInDB, search->GetTerms());
    if(search->GetTerms().size() == 1 && IsIdeograph(search->GetTerms().front().get()) && IsPattern(idsInDB)) {
        const Ideograph query = *AsIdeograph(search->GetTerms().front().get());
        if(HasForeignHVOrigin(AsPattern(idsInDB), query) && !HasMatchingHVOrigin(AsPattern(idsInDB), query))
            return false;
    }

    const bool matched = search->GetMode() == SEARCH_EXPRESSION_ANY
        ? MatchAnyExpression(idsInDB, search)
        : MatchSearchTermsWithAny(idsInDB, search->GetTerms());
    if(!matched) return false;

    const IDSOwnerList& exceptTerms = search->GetExceptTerms();
    return exceptTerms.empty() || !MatchExceptTerms(idsInDB, exceptTerms);
}

bool IDSdatabase::MatchAnyExpression(IDS* idsInDB, SearchExpression* search) {
    if(idsInDB == nullptr || search == nullptr) return false;

    for(const auto& term: search->GetTerms()) {
        if(IsResidueCountQuery(term.get())) return false;
        const IDSOwnerList alternatives = ExpandQueryOnlyTerm(term.get());
        for(const auto& alternative: alternatives) {
            const bool strokeMatched = IsStroke(idsInDB) && IsIdeograph(alternative.get()) &&
                MatchSingleStrokeSearchTerm(AsStroke(idsInDB), *AsIdeograph(alternative.get()));
            const bool matched = strokeMatched || (IsSearchTermMemoSafe(alternative.get())
                ? MatchIDSUnscoped(idsInDB, alternative.get())
                : IDSmatch(idsInDB, alternative.get()));
            if(matched) return true;
        }
    }
    return false;
}

bool IDSdatabase::MatchExceptTerms(IDS* idsInDB, const IDSOwnerList& terms) {
    // Each direct exception term removes a candidate independently. A nested
    // <search=...> remains one term and therefore retains its AND semantics.
    for(const auto& term: terms) {
        IDSOwnerList oneTerm;
        oneTerm.push_back(term->Clone());
        if(MatchSearchTermsWithAny(idsInDB, oneTerm)) return true;
    }
    return false;
}

bool IDSdatabase::MatchSearchTermsWithAny(IDS* idsInDB, const IDSOwnerList& terms) {
    bool hasComplexAny = false;
    for(const auto& term: terms) {
        if(!IsSearchExpression(term.get()) ||
            AsSearchExpression(term.get())->GetMode() != SEARCH_EXPRESSION_ANY)
            continue;
        SearchExpression* any = AsSearchExpression(term.get());
        if(!any->GetExceptTerms().empty()) hasComplexAny = true;
        for(const auto& alternative: any->GetTerms())
            if(IsSearchExpression(alternative.get())) {
                hasComplexAny = true;
                break;
            }
        if(hasComplexAny) break;
    }

    if(!hasComplexAny) {
        // <any> 是一个搜索项，而不是要复制成多次完整查询的宏。
        // IDSsearch 会在当前节点直接调用 IDSmatch；IDSmatch 再由
        // MatchAnyExpression 判断候选是否命中任一分支。这样可以避免
        // <search=<any=A,B>,C> 被拆成两次全库递归搜索。
        return MatchSearchTerms(idsInDB, terms);
    }

    const std::vector<IDSOwnerList> expandedTerms = ExpandQueryOnlyTerms(terms);
    if(expandedTerms.empty()) return false;

    for(const auto& expanded: expandedTerms) {
        for(size_t index = 0; index < expanded.size(); index++) {
            IDS* term = expanded[index].get();
            if(!IsSearchExpression(term) || AsSearchExpression(term)->GetMode() != SEARCH_EXPRESSION_ANY) continue;

            SearchExpression*   anyExpression = AsSearchExpression(term);
            const IDSOwnerList& exceptTerms   = anyExpression->GetExceptTerms();
            for(const auto& alternative: anyExpression->GetTerms()) {
                IDSOwnerList nextTerms;
                nextTerms.reserve(expanded.size());
                for(size_t termIndex = 0; termIndex < expanded.size(); termIndex++)
                    nextTerms.push_back(termIndex == index ? alternative->Clone() : expanded[termIndex]->Clone());


                if(MatchSearchTermsWithAny(idsInDB, nextTerms) &&
                    (exceptTerms.empty() || !MatchExceptTerms(idsInDB, exceptTerms)))
                    return true;
            }
            return false;
        }
        if(MatchSearchTerms(idsInDB, expanded)) return true;
    }
    return false;
}

bool IDSdatabase::CanMatchIWDSShape(IDS* idsInDB, IDS* term) const {
    if(!IsPattern(idsInDB) || !IsPattern(term)) return true;
    if(config.fuzzyMatch.unificationLevel < IWDS_UNIFICATION_LV1 ||
        config.fuzzyMatch.unificationLevel >= IWDS_UNIFICATION_LEVEL_COUNT)
        return false;

    const std::string candidateShape = IWDSIDSShapeKey(idsInDB);
    const std::string queryShape     = IWDSIDSShapeKey(term);
    const std::string queryKey       = term->toString();
    for(size_t level = IWDS_UNIFICATION_LV1;
        level <= static_cast<size_t>(config.fuzzyMatch.unificationLevel) && level < IWDS_UNIFICATION_LEVEL_COUNT;
        level++) {
        const auto queryExact = _unifiableIDSGroupIndex[level].find(queryKey);
        if(queryExact != _unifiableIDSGroupIndex[level].end()) {
            const size_t groupIndex = queryExact->second;
            if(groupIndex >= _unifiableIDSParsedGroups[level].size()) continue;
            for(const auto& member: _unifiableIDSParsedGroups[level][groupIndex])
                if(IWDSIDSShapeKey(member.get()) == candidateShape) return true;
            continue;
        }

        const auto shapeFound = _unifiableIDSShapeIndex[level].find(candidateShape);
        if(shapeFound == _unifiableIDSShapeIndex[level].end()) continue;
        for(const size_t groupIndex: shapeFound->second) {
            if(groupIndex >= _unifiableIDSParsedGroups[level].size()) continue;
            for(const auto& member: _unifiableIDSParsedGroups[level][groupIndex])
                if(IWDSIDSShapeKey(member.get()) == queryShape) return true;
        }
    }
    return false;
}

bool IDSdatabase::HasMatchingHVOrigin(const Pattern* candidate, const Ideograph& query) const {
    if(candidate == nullptr) return false;
    const std::string queryGlyph = query.toString();
    for(const HVOriginRange& origin: candidate->GetHVOriginRanges())
        if(origin.glyph == queryGlyph) return true;
    return false;
}

bool IDSdatabase::HasForeignHVOrigin(const Pattern* candidate, const Ideograph& query) const {
    if(candidate == nullptr) return false;
    if(!config.fuzzyMatch.excludeNonEquivalentSameIDS) return false;
    const std::string queryGlyph = query.toString();
    for(const HVOriginRange& origin: candidate->GetHVOriginRanges())
        if(origin.glyph != queryGlyph) return true;
    return false;
}

bool IDSdatabase::MatchSearchTermCached(IDS* idsInDB, IDS* term) {
    if(idsInDB == nullptr || term == nullptr) return false;

    std::string key = idsInDB->toString();
    key.push_back(static_cast<char>(0x1F));
    key              += term->toString();
    const auto cached = _searchTermMemo.find(key);
    if(cached != _searchTermMemo.end()) return cached->second;

    bool matched = false;
    {
        // Search-term results must not leak IWDS internal bindings to callers.
        VariableBindingScope bindings(_variableBindings);
        _variableBindings.clear();
        bool queryHasUniqueSeparator = false;
        if(IsIdeograph(term)) {
            const auto queryDefinitions = _rawIDSUniqueSeparators.find(*AsIdeograph(term));
            if(queryDefinitions != _rawIDSUniqueSeparators.end())
                for(const std::string& separator: queryDefinitions->second)
                    if(!separator.empty()) {
                        queryHasUniqueSeparator = true;
                        break;
                    }
        }

        bool blockedByForeignHVOrigin = false;
        if(IsIdeograph(term) && IsPattern(idsInDB)) {
            Pattern* candidatePattern = AsPattern(idsInDB);
            const Ideograph query = *AsIdeograph(term);
            if(HasMatchingHVOrigin(candidatePattern, query)) matched = true;
            blockedByForeignHVOrigin = HasForeignHVOrigin(candidatePattern, query);
        }
        if(!matched && !blockedByForeignHVOrigin) {
            if(IDSequal(idsInDB, term))
                matched = true;
            else
                matched = IDSmatch(idsInDB, term);
        }
        if(!matched && !blockedByForeignHVOrigin && IsIdeograph(term) && IsStroke(idsInDB))
            matched = MatchSingleStrokeSearchTerm(AsStroke(idsInDB), *AsIdeograph(term));
        if(!matched && !blockedByForeignHVOrigin && IsIdeograph(term) && IsPattern(idsInDB) &&
            config.fuzzyMatch.excludeNonEquivalentSameIDS &&
            queryHasUniqueSeparator == !idsInDB->GetUniqueSeparator().empty()) {
            Pattern* candidatePattern = AsPattern(idsInDB);
            if(IsArrangeIDC(candidatePattern->GetIDC())) {
                for(IDS* expansion: FindIDS(*AsIdeograph(term))) {
                    if(!IsPattern(expansion) || AsPattern(expansion)->GetIDC() != candidatePattern->GetIDC() ||
                        !IsArrangeIDC(AsPattern(expansion)->GetIDC()))
                        continue;

                    if(IDSsubarray(BorrowPatternIDS(candidatePattern), BorrowPatternIDS(AsPattern(expansion))) !=
                        SIZE_MAX) {
                        matched = true;
                        break;
                    }
                }
            }
        }
        if(!matched && IsIdeograph(idsInDB)) {
            const Ideograph candidate = *AsIdeograph(idsInDB);
            if(_iterStack.find(candidate) == _iterStack.end()) {
                _iterStack.insert(candidate);
                if(_cache_BasicIdeo.find(candidate) == _cache_BasicIdeo.end() &&
                    !(config.fuzzyMatch.excludeNonEquivalentSameIDS && IsIdeograph(term) &&
                        IsNonEquivalentSameIDS(candidate, *AsIdeograph(term)))) {
                    for(IDS* expansion: FindIDS(candidate)) {
                        if(MatchSearchTermCached(expansion, term)) {
                            matched = true;
                            break;
                        }
                    }
                }
                _iterStack.erase(candidate);
            }
        } else if(!matched && IsPattern(idsInDB)) {
            Pattern* candidatePattern = AsPattern(idsInDB);
            const std::string queryGlyph = IsIdeograph(term) ? AsIdeograph(term)->toString() : std::string();
            const auto& children = candidatePattern->GetpIDSRef();
            for(size_t childIndex = 0; childIndex < children.size(); childIndex++) {
                if(IsIdeograph(term)) {
                    bool skipChild = false;
                    for(const HVOriginRange& origin: candidatePattern->GetHVOriginRanges())
                        if(origin.glyph != queryGlyph && childIndex >= origin.first && childIndex < origin.last) {
                            skipChild = true;
                            break;
                        }
                    if(skipChild) continue;
                }
                IDS* child = children[childIndex].get();
                if(MatchSearchTermCached(child, term)) {
                    matched = true;
                    break;
                }
            }
        }
    }

    _searchTermMemo.emplace(std::move(key), matched);
    return matched;
}

bool IDSdatabase::MatchSearchTerms(IDS* idsInDB, const IDSOwnerList& terms) {
    if(idsInDB == nullptr || terms.empty()) return false;

    const std::vector<IDSOwnerList> expandedTerms = ExpandQueryOnlyTerms(terms);
    if(expandedTerms.empty()) return false;

    for(const auto& expanded: expandedTerms) {
        size_t residueIndex = expanded.size();
        for(size_t index = 0; index < expanded.size(); index++)
            if(IsResidueCountQuery(expanded[index].get())) {
                if(residueIndex != expanded.size()) return false;
                residueIndex = index;
            }

        if(residueIndex == expanded.size()) {
            if(expanded.size() == 1 && (IsPattern(expanded.front().get()) || IsIdeograph(expanded.front().get())) &&
                IsSearchTermMemoSafe(expanded.front().get()) && _variableBindings.empty()) {
                if(MatchSearchTermCached(idsInDB, expanded.front().get())) return true;
                continue;
            }

            // 单项 <any> 的固定分支可先排除不可能命中的候选；可能命中时仍由下方消费匹配确认。
            if(expanded.size() == 1 && _variableBindings.empty() &&
                IsSearchExpression(expanded.front().get())) {
                SearchExpression* any = AsSearchExpression(expanded.front().get());
                if(any->GetMode() == SEARCH_EXPRESSION_ANY && any->GetExceptTerms().empty() &&
                    !any->GetTerms().empty()) {
                    bool prefilterSafe = true;
                    bool possibleMatch = false;
                    for(const auto& choice: any->GetTerms()) {
                        if(!IsSearchTermMemoSafe(choice.get())) {
                            prefilterSafe = false;
                            break;
                        }
                        if(MatchSearchTermCached(idsInDB, choice.get())) {
                            possibleMatch = true;
                            break;
                        }
                    }
                    if(prefilterSafe && !possibleMatch) continue;
                }
            }

            // 多项 search 的完整消费匹配很昂贵，尤其是 lv2 将一个条件
            // 展开为 <any=...> 后会在每个节点尝试多个等价分支。先做
            // 不改变语义的必要条件筛选，失败时无需克隆候选树并递归消费。
            bool canPrefilter = expanded.size() > 1 && _variableBindings.empty();
            if(canPrefilter) {
                // 每个条件都在独立候选副本上检查；部分 IDS 分支会在匹配过程中
                // 改写临时树，因此不能污染后面的正式匹配。
                auto savedIterStack = std::move(_iterStack);
                _iterStack.clear();
                auto savedVariableBindings = std::move(_variableBindings);
                _variableBindings.clear();
                auto savedSearchTermMemo = std::move(_searchTermMemo);
                _searchTermMemo.clear();
                bool prefilterMatched = true;
                std::vector<IDS*> prefilterTerms;
                prefilterTerms.reserve(expanded.size());
                for(const auto& term: expanded) {
                    if(!IsIndependentSearchTermMemoSafe(term.get())) {
                        canPrefilter = false;
                        break;
                    }
                    prefilterTerms.push_back(term.get());
                }
                // 先检查直接字形，再检查含多个候选的 <any>；通常能更早
                // 淘汰候选，避免对每个 <any> 分支重复递归。
                std::stable_sort(prefilterTerms.begin(), prefilterTerms.end(), [](IDS* left, IDS* right) {
                    const bool leftAny = IsSearchExpression(left);
                    const bool rightAny = IsSearchExpression(right);
                    return leftAny != rightAny ? !leftAny : false;
                });
                for(IDS* term: prefilterTerms) {
                    if(!canPrefilter) break;
                    IDSOwner prefilterCandidate = idsInDB->Clone();
                    _iterStack.clear();
                    std::vector<IDS*> oneTerm = {term};
                    if(!IDSsearch(prefilterCandidate.get(), oneTerm) || !oneTerm.empty()) {
                        prefilterMatched = false;
                        break;
                    }
                }
                _iterStack = std::move(savedIterStack);
                _variableBindings = std::move(savedVariableBindings);
                _searchTermMemo = std::move(savedSearchTermMemo);
                if(canPrefilter && !prefilterMatched) continue;
            }

            std::vector<IDS*> pending;
            pending.reserve(expanded.size());
            for(const auto& term: expanded)
                pending.push_back(term.get());

            IDSOwner candidate = idsInDB->Clone();
            if(IDSsearch(candidate.get(), pending) && pending.empty()) return true;
            continue;
        }

        SearchParam*      residue = AsSearchParam(expanded[residueIndex].get());
        std::vector<IDS*> beforeTerms;
        std::vector<IDS*> afterTerms;
        beforeTerms.reserve(residueIndex);
        afterTerms.reserve(expanded.size() - residueIndex - 1);
        for(size_t index = 0; index < expanded.size(); index++) {
            if(index < residueIndex)
                beforeTerms.push_back(expanded[index].get());
            else if(index > residueIndex)
                afterTerms.push_back(expanded[index].get());
        }

        // A full match of one glyph consumes the entire candidate, so residue is zero.
        // Exact zero retains the same-glyph and identical-IDS search behavior.
        // ideograph+suffix is also treated as a single glyph, so residue is zero.
        bool wholeGlyphMatch = false;
        if(beforeTerms.size() == 1 && afterTerms.empty()) {
            IDS* beforeTerm = beforeTerms.front();
            if(IsIdeograph(beforeTerm))
                wholeGlyphMatch = MatchesSameIDS(idsInDB, *AsIdeograph(beforeTerm), true);
            else if(IsSearchExpression(beforeTerm) &&
                AsSearchExpression(beforeTerm)->GetMode() == SEARCH_EXPRESSION_ANY)
                // <any=A,B> 作为一个部件搜索项直接传递时，仍需保留 residue=0
                // 对“候选字形本身命中 A 或 B”的处理。
                wholeGlyphMatch = IDSmatch(idsInDB, beforeTerm);
        }
        if(wholeGlyphMatch) {
            if(residue->GetStrokeMinimum() == 0 && residue->GetStrokeMaximum() == 0) return true;
            continue;
        }

        IDSOwner remainder = idsInDB->Clone();
        if(!beforeTerms.empty()) {
            std::vector<IDS*> pending = beforeTerms;
            IDSOwner          reduced;
            if(!IDSsearch(remainder.get(), pending, &reduced) || !pending.empty()) continue;
            if(reduced != nullptr) remainder = std::move(reduced);
        }
        if(!afterTerms.empty()) {
            std::vector<IDS*> pending = afterTerms;
            IDSOwner          ignored;
            IDSOwner          validation = idsInDB->Clone();
            if(!IDSsearch(validation.get(), pending, &ignored) || !pending.empty()) continue;
        }
        if(MatchStrokeCount(remainder.get(), residue)) return true;
    }
    return false;
}
bool IDSdatabase::MatchIdeograph(IDS* idsInDB, IDS* ids) {
    if(IsWildcard(ids)) return true;
    if(IsIdeograph(ids)) {
        const Ideograph query = *AsIdeograph(ids);
        if(IdeoEqual(*AsIdeograph(idsInDB), query, true)) return true;
        if(config.fuzzyMatch.excludeNonEquivalentSameIDS &&
            IsNonEquivalentSameIDS(*AsIdeograph(idsInDB), query))
            return false;

        // 查询预处理会生成同 IDS 变体；这里再保留一个局部兜底，
        // 确保搜索表达式或其他直接调用 IDSmatch 的路径也遵守相同集合。
        const auto& variantIndex = GetSameIDSVariantIndex();
        const auto found = variantIndex.find(query);
        if(found != variantIndex.end()) {
            const bool strictStrokeSuffix = IsSingleStrokeIdeograph(query);
            for(const Ideograph& variant: found->second) {
                if(strictStrokeSuffix && variant.GetSuffix() != query.GetSuffix() &&
                    !IsCJKRadicalStrokeAlias(query, variant))
                    continue;
                if(IdeoEqual(*AsIdeograph(idsInDB), variant, true)) return true;
            }
        }
        return false;
    }
    if(!IsPattern(ids)) return false;

    Ideograph candidateIdeo = *AsIdeograph(idsInDB);
    if(_iterStack.find(candidateIdeo) != _iterStack.end()) return false;

    _iterStack.insert(candidateIdeo);
    bool              matched     = false;
    std::vector<IDS*> expandedIDS = FindIDS(candidateIdeo);
    // HV 缓存会把部件继续展开；变量需要同时看到原始 IDS，才能把一个原始部件作为整体绑定。
    const auto rawDefinitions = _rawIDSDB.find(candidateIdeo);
    if(rawDefinitions != _rawIDSDB.end()) {
        for(const auto& raw: rawDefinitions->second) {
            bool duplicate = false;
            for(IDS* expanded: expandedIDS)
                if(IDSequal(expanded, raw.get())) {
                    duplicate = true;
                    break;
                }
            if(!duplicate) expandedIDS.push_back(raw.get());
        }
    }
    for(auto expanded: expandedIDS) {
        matched = IDSmatch(expanded, ids);
        if(matched) break;
    }
    _iterStack.erase(candidateIdeo);
    return matched;
}

bool IDSdatabase::MatchStroke(IDS* idsInDB, IDS* ids) {
    if(IsWildcard(ids)) return true;
    if(!IsStroke(ids)) return false;
    return (*AsStroke(idsInDB)) == StrokeSimplify(*AsStroke(ids));
}

bool IDSdatabase::MatchPattern(IDS* idsInDB, IDS* ids, bool surroundEqual) {
    if(IsWildcard(ids)) return true;

    Pattern* candidate = AsPattern(idsInDB);
    if(IsPattern(ids)) return MatchPatternPair(candidate, AsPattern(ids), surroundEqual);

    if(!IsIdeograph(ids)) return false;

    const Ideograph query = *AsIdeograph(ids);
    if(HasMatchingHVOrigin(candidate, query)) return true;
    if(HasForeignHVOrigin(candidate, query)) return false;

    bool              queryHasUniqueSeparator = false;
    const auto        queryDefinitions        = _rawIDSUniqueSeparators.find(query);
    if(queryDefinitions != _rawIDSUniqueSeparators.end()) {
        for(const std::string& separator: queryDefinitions->second)
            if(!separator.empty()) {
                queryHasUniqueSeparator = true;
                break;
            }
    }
    if(config.fuzzyMatch.excludeNonEquivalentSameIDS &&
        queryHasUniqueSeparator != !idsInDB->GetUniqueSeparator().empty())
        return false;
    bool              matched       = false;
    std::vector<IDS*> expandedQuery = FindIDS(*AsIdeograph(ids));
    for(auto expanded: expandedQuery) {
        matched = IDSmatch(idsInDB, expanded);
        if(matched) break;
    }
    return matched;
}

bool IDSdatabase::MatchEnclosingComponentExpansion(Pattern* pidsInDB, Pattern* pids) {
    if(PatternChildCount(pids) != 2 || PatternChildCount(pidsInDB) != 2) return false;
    if(IsWildcard(PatternChild(pids, 0)) || !IsWildcard(PatternChild(pids, 1))) return false;

    const bool isHorizontalQuery          = pids->GetIDC() == IDC_LEFT_RIGHT || pids->GetIDC() == IDC_HORIZONAL_ARRANGE;
    const bool isVerticalQuery            = pids->GetIDC() == IDC_ABOVE_BELOW || pids->GetIDC() == IDC_VERTICAL_ARRANGE;
    const bool isHorizontalEnclosingMatch = isHorizontalQuery && pidsInDB->GetIDC() == IDC_SURROUND_LOWERLEFT;
    const bool isVerticalEnclosingMatch   = isVerticalQuery && pidsInDB->GetIDC() == IDC_SURROUND_UPPERLEFT;
    if(!isHorizontalEnclosingMatch && !isVerticalEnclosingMatch) return false;

    // 只接受“首部件 + 末位通配符”的开放查询，避免把“⿰⬚目”等受限查询放宽。
    // 包围部件先按自身 IDS 展开；例如“礼”展开为“⿰礻乚”后可匹配“⿰礻⬚”。
    return IDSmatch(PatternChild(pidsInDB, 0), pids);
}
bool IDSdatabase::MatchNormalizedPatterns(Pattern* pidsInDB, Pattern* pids) {
    VariableBindingScope bindings(_variableBindings);
    if(pids->GetIDC() != pidsInDB->GetIDC()) return false;

    if(IsArrangeIDC(pids->GetIDC()))
        return bindings.Finish(IDSarrayMatch(BorrowPatternIDS(pidsInDB), BorrowPatternIDS(pids), pids->GetIDC()));

    if(pids->GetIDC() == IDC_OVERLAY && PatternChildCount(pids) == PatternChildCount(pidsInDB)) {
        if(IDSmatch(PatternChild(pidsInDB, 0), PatternChild(pids, 0)) &&
            IDSmatch(PatternChild(pidsInDB, 1), PatternChild(pids, 1)))
            return bindings.Finish(true);

        _variableBindings = bindings.GetSaved();
        if(IDSmatch(PatternChild(pidsInDB, 0), PatternChild(pids, 1)) &&
            IDSmatch(PatternChild(pidsInDB, 1), PatternChild(pids, 0)))
            return bindings.Finish(true);
        return false;
    }

    if(PatternChildCount(pids) != PatternChildCount(pidsInDB)) return false;
    for(size_t i = 0; i < PatternChildCount(pidsInDB); i++)
        if(!IDSmatch(PatternChild(pidsInDB, i), PatternChild(pids, i))) return false;
    return bindings.Finish(true);
}

bool IDSdatabase::MatchPatternPair(Pattern* pidsInDB, Pattern* pids, bool surroundEqual) {
    (void)surroundEqual;
    if(MatchEnclosingComponentExpansion(pidsInDB, pids)) return true;

    bool matched = MatchNormalizedPatterns(pidsInDB, pids);
    if(!matched) matched = IDSsurroundMatch(pidsInDB, pids);
    return matched;
}

const IDCtype surroundIDC[] = {
    IDC_SURROUND_FULL,
    IDC_SURROUND_ABOVE,
    IDC_SURROUND_BELOW,
    IDC_SURROUND_LEFT,
    IDC_SURROUND_RIGHT,
    IDC_SURROUND_UPPERLEFT,
    IDC_SURROUND_UPPERRIGHT,
    IDC_SURROUND_LOWERLEFT,
    IDC_SURROUND_LOWERRIGHT,
};

static bool IsSurroundIDC(IDCtype idc) {
    for(auto i: surroundIDC)
        if(i == idc) return true;
    return false;
}

// Only to process surround structure: ⿸①② vs ⿸⿸①1①2②(①=⿸①1①2)
bool IDSdatabase::IDSsurroundMatch(Pattern* pidsInDB, Pattern* pids) {
    // Precedent condition checking
    if(pids->GetIDC() != pidsInDB->GetIDC()) return false;
    if(!IsSurroundIDC(pids->GetIDC())) return false;
    if(!IsWildcard(PatternChild(pids, 1))) return false;
    // Extract the first element of 'pidsInDB'
    auto      idsInDBelement1 = PatternChild(pidsInDB, 0), idselement1 = PatternChild(pids, 0);
    IDSOwner  newIDS;
    Ideograph findIdeo(0);
    if(IsIdeograph(idsInDBelement1)) {
        if(IDSmatch(idsInDBelement1, idselement1)) return true;
        // Avoid loopback
        if(_iterStack.find(*AsIdeograph(idsInDBelement1)) == _iterStack.end()) {
            findIdeo = *AsIdeograph(idsInDBelement1);
            _iterStack.insert(findIdeo);
        } else
            return false;
        std::vector<IDS*> idsInDB1extr = FindIDS(*AsIdeograph(idsInDBelement1));
        for(auto i: idsInDB1extr)
            if(IsPattern(i)) {
                Pattern* pi = AsPattern(i);
                if(pi->GetIDC() == pids->GetIDC()) {
                    newIDS = i->Clone();
                    break;
                }
            }
    } else if(IsPattern(idsInDBelement1)) {
        Pattern* pidsInDBelement1 = AsPattern(idsInDBelement1);
        if(pidsInDBelement1->GetIDC() == pids->GetIDC()) newIDS = idsInDBelement1->Clone();
    } else
        return false;
    // Matching
    bool out = (newIDS != nullptr && IsPattern(newIDS.get())) ? IDSsurroundMatch(AsPattern(newIDS.get()), pids) : false;
    _iterStack.erase(findIdeo);
    return out;
}

size_t IDSdatabase::IDSsubarray(const std::vector<IDS*>& s, const std::vector<IDS*>& ideo) {
    size_t n = s.size(), m = ideo.size();
    if(m == 0 || n < m) return SIZE_MAX;
    for(size_t i = 0; i <= n - m; ++i) {
        bool match = true;
        for(size_t j = 0; j < m; ++j) {
            if(!((IsIdeograph(s[i + j]) && IsIdeograph(ideo[j]))
                       ? IdeoEqual(*AsIdeograph(s[i + j]), *AsIdeograph(ideo[j]), true)
                       : IDSequal(s[i + j], ideo[j]))) {
                match = false;
                break;
            }
        }
        if(match) return i;
    }
    return SIZE_MAX;
}

size_t IDSvectorFind(const std::vector<IDS*>& vec, IDS* find) {
    for(size_t i = 0; i < vec.size(); i++)
        if(IDSequal(find, vec[i])) return i;
    return SIZE_MAX;
}

bool IDSdatabase::HasImpossibleStrokeLowerBound(IDS* remain, const std::vector<IDS*>& terms) {
    if(remain == nullptr || config.fuzzyMatch.unificationLevel != IWDS_UNIFICATION_NONE) return false;

    uint32_t          maximum  = 0;
    const std::string cacheKey = remain->toString();
    const auto        cached   = _strokeMaximumCache.find(cacheKey);
    if(cached != _strokeMaximumCache.end())
        maximum = cached->second;
    else {
        const StrokeCountSet counts = GetStrokeCounts(remain);
        if(counts.empty()) return false;
        maximum = *std::max_element(counts.begin(), counts.end());
        _strokeMaximumCache.insert({cacheKey, maximum});
    }

    // 每个普通 search 项都必须在当前剩余子树中找到。这里只比较单项的
    // 最小笔画数，不把多个条件相加；条件可能落在同一节点，也可能分布
    // 在不同的 IDS 分支上，相加会把合法结果错误剪掉。
    for(IDS* term: terms) {
        if(IsStrokeCountQuery(term)) {
            if(AsSearchParam(term)->GetStrokeMinimum() > maximum) return true;
            continue;
        }
        if(IsResidueCountQuery(term) || IsWildcard(term) || IsVariable(term)) continue;

        const bool isSimpleAny = IsSearchExpression(term) &&
            AsSearchExpression(term)->GetMode() == SEARCH_EXPRESSION_ANY &&
            AsSearchExpression(term)->GetExceptTerms().empty();
        if(!IsIdeograph(term) && !isSimpleAny) continue;

        // <any=A,B> 的下界是 min(stroke(A), stroke(B))。
        // 无法计算笔画数时 GetStrokeRange 返回 0，因此不会误剪枝。
        if(GetStrokeRange(term).minimum > maximum) return true;
    }
    return false;
}

// 返回值为是否命中任一条件；terms 为空表示全部条件已满足。
bool IDSdatabase::IDSsearch(IDS* remain, std::vector<IDS*>& terms, IDSOwner* newRemain) {
    if(remain == nullptr) return false;
    auto simpleGlyphAny = [](IDS* term) -> SearchExpression* {
        if(!IsSearchExpression(term)) return nullptr;
        SearchExpression* any = AsSearchExpression(term);
        if(any->GetMode() != SEARCH_EXPRESSION_ANY || !any->GetExceptTerms().empty() || any->GetTerms().empty())
            return nullptr;
        for(const auto& choice: any->GetTerms())
            if(!IsIdeograph(choice.get())) return nullptr;
        return any;
    };
    auto saveRemainder = [&]() {
        if(newRemain != nullptr) *newRemain = remain->Clone();
    };
    if(terms.empty()) {
        saveRemainder();
        return true;
    }
    if(HasImpossibleStrokeLowerBound(remain, terms)) return false;

    std::vector<size_t> removedIndices;
    bool                termFound = false;

    // 当前节点只能占用一个搜索项。笔画中的 <any=字形…> 留给下方逐 token 消费。
    for(size_t termIndex = 0; termIndex < terms.size(); termIndex++) {
        IDS* term = terms[termIndex];
        if(IsStroke(remain) && simpleGlyphAny(term) != nullptr) continue;
        if(!IsResidueCountQuery(term) && IDSmatch(remain, term)) {
            removedIndices.push_back(termIndex);
            break;
        }
    }
    if(EraseMarkedTerms(terms, removedIndices)) termFound = true;
    if(terms.empty()) {
        saveRemainder();
        return true;
    }

    if(IsIdeograph(remain)) {
        const Ideograph ideograph = *AsIdeograph(remain);
        if(_iterStack.find(ideograph) != _iterStack.end()) return termFound;

        _iterStack.insert(ideograph);
        bool              found            = termFound;
        bool              completedBranch  = false;
        bool              hasPartialBranch = termFound;
        std::vector<IDS*> bestTerms        = terms;
        size_t            bestRemaining    = terms.size();
        IDSOwner          bestRemain;
        bool canExpand = true;
        if(config.fuzzyMatch.excludeNonEquivalentSameIDS)
            for(IDS* term: terms)
                if(IsIdeograph(term) && IsNonEquivalentSameIDS(ideograph, *AsIdeograph(term))) {
                    canExpand = false;
                    break;
                }
        if(canExpand && _cache_BasicIdeo.find(ideograph) == _cache_BasicIdeo.end()) {
            for(IDS* expanded: FindIDS(ideograph)) {
                IDSOwner          expandedCandidate = expanded->Clone();
                IDSOwner          expandedRemain;
                std::vector<IDS*> branchTerms = terms;
                if(IDSsearch(expandedCandidate.get(), branchTerms, newRemain == nullptr ? nullptr : &expandedRemain)) {
                    found = true;
                    if(branchTerms.empty()) {
                        // 一个 IDS 分支已经满足全部条件，提交这个分支的结果。
                        terms = std::move(branchTerms);
                        if(newRemain != nullptr && expandedRemain != nullptr) *newRemain = std::move(expandedRemain);
                        completedBranch = true;
                        break;
                    }

                    // 只记录单个 IDS 分支的最佳部分匹配，不能把不同分支的
                    // 剩余条件串接起来。
                    if(!hasPartialBranch || branchTerms.size() < bestRemaining) {
                        hasPartialBranch = true;
                        bestRemaining    = branchTerms.size();
                        bestTerms        = std::move(branchTerms);
                        if(newRemain != nullptr && expandedRemain != nullptr) bestRemain = std::move(expandedRemain);
                    }
                }
            }
        }
        if(!completedBranch && hasPartialBranch) {
            terms = std::move(bestTerms);
            if(newRemain != nullptr && bestRemain != nullptr) *newRemain = std::move(bestRemain);
        }
        _iterStack.erase(ideograph);
        return found;
    }

    if(IsPattern(remain)) {
        Pattern* remainPattern = AsPattern(remain);
        if(IsArrangeIDC(remainPattern->GetIDC())) {
            for(size_t index = 0; index < terms.size(); index++) {
                IDSOwnerList componentAlternatives;
                if(IsIdeograph(terms[index]))
                    componentAlternatives.push_back(terms[index]->Clone());
                else if(IsSearchExpression(terms[index]) &&
                    AsSearchExpression(terms[index])->GetMode() == SEARCH_EXPRESSION_ANY &&
                    AsSearchExpression(terms[index])->GetExceptTerms().empty()) {
                    for(const auto& alternative: AsSearchExpression(terms[index])->GetTerms())
                        if(IsIdeograph(alternative.get())) componentAlternatives.push_back(alternative->Clone());
                }
                if(componentAlternatives.empty()) continue;

                bool componentFound = false;
                for(const auto& componentAlternative: componentAlternatives) {
                    const Ideograph componentGlyph = *AsIdeograph(componentAlternative.get());
                    for(IDS* componentIDS: FindIDS(componentGlyph)) {
                    if(componentFound || !IsPattern(componentIDS) ||
                        AsPattern(componentIDS)->GetIDC() != remainPattern->GetIDC() ||
                        !IsArrangeIDC(AsPattern(componentIDS)->GetIDC()))
                        continue;

                    const std::vector<IDS*> remainingChildren = BorrowPatternIDS(remainPattern);
                    const std::vector<IDS*> componentChildren = BorrowPatternIDS(AsPattern(componentIDS));
                    const size_t            matchPosition     = IDSsubarray(remainingChildren, componentChildren);
                    if(matchPosition == SIZE_MAX) continue;

                    bool blockedByForeignOrigin = false;
                    const std::string queryGlyph = componentGlyph.toString();
                    for(const HVOriginRange& origin: remainPattern->GetHVOriginRanges())
                        if(origin.glyph != queryGlyph && matchPosition < origin.last &&
                            matchPosition + componentChildren.size() > origin.first) {
                            blockedByForeignOrigin = true;
                            break;
                        }
                    if(blockedByForeignOrigin) continue;
                    removedIndices.push_back(index);
                    componentFound = true;
                    for(size_t childIndex = componentChildren.size(); childIndex > 0; childIndex--)
                        remainPattern->GetpIDSRef().erase(
                            remainPattern->GetpIDSRef().begin() + matchPosition + childIndex - 1);
                }
                }
            }
            if(EraseMarkedTerms(terms, removedIndices)) termFound = true;
            if(terms.empty()) {
                saveRemainder();
                return true;
            }
        }

        // 先消耗当前层的直接部件，再递归处理尚未满足的条件。
        IDSOwnerList& remainingChildren = remainPattern->GetpIDSRef();
        for(size_t termIndex = 0; termIndex < terms.size(); termIndex++) {
            if(IsStrokeCountQuery(terms[termIndex]) || IsResidueCountQuery(terms[termIndex])) continue;
            for(size_t childIndex = remainingChildren.size(); childIndex > 0; childIndex--) {
                IDS* child = remainingChildren[childIndex - 1].get();
                if(IsIdeograph(terms[termIndex]) && IsPattern(child) &&
                    HasForeignHVOrigin(AsPattern(child), *AsIdeograph(terms[termIndex])) &&
                    !HasMatchingHVOrigin(AsPattern(child), *AsIdeograph(terms[termIndex])))
                    continue;
                if(IDSmatch(child, terms[termIndex])) {
                    remainingChildren.erase(remainingChildren.begin() + childIndex - 1);
                    removedIndices.push_back(termIndex);
                    break;
                }
            }
        }
        if(EraseMarkedTerms(terms, removedIndices)) termFound = true;
        if(terms.empty()) {
            saveRemainder();
            return true;
        }

        for(auto& child: remainingChildren) {
            IDSOwner newBranch;
            if(IDSsearch(child.get(), terms, &newBranch)) {
                termFound = true;
                if(newBranch != nullptr) child = std::move(newBranch);
            }
            if(terms.empty()) {
                saveRemainder();
                return true;
            }
        }
        if(termFound) saveRemainder();
        return termFound;
    }

    if(IsStroke(remain)) {
        Stroke* remainStroke = AsStroke(remain);
        if(remainStroke->isBasicStroke()) return termFound;

        std::vector<Stroke_Data>& strokes = remainStroke->GetStrokeRef();
        for(size_t termIndex = 0; termIndex < terms.size(); termIndex++) {
            IDS* term = terms[termIndex];
            SearchExpression* any = simpleGlyphAny(term);
            if(!IsIdeograph(term) && any == nullptr) continue;
            const std::string component = IsIdeograph(term) ? AsIdeograph(term)->toString() : "";
            if(any == nullptr && _strokeDB.find(component) == _strokeDB.end()) continue;
            for(size_t strokeIndex = strokes.size(); strokeIndex > 0; strokeIndex--) {
                const Stroke_Data& token = strokes[strokeIndex - 1];
                bool matched = any == nullptr ? token.stroke == component : false;
                if(any != nullptr)
                    for(const auto& choice: any->GetTerms())
                        if(MatchSingleStrokeToken(token, *AsIdeograph(choice.get()))) {
                            matched = true;
                            break;
                        }
                if(matched) {
                    strokes.erase(strokes.begin() + strokeIndex - 1);
                    removedIndices.push_back(termIndex);
                    break;
                }
            }
        }
        if(EraseMarkedTerms(terms, removedIndices)) termFound = true;
        if(termFound) saveRemainder();
        return termFound;
    }

    if(termFound) saveRemainder();
    return termFound;
}

bool IDSdatabase::ContainsQueryOnlyOperator(IDS* ids) {
    if(IsSearchParam(ids) || IsSearchExpression(ids) || IsVariable(ids)) return true;
    if(!IsPattern(ids)) return false;

    Pattern* pattern = AsPattern(ids);
    if(IsReplaceIDC(pattern->GetIDC()) || IsSubtractIDC(pattern->GetIDC())) return true;

    for(const auto& child: pattern->GetpIDSRef())
        if(ContainsQueryOnlyOperator(child.get())) return true;
    return false;
}

IDSOwnerList IDSdatabase::BuildComponentVariants(Ideograph component) {
    IDSOwnerList variants;
    for(auto componentIDS: FindIDS(component)) {
        IDSOwnerList alternatives = HVExtractOwned(componentIDS);
        for(auto& normalizedVariant: alternatives) {
            bool duplicate = false;
            for(const auto& existing: variants)
                if(IDSequal(existing.get(), normalizedVariant.get())) {
                    duplicate = true;
                    break;
                }
            if(!duplicate) variants.push_back(std::move(normalizedVariant));
        }
    }
    return variants;
}

IDSOwner IDSdatabase::ReplaceArrangementComponents(Pattern* pattern, const Ideograph& sourceComponent,
    const IDSOwnerList& componentVariants, IDS* replacement, bool& replaced) {
    const IDSOwnerList& originalChildren = pattern->GetpIDSRef();
    IDSOwnerList        children;
    children.reserve(originalChildren.size());
    for(const auto& child: originalChildren)
        children.push_back(ReplaceComponent(child.get(), sourceComponent, componentVariants, replacement, replaced));

    IDSOwnerList replacedChildren;
    replacedChildren.reserve(children.size());
    const size_t originalSplit = pattern->GetPreferSplitPoint();
    size_t       newSplit      = SIZE_MAX;
    for(size_t index = 0; index < children.size();) {
        const size_t fragmentLength =
            FindArrangementFragmentLength(children, index, pattern->GetIDC(), componentVariants);
        if(fragmentLength != 0) {
            replacedChildren.push_back(replacement->Clone());
            if(originalSplit != SIZE_MAX && originalSplit >= index && originalSplit < index + fragmentLength)
                newSplit = replacedChildren.size() - 1;
            index   += fragmentLength;
            replaced = true;
        } else {
            replacedChildren.push_back(std::move(children[index]));
            if(originalSplit == index) newSplit = replacedChildren.size() - 1;
            index++;
        }
    }

    int overlayRange[2];
    pattern->GetOverlayRange(overlayRange);
    std::vector<IDS*> childPointers = BorrowIDSList(replacedChildren);
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, newSplit, overlayRange, pattern->GetOverlayType(),
        pattern->GetOptionalInt()));
}

IDSOwner IDSdatabase::ReplaceComponent(IDS* ids, const Ideograph& sourceComponent,
    const IDSOwnerList& componentVariants, IDS* replacement, bool& replaced) {
    if(IsIdeograph(ids)) {
        if(*AsIdeograph(ids) == sourceComponent) {
            replaced = true;
            return replacement->Clone();
        }
        return ids->Clone();
    }
    if(!IsPattern(ids)) return ids->Clone();

    for(const auto& variant: componentVariants)
        if(IDSequal(ids, variant.get())) {
            replaced = true;
            return replacement->Clone();
        }

    Pattern* pattern = AsPattern(ids);
    if(IsArrangeIDC(pattern->GetIDC()))
        return ReplaceArrangementComponents(pattern, sourceComponent, componentVariants, replacement, replaced);

    IDSOwnerList children;
    children.reserve(PatternChildCount(pattern));
    for(const auto& child: pattern->GetpIDSRef())
        children.push_back(ReplaceComponent(child.get(), sourceComponent, componentVariants, replacement, replaced));

    int overlayRange[2];
    pattern->GetOverlayRange(overlayRange);
    std::vector<IDS*> childPointers = BorrowIDSList(children);
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(), overlayRange,
        pattern->GetOverlayType(), pattern->GetOptionalInt()));
}

static IDSOwner RemoveComponent(
    IDS* ids, const Ideograph& sourceComponent, const IDSOwnerList& componentVariants, bool& removed);

static IDSOwner RemoveArrangementComponents(
    Pattern* pattern, const Ideograph& sourceComponent, const IDSOwnerList& componentVariants, bool& removed) {
    const IDSOwnerList& originalChildren = pattern->GetpIDSRef();
    IDSOwnerList        children;
    children.reserve(originalChildren.size());
    for(const auto& child: originalChildren) {
        IDSOwner reduced = RemoveComponent(child.get(), sourceComponent, componentVariants, removed);
        if(reduced != nullptr) children.push_back(std::move(reduced));
    }

    IDSOwnerList reducedChildren;
    reducedChildren.reserve(children.size());
    const size_t originalSplit = pattern->GetPreferSplitPoint();
    size_t       newSplit      = SIZE_MAX;
    for(size_t index = 0; index < children.size();) {
        const size_t fragmentLength =
            FindArrangementFragmentLength(children, index, pattern->GetIDC(), componentVariants);
        if(fragmentLength != 0) {
            removed = true;
            index  += fragmentLength;
            continue;
        }

        if(index == originalSplit) newSplit = reducedChildren.size();
        reducedChildren.push_back(std::move(children[index]));
        index++;
    }

    if(!removed) return pattern->Clone();
    if(reducedChildren.empty()) return IDSOwner();
    if(reducedChildren.size() == 1) return std::move(reducedChildren.front());

    int overlayRange[2];
    pattern->GetOverlayRange(overlayRange);
    std::vector<IDS*> childPointers = BorrowIDSList(reducedChildren);
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, newSplit, overlayRange, pattern->GetOverlayType(),
        pattern->GetOptionalInt()));
}

static IDSOwner RemoveComponent(
    IDS* ids, const Ideograph& sourceComponent, const IDSOwnerList& componentVariants, bool& removed) {
    if(IsIdeograph(ids)) {
        if(*AsIdeograph(ids) == sourceComponent) {
            removed = true;
            return IDSOwner();
        }
        return ids->Clone();
    }
    if(!IsPattern(ids)) return ids->Clone();

    for(const auto& variant: componentVariants)
        if(IDSequal(ids, variant.get())) {
            removed = true;
            return IDSOwner();
        }

    Pattern* pattern = AsPattern(ids);
    if(IsArrangeIDC(pattern->GetIDC()))
        return RemoveArrangementComponents(pattern, sourceComponent, componentVariants, removed);

    const IDSOwnerList& originalChildren = pattern->GetpIDSRef();
    IDSOwnerList        children;
    children.reserve(originalChildren.size());
    const size_t originalSplit = pattern->GetPreferSplitPoint();
    size_t       newSplit      = SIZE_MAX;
    for(size_t index = 0; index < originalChildren.size(); index++) {
        IDSOwner reduced = RemoveComponent(originalChildren[index].get(), sourceComponent, componentVariants, removed);
        if(reduced == nullptr) continue;
        if(index == originalSplit) newSplit = children.size();
        children.push_back(std::move(reduced));
    }

    if(!removed) return ids->Clone();
    if(children.empty()) return IDSOwner();
    if(children.size() == 1) return std::move(children.front());

    int overlayRange[2];
    pattern->GetOverlayRange(overlayRange);
    std::vector<IDS*> childPointers = BorrowIDSList(children);
    return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, newSplit, overlayRange, pattern->GetOverlayType(),
        pattern->GetOptionalInt()));
}

IDSOwnerList IDSdatabase::BuildReplaceQueries(Pattern* replaceQuery) {
    IDSOwnerList queries;
    if(PatternChildCount(replaceQuery) != 3) return queries;

    IDS* source          = PatternChild(replaceQuery, 0);
    IDS* sourceComponent = PatternChild(replaceQuery, 1);
    IDS* replacement     = PatternChild(replaceQuery, 2);
    if(!IsIdeograph(source) || !IsIdeograph(sourceComponent) ||
        (!IsWildcard(replacement) && ContainsQueryOnlyOperator(replacement)))
        return queries;

    const Ideograph component         = *AsIdeograph(sourceComponent);
    IDSOwnerList    componentVariants = BuildComponentVariants(component);
    for(auto sourceIDS: FindIDS(*AsIdeograph(source))) {
        IDSOwnerList sourceAlternatives = HVExtractOwned(sourceIDS);
        for(const auto& normalizedSource: sourceAlternatives) {
            bool     replaced = false;
            IDSOwner query =
                ReplaceComponent(normalizedSource.get(), component, componentVariants, replacement, replaced);
            if(!replaced) continue;

            bool duplicate = false;
            for(const auto& existing: queries)
                if(IDSequal(existing.get(), query.get())) {
                    duplicate = true;
                    break;
                }
            if(!duplicate) queries.push_back(std::move(query));
        }
    }
    return queries;
}

IDSOwnerList IDSdatabase::BuildSubtractQueries(Pattern* subtractQuery) {
    IDSOwnerList queries;
    if(PatternChildCount(subtractQuery) != 2) return queries;

    IDS* source          = PatternChild(subtractQuery, 0);
    IDS* sourceComponent = PatternChild(subtractQuery, 1);
    if(!IsIdeograph(source) || !IsIdeograph(sourceComponent)) return queries;

    const Ideograph component         = *AsIdeograph(sourceComponent);
    IDSOwnerList    componentVariants = BuildComponentVariants(component);
    for(auto sourceIDS: FindIDS(*AsIdeograph(source))) {
        IDSOwnerList sourceAlternatives = HVExtractOwned(sourceIDS);
        for(const auto& normalizedSource: sourceAlternatives) {
            bool     removed = false;
            IDSOwner query   = RemoveComponent(normalizedSource.get(), component, componentVariants, removed);
            if(!removed || query == nullptr) continue;

            bool duplicate = false;
            for(const auto& existing: queries)
                if(IDSequal(existing.get(), query.get())) {
                    duplicate = true;
                    break;
                }
            if(!duplicate) queries.push_back(std::move(query));
        }
    }
    return queries;
}

IDSOwner IDSdatabase::PreprocessReplaceSubtractQuery(IDS* ids, size_t maximum) {
    if(ids == nullptr || maximum == 0) return IDSOwner();

    auto makeAny = [&](const IDSOwnerList& choices) -> IDSOwner {
        if(choices.empty()) return IDSOwner();
        if(choices.size() == 1) return choices.front()->Clone();

        IDSOwnerList alternatives;
        for(const auto& choice: choices) {
            bool truncated = false;
            AppendUniqueOwned(alternatives, choice->Clone(), maximum, truncated);
            if(alternatives.size() >= maximum) break;
        }
        if(alternatives.empty()) return IDSOwner();
        if(alternatives.size() == 1) return alternatives.front()->Clone();
        return IDSOwner(new SearchExpression(std::move(alternatives), SEARCH_EXPRESSION_ANY));
    };

    auto appendClone = [maximum](IDSOwnerList& target, IDS* value) {
        if(value == nullptr) return;
        bool truncated = false;
        AppendUniqueOwned(target, value->Clone(), maximum, truncated);
    };

    std::function<IDSOwner(IDS*)> preprocessNode;
    auto appendChoice = [&](IDSOwnerList& target, const IDSOwner& choice, bool flattenAny) {
        if(choice == nullptr) return;
        if(flattenAny && IsSearchExpression(choice.get()) &&
            AsSearchExpression(choice.get())->GetMode() == SEARCH_EXPRESSION_ANY &&
            AsSearchExpression(choice.get())->GetExceptTerms().empty()) {
            for(const auto& nested: AsSearchExpression(choice.get())->GetTerms())
                appendClone(target, nested.get());
            return;
        }
        appendClone(target, choice.get());
    };

    preprocessNode = [&](IDS* value) -> IDSOwner {
        if(value == nullptr) return IDSOwner();

        if(IsReplaceQuery(value) || IsSubtractQuery(value))
            return makeAny(ExpandQueryOnlyTerm(value));

        if(IsSearchExpression(value)) {
            SearchExpression* search = AsSearchExpression(value);
            IDSOwnerList        terms;
            IDSOwnerList        exceptTerms;
            const bool          flattenTerms = search->GetMode() != SEARCH_EXPRESSION_ALL;

            for(const auto& term: search->GetTerms()) {
                IDSOwner choice = preprocessNode(term.get());
                if(choice == nullptr) return IDSOwner();
                appendChoice(terms, choice, flattenTerms);
            }
            for(const auto& term: search->GetExceptTerms()) {
                IDSOwner choice = preprocessNode(term.get());
                if(choice == nullptr) return IDSOwner();
                // Each exception term excludes independently, so generated
                // replacement/subtraction alternatives can be flattened.
                appendChoice(exceptTerms, choice, true);
            }
            return IDSOwner(new SearchExpression(
                std::move(terms), search->GetMode(), std::move(exceptTerms)));
        }

        if(!IsPattern(value)) return value->Clone();

        Pattern*    pattern = AsPattern(value);
        IDSOwnerList children;
        children.reserve(pattern->GetpIDSRef().size());
        for(const auto& child: pattern->GetpIDSRef()) {
            IDSOwner expanded = preprocessNode(child.get());
            if(expanded == nullptr) return IDSOwner();
            children.push_back(std::move(expanded));
        }

        std::vector<IDS*> childPointers;
        childPointers.reserve(children.size());
        for(const auto& child: children)
            childPointers.push_back(child.get());

        int overlayRange[2] = {0, 0};
        pattern->GetOverlayRange(overlayRange);
        return IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(),
            overlayRange, pattern->GetOverlayType(), pattern->GetOptionalInt()));
    };

    return preprocessNode(ids);
}

IDSOwnerList IDSdatabase::ExpandQueryOnlyTerm(IDS* term) {
    if(term == nullptr) return IDSOwnerList();
    if(IsSubtractQuery(term)) return BuildSubtractQueries(AsPattern(term));
    if(IsReplaceQuery(term)) return BuildReplaceQueries(AsPattern(term));

    IDSOwnerList out;
    out.push_back(term->Clone());
    return out;
}

std::vector<IDSOwnerList> IDSdatabase::ExpandQueryOnlyTerms(const IDSOwnerList& terms) {
    std::vector<IDSOwnerList>   out;
    IDSOwnerList                current;
    std::function<void(size_t)> expand = [&](size_t index) {
        if(index == terms.size()) {
            IDSOwnerList expanded;
            expanded.reserve(current.size());
            for(const auto& term: current)
                expanded.push_back(term->Clone());
            out.push_back(std::move(expanded));
            return;
        }

        const IDSOwnerList alternatives = ExpandQueryOnlyTerm(terms[index].get());
        if(alternatives.empty()) return;

        for(const auto& alternative: alternatives) {
            current.push_back(alternative->Clone());
            expand(index + 1);
            current.pop_back();
        }
    };
    expand(0);
    return out;
}

IDSOwnerList IDSdatabase::ExpandIWDSQuery(IDS* ids, size_t maximum) {
    IDSOwnerList out;
    if(ids == nullptr || maximum == 0) return out;

    auto append = [&](IDSOwnerList& target, IDSOwner value, IDSPreprocessRule rule = IDS_PREPROCESS_NONE) {
        if(value == nullptr) return;
        const std::string expression = value->toString();
        bool              truncated  = false;
        AppendUniqueOwned(target, std::move(value), maximum, truncated);
        if(!truncated) RecordQueryPreprocessRule(expression, rule);
    };
    std::function<IDSOwnerList(IDS*)>                             expandNode;
    std::function<std::vector<IDSOwnerList>(const IDSOwnerList&)> expandList;

    // 对具体查询执行一次 IWDS 模板匹配，并把模板变量替换成查询中
    // 实际绑定的部件。这样后续数据库匹配只处理已经展开的普通 IDS。
    auto appendPatternGroupAlternatives = [&](IDS* query, IDSOwnerList& target) {
        if(query == nullptr || !IsPattern(query)) return;
        const std::string queryShape = IWDSIDSShapeKey(query);
        if(queryShape.empty()) return;

        const IDCtype queryIDC                        = AsPattern(query)->GetIDC();
        const bool    allowVariableStructureTransform = IsIWDSVariableStructureTransformIDC(queryIDC);

        const VariableBindings savedBindings = _variableBindings;
        for(size_t level = IWDS_UNIFICATION_LV1;
            level <= static_cast<size_t>(config.fuzzyMatch.unificationLevel) && level < IWDS_UNIFICATION_LEVEL_COUNT;
            level++) {
            const auto shapeFound = _unifiableIDSShapeIndex[level].find(queryShape);
            if(shapeFound == _unifiableIDSShapeIndex[level].end()) continue;

            for(const size_t groupIndex: shapeFound->second) {
                if(groupIndex >= _unifiableIDSParsedGroups[level].size()) continue;
                const UnifiableIDSParsedGroup& group = _unifiableIDSParsedGroups[level][groupIndex];
                bool                           groupHasVariableStructureAlternative = false;
                if(allowVariableStructureTransform) {
                    for(const auto& member: group) {
                        if(!IsPattern(member.get()) ||
                            PatternChildCount(AsPattern(member.get())) != PatternChildCount(AsPattern(query)))
                            continue;
                        if(AsPattern(member.get())->GetIDC() != queryIDC &&
                            IsIWDSVariableStructureTransformIDC(AsPattern(member.get())->GetIDC()) &&
                            !ContainsIWDSFixedAtom(member.get())) {
                            groupHasVariableStructureAlternative = true;
                            break;
                        }
                    }
                }
                for(const auto& queryMember: group) {
                    if(IWDSIDSShapeKey(queryMember.get()) != queryShape) continue;

                    _variableBindings.clear();
                    // 仅对纯变量模板执行结构变换放宽；含固定部件的模板仍需
                    // 走 querySide=true，以保留具体嵌套字形的约束。
                    const bool isPureVariableTemplate = !ContainsIWDSFixedAtom(queryMember.get());
                    const bool structureTransform     = allowVariableStructureTransform &&
                        groupHasVariableStructureAlternative && isPureVariableTemplate;
                    const bool queryMatched =
                        MatchIWDSUnificationPattern(query, queryMember.get(), !structureTransform);
                    if(!queryMatched) {
                        _variableBindings = savedBindings;
                        continue;
                    }
                    const VariableBindings bindings = _variableBindings;
                    _variableBindings               = savedBindings;

                    for(const auto& targetMember: group) {
                        std::string expression = targetMember->toString();
                        for(const auto& binding: bindings) {
                            // IWDS template instantiation has no arrangement context. A sequence
                            // binding is therefore not representable as one <var=...> replacement.
                            if(binding.second.arrangement != IDC_UNKNOWN || binding.second.nodes.size() != 1) continue;
                            const std::string token  = "<var=" + binding.first + ">";
                            const std::string& value = binding.second.nodes.front();
                            size_t            offset = 0;
                            while((offset = expression.find(token, offset)) != std::string::npos) {
                                expression.replace(offset, token.size(), value);
                                offset += value.size();
                            }
                        }

                        IDSOwner instantiated = ParseIDSOwned(expression);
                        if(instantiated != nullptr && !IDSequal(instantiated.get(), query))
                            append(target, std::move(instantiated), IWDSRuleForLevel(level));
                    }
                }
            }
        }
        _variableBindings = savedBindings;
    };

    expandList = [&](const IDSOwnerList& source) {
        std::vector<IDSOwnerList> result(1);
        for(const auto& child: source) {
            const IDSOwnerList choices = expandNode(child.get());
            if(choices.empty()) return std::vector<IDSOwnerList>();

            std::vector<IDSOwnerList> next;
            for(const auto& prefix: result) {
                for(const auto& choice: choices) {
                    if(next.size() >= maximum) break;
                    IDSOwnerList combined;
                    for(const auto& item: prefix)
                        combined.push_back(item->Clone());
                    combined.push_back(choice->Clone());
                    next.push_back(std::move(combined));
                }
                if(next.size() >= maximum) break;
            }
            result = std::move(next);
            if(result.empty()) break;
        }
        return result;
    };

    expandNode = [&](IDS* value) {
        IDSOwnerList result;
        if(value == nullptr) return result;
        append(result, value->Clone());

        const bool allowIWDS = config.fuzzyMatch.unificationLevel >= IWDS_UNIFICATION_SOURCE_CODE_SEPARATION &&
            config.fuzzyMatch.unificationLevel < IWDS_UNIFICATION_LEVEL_COUNT;

        if(IsIdeograph(value)) {
            const Ideograph ideograph = *AsIdeograph(value);
            const auto&     variantIndex = GetSameIDSVariantIndex();
            const auto      sameIDS   = variantIndex.find(ideograph);
            if(sameIDS != variantIndex.end()) {
                const bool strictStrokeSuffix = IsSingleStrokeIdeograph(ideograph);
                for(const Ideograph& variant: sameIDS->second) {
                    if(strictStrokeSuffix && variant.GetSuffix() != ideograph.GetSuffix() &&
                        !IsCJKRadicalStrokeAlias(ideograph, variant))
                        continue;
                    append(result, IDSOwner(new Ideograph(variant)), IDS_PREPROCESS_SAME_IDS);
                }
            }
            if(!allowIWDS) return result;

            for(size_t level = IWDS_UNIFICATION_SOURCE_CODE_SEPARATION;
                level <= static_cast<size_t>(config.fuzzyMatch.unificationLevel) &&
                level < IWDS_UNIFICATION_LEVEL_COUNT;
                level++) {
                const auto found = _unifiableIdeoGroupIndex[level].find(ideograph);
                if(found != _unifiableIdeoGroupIndex[level].end() &&
                    found->second < _unifiableIdeoGroups[level].size()) {
                    for(const Ideograph& related: _unifiableIdeoGroups[level][found->second])
                        if(!(related == ideograph))
                            append(result, IDSOwner(new Ideograph(related)), IWDSRuleForLevel(level));
                }

                const auto expressionFound = _unifiableIDSGroupIndex[level].find(ideograph.toString());
                if(expressionFound == _unifiableIDSGroupIndex[level].end() ||
                    expressionFound->second >= _unifiableIDSGroups[level].size())
                    continue;
                for(const std::string& expression: _unifiableIDSGroups[level][expressionFound->second]) {
                    IDSOwner parsed = ParseIDSOwned(expression);
                    if(parsed != nullptr && !ContainsQueryOnlyOperator(parsed.get()) && !IDSequal(parsed.get(), value))
                        append(result, std::move(parsed), IWDSRuleForLevel(level));
                }
            }
            return result;
        }

        if(IsSearchExpression(value)) {
            SearchExpression* search = AsSearchExpression(value);
            // <search=...> 的每个条件都要保留“同时满足”的关系。
            // 因此一个条件的多个 IWDS 等同式应收拢为嵌套 <any=...>，
            // 而不是把整个 search 展开成多个顶层查询。
            auto makeAnyOrSingle = [&](const IDSOwnerList& choices) -> IDSOwner {
                if(choices.empty()) return IDSOwner();
                if(choices.size() == 1) return choices.front()->Clone();

                IDSOwnerList alternatives;
                alternatives.reserve(choices.size());
                for(const auto& choice: choices)
                    alternatives.push_back(choice->Clone());
                return IDSOwner(new SearchExpression(std::move(alternatives), SEARCH_EXPRESSION_ANY));
            };
            auto expandSearchTerms = [&](const IDSOwnerList& source, bool groupChoices) {
                IDSOwnerList expandedTerms;
                for(const auto& term: source) {
                    const IDSOwnerList choices = expandNode(term.get());
                    if(choices.empty()) return IDSOwnerList();

                    if(groupChoices) {
                        IDSOwner grouped = makeAnyOrSingle(choices);
                        if(grouped == nullptr) return IDSOwnerList();
                        expandedTerms.push_back(std::move(grouped));
                    } else {
                        for(const auto& choice: choices)
                            expandedTerms.push_back(choice->Clone());
                    }
                }
                return expandedTerms;
            };

            // <any=...> 和 <except=...> 的直接项本身就是“或”关系；
            // <search=...> 的直接项则分别变成一个等同候选组。
            const bool   groupTerms    = search->GetMode() != SEARCH_EXPRESSION_ANY;
            IDSOwnerList expandedTerms = expandSearchTerms(search->GetTerms(), groupTerms);
            if(expandedTerms.empty() && !search->GetTerms().empty()) return result;
            IDSOwnerList expandedExceptTerms = expandSearchTerms(search->GetExceptTerms(), true);
            if(expandedExceptTerms.empty() && !search->GetExceptTerms().empty()) return result;

            IDSOwner expandedSearch = IDSOwner(
                new SearchExpression(std::move(expandedTerms), search->GetMode(), std::move(expandedExceptTerms)));
            const std::string expandedSearchKey = expandedSearch->toString();
            MergeQueryPreprocessRulesTree(expandedSearchKey, expandedSearch.get());
            append(result, std::move(expandedSearch));
            return result;
        }

        if(!IsPattern(value) || IsReplaceQuery(value) || IsSubtractQuery(value)) return result;

        Pattern*          pattern  = AsPattern(value);
        const std::string exactKey = pattern->toString();
        IDSOwnerList       patternCandidates;
        patternCandidates.push_back(value->Clone());
        if(allowIWDS) {
            // 先收集结构等效式，再对每个结构候选展开子部件。
            // 这样 ⿱⬚𠧒 -> ⿺⬚𠧒 和 𠧒 -> 乞 可以组合为 ⿺⬚乞。
            appendPatternGroupAlternatives(value, patternCandidates);
            for(size_t level = IWDS_UNIFICATION_LV1;
                level <= static_cast<size_t>(config.fuzzyMatch.unificationLevel) &&
                level < IWDS_UNIFICATION_LEVEL_COUNT;
                level++) {
                const auto found = _unifiableIDSGroupIndex[level].find(exactKey);
                if(found == _unifiableIDSGroupIndex[level].end() || found->second >= _unifiableIDSGroups[level].size())
                    continue;
                for(const std::string& expression: _unifiableIDSGroups[level][found->second]) {
                    IDSOwner parsed = ParseIDSOwned(expression);
                    if(parsed != nullptr && !ContainsQueryOnlyOperator(parsed.get()) && !IDSequal(parsed.get(), value))
                        append(patternCandidates, std::move(parsed), IWDSRuleForLevel(level));
                }
            }
        }

        for(const auto& candidate: patternCandidates) {
            if(result.size() >= maximum || !IsPattern(candidate.get())) break;
            Pattern*                 candidatePattern = AsPattern(candidate.get());
            const std::vector<IDSOwnerList> childChoices = expandList(candidatePattern->GetpIDSRef());
            for(const IDSOwnerList& children: childChoices) {
                if(result.size() >= maximum) break;
                std::vector<IDS*> childPointers;
                for(const auto& child: children)
                    childPointers.push_back(child.get());

                int overlayRange[2] = {0, 0};
                candidatePattern->GetOverlayRange(overlayRange);
                IDSOwner expandedPattern = IDSOwner(new Pattern(candidatePattern->GetIDC(), childPointers,
                    candidatePattern->GetPreferSplitPoint(), overlayRange, candidatePattern->GetOverlayType(),
                    candidatePattern->GetOptionalInt()));
                const std::string expandedPatternKey = expandedPattern->toString();
                MergeQueryPreprocessRules(expandedPatternKey, candidate->toString());
                for(const auto& child: children)
                    MergeQueryPreprocessRules(expandedPatternKey, child->toString());
                append(result, std::move(expandedPattern));
            }
        }
        return result;
    };

    return expandNode(ids);
}

IDSOwnerList IDSdatabase::BuildEquivalentQueryOwners(IDS* ids, size_t maximum) {
    IDSOwnerList out;
    _queryPreprocessRules.clear();
    if(ids == nullptr || maximum == 0) return out;

    // replace/subtract 必须先改写成普通 IDS 查询；否则它们会在每个
    // 数据库候选字形的 MatchSearchTerms 中重复展开。
    IDSOwner queryOnlyPreprocessed;
    if(ContainsReplaceSubtractOperator(ids)) {
        queryOnlyPreprocessed = PreprocessReplaceSubtractQuery(ids, maximum);
        if(queryOnlyPreprocessed == nullptr) return out;
        ids = queryOnlyPreprocessed.get();
    }

    auto appendHVAlternative = [&](IDS* source, const IDSOwner& alternative) {
        if(source == nullptr || IDSequal(source, alternative.get())) return;
        const std::string alternativeText = alternative->toString();
        RecordQueryPreprocessRule(alternativeText, IDS_PREPROCESS_HV_EXTRACT);
        // HV 归一化只是后续步骤，必须继承此前已经记录的
        // CJK 回退、同 IDS、IWDS 或义白等预处理原因。
        MergeQueryPreprocessRules(alternativeText, source->toString());
    };

    std::function<IDSOwnerList(IDS*)>                             expandYibaiNode;
    std::function<std::vector<IDSOwnerList>(const IDSOwnerList&)> expandYibaiList;

    // yibai 用“⿳ABC”表示“⿴(⿱AC)B”。只有当“⿱AC”确实
    // 是数据库中的字形部件时才展开，避免把所有三层结构都误当成包围结构。
    std::function<bool(IDS*, IDS*)> sameYibaiShape = [&](IDS* left, IDS* right) {
        if(left == nullptr || right == nullptr || left->GetType() != right->GetType()) return false;
        if(IsIdeograph(left)) return AsIdeograph(left)->GetPured() == AsIdeograph(right)->GetPured();
        if(!IsPattern(left)) return IDSequal(left, right);

        Pattern* leftPattern  = AsPattern(left);
        Pattern* rightPattern = AsPattern(right);
        if(rightPattern == nullptr || leftPattern->GetIDC() != rightPattern->GetIDC() ||
            PatternChildCount(leftPattern) != PatternChildCount(rightPattern))
            return false;
        for(size_t index = 0; index < PatternChildCount(leftPattern); index++)
            if(!sameYibaiShape(PatternChild(leftPattern, index), PatternChild(rightPattern, index))) return false;
        return true;
    };

    auto findYibaiWrappers = [&](IDS* top, IDS* bottom) {
        std::vector<Ideograph> wrappers;
        for(const auto& entry: _rawIDSDB) {
            Ideograph wrapper = entry.first;
            if(wrapper.inPUA()) continue;
            for(const auto& definition: entry.second) {
                if(!IsPattern(definition.get())) continue;
                Pattern* pattern = AsPattern(definition.get());
                if(pattern->GetIDC() != IDC_ABOVE_BELOW || PatternChildCount(pattern) != 2) continue;
                if(sameYibaiShape(PatternChild(pattern, 0), top) && sameYibaiShape(PatternChild(pattern, 1), bottom)) {
                    wrappers.push_back(wrapper);
                    break;
                }
            }
        }
        return wrappers;
    };

    auto appendYibai = [&](IDSOwnerList& target, IDSOwner value) {
        bool truncated = false;
        AppendUniqueOwned(target, std::move(value), maximum, truncated);
    };

    expandYibaiList = [&](const IDSOwnerList& source) {
        std::vector<IDSOwnerList> result(1);
        for(const auto& child: source) {
            const IDSOwnerList choices = expandYibaiNode(child.get());
            if(choices.empty()) return std::vector<IDSOwnerList>();

            std::vector<IDSOwnerList> next;
            for(const auto& prefix: result) {
                for(const auto& choice: choices) {
                    if(next.size() >= maximum) break;
                    IDSOwnerList combined;
                    for(const auto& item: prefix)
                        combined.push_back(item->Clone());
                    combined.push_back(choice->Clone());
                    next.push_back(std::move(combined));
                }
                if(next.size() >= maximum) break;
            }
            result = std::move(next);
            if(result.empty()) break;
        }
        return result;
    };

    expandYibaiNode = [&](IDS* value) {
        IDSOwnerList result;
        if(value == nullptr) return result;
        appendYibai(result, value->Clone());

        // 🔄、subtract 等查询操作符必须交给原有的专用预处理，
        // yibai 兼容层只处理普通 IDS 树。
        if(IsReplaceQuery(value) || IsSubtractQuery(value)) return result;

        if(IsSearchExpression(value)) {
            SearchExpression* search          = AsSearchExpression(value);
            auto              makeAnyOrSingle = [&](const IDSOwnerList& choices) -> IDSOwner {
                if(choices.empty()) return IDSOwner();
                if(choices.size() == 1) return choices.front()->Clone();

                IDSOwnerList alternatives;
                for(const auto& choice: choices)
                    alternatives.push_back(choice->Clone());
                return IDSOwner(new SearchExpression(std::move(alternatives), SEARCH_EXPRESSION_ANY));
            };
            auto expandSearchTerms = [&](const IDSOwnerList& source, bool groupChoices) {
                IDSOwnerList expandedTerms;
                for(const auto& term: source) {
                    const IDSOwnerList choices = expandYibaiNode(term.get());
                    if(choices.empty()) return IDSOwnerList();
                    if(groupChoices) {
                        IDSOwner grouped = makeAnyOrSingle(choices);
                        if(grouped == nullptr) return IDSOwnerList();
                        expandedTerms.push_back(std::move(grouped));
                    } else {
                        for(const auto& choice: choices)
                            expandedTerms.push_back(choice->Clone());
                    }
                }
                return expandedTerms;
            };

            const bool   groupTerms    = search->GetMode() != SEARCH_EXPRESSION_ANY;
            IDSOwnerList expandedTerms = expandSearchTerms(search->GetTerms(), groupTerms);
            if(expandedTerms.empty() && !search->GetTerms().empty()) return result;
            IDSOwnerList expandedExceptTerms = expandSearchTerms(search->GetExceptTerms(), true);
            if(expandedExceptTerms.empty() && !search->GetExceptTerms().empty()) return result;
            appendYibai(result,
                IDSOwner(
                    new SearchExpression(std::move(expandedTerms), search->GetMode(), std::move(expandedExceptTerms))));
            return result;
        }

        if(!IsPattern(value)) return result;

        Pattern*                        pattern      = AsPattern(value);
        const std::vector<IDSOwnerList> childChoices = expandYibaiList(pattern->GetpIDSRef());
        for(const IDSOwnerList& children: childChoices) {
            std::vector<IDS*> childPointers;
            for(const auto& child: children)
                childPointers.push_back(child.get());

            int overlayRange[2] = {0, 0};
            pattern->GetOverlayRange(overlayRange);
            appendYibai(result,
                IDSOwner(new Pattern(pattern->GetIDC(), childPointers, pattern->GetPreferSplitPoint(), overlayRange,
                    pattern->GetOverlayType(), pattern->GetOptionalInt())));

            if(pattern->GetIDC() == IDC_ABOVE_MIDDLE_BELOW && childPointers.size() == 3) {
                const std::vector<Ideograph> wrappers = findYibaiWrappers(childPointers[0], childPointers[2]);
                for(const Ideograph& wrapper: wrappers) {
                    Ideograph         wrapperCopy = wrapper;
                    std::vector<IDS*> compatibleChildren;
                    compatibleChildren.push_back(&wrapperCopy);
                    compatibleChildren.push_back(childPointers[1]);
                    appendYibai(result, IDSOwner(new Pattern(IDC_SURROUND_FULL, compatibleChildren)));
                }
            }
            if(result.size() >= maximum) break;
        }
        return result;
    };

    IDSOwnerList hvAlternatives;
    // 同 IDS 变体必须在 HV 展开前进入候选查询，否则复合部件的
    // 字形身份可能已经被 HV 展开丢失。
    IDSOwnerList sameIDSAlternatives = ExpandSameIDSQuery(ids, maximum);
    if(sameIDSAlternatives.empty()) sameIDSAlternatives.push_back(ids->Clone());
    for(const auto& alternative: sameIDSAlternatives)
        if(!IDSequal(alternative.get(), ids))
            RecordQueryPreprocessRule(alternative->toString(), IDS_PREPROCESS_SAME_IDS);
    if(_format == IDSDB_YIBAI) {
        // ⿳ 会被 HVExtractOwned 归一化为 ▤，所以 yibai 兼容式必须在
        // HV 展开之前生成；否则将丢失“⿳ABC”这一语义信息。
        IDSOwnerList yibaiPreprocessed = expandYibaiNode(ids);
        if(yibaiPreprocessed.empty()) yibaiPreprocessed.push_back(ids->Clone());
        for(const auto& query: yibaiPreprocessed) {
            if(!IDSequal(query.get(), ids))
                RecordQueryPreprocessRule(query->toString(), IDS_PREPROCESS_YIBAI_COMPATIBILITY);
            IDSOwnerList hv = HVExtractOwned(query.get(), true, true, maximum);
            if(hv.empty()) hv.push_back(query->Clone());
            for(const auto& alternative: hv) {
                appendHVAlternative(query.get(), alternative);
                bool truncated = false;
                AppendUniqueOwned(hvAlternatives, alternative->Clone(), maximum, truncated);
            }
        }
    } else {
        hvAlternatives = HVExtractOwned(ids, true, true, maximum);
        for(const auto& alternative: hvAlternatives)
            appendHVAlternative(ids, alternative);
    }

    for(const auto& sameIDSQuery: sameIDSAlternatives) {
        if(IDSequal(sameIDSQuery.get(), ids)) continue;

        if(_format == IDSDB_YIBAI) {
            IDSOwnerList yibaiPreprocessed = expandYibaiNode(sameIDSQuery.get());
            if(yibaiPreprocessed.empty()) yibaiPreprocessed.push_back(sameIDSQuery->Clone());
            for(const auto& query: yibaiPreprocessed) {
                if(!IDSequal(query.get(), sameIDSQuery.get()))
                    RecordQueryPreprocessRule(query->toString(), IDS_PREPROCESS_YIBAI_COMPATIBILITY);
                IDSOwnerList hv = HVExtractOwned(query.get(), true, true, maximum);
                if(hv.empty()) hv.push_back(query->Clone());
                for(const auto& alternative: hv) {
                    appendHVAlternative(query.get(), alternative);
                    bool truncated = false;
                    AppendUniqueOwned(hvAlternatives, alternative->Clone(), maximum, truncated);
                }
            }
        } else {
            IDSOwnerList hv = HVExtractOwned(sameIDSQuery.get(), true, true, maximum);
            if(hv.empty()) hv.push_back(sameIDSQuery->Clone());
            for(const auto& alternative: hv) {
                appendHVAlternative(sameIDSQuery.get(), alternative);
                bool truncated = false;
                AppendUniqueOwned(hvAlternatives, alternative->Clone(), maximum, truncated);
            }
        }
        if(hvAlternatives.size() >= maximum) break;
    }
    if(hvAlternatives.empty()) hvAlternatives.push_back(ids->Clone());
    // 开启任一 IWDS 等级时保留原始查询；其余等同式仍由预处理候选承担。
    if(config.fuzzyMatch.unificationLevel >= IWDS_UNIFICATION_SOURCE_CODE_SEPARATION &&
        config.fuzzyMatch.unificationLevel < IWDS_UNIFICATION_LEVEL_COUNT) {
        bool hasRawQuery = false;
        for(const auto& alternative: hvAlternatives)
            if(IDSequal(alternative.get(), ids)) {
                hasRawQuery = true;
                break;
            }
        if(!hasRawQuery) hvAlternatives.push_back(ids->Clone());
    }

    for(const auto& alternative: hvAlternatives) {
        IDSOwnerList yibaiAlternatives;
        if(_format == IDSDB_YIBAI)
            yibaiAlternatives = expandYibaiNode(alternative.get());
        else
            yibaiAlternatives.push_back(alternative->Clone());

        for(const auto& yibaiAlternative: yibaiAlternatives) {
            if(!IDSequal(yibaiAlternative.get(), alternative.get()))
                RecordQueryPreprocessRule(yibaiAlternative->toString(), IDS_PREPROCESS_YIBAI_COMPATIBILITY);
            const IDSOwnerList expanded = ExpandIWDSQuery(yibaiAlternative.get(), maximum);
            for(const auto& query: expanded) {
                // IWDS 分组中的成员保存的是原始 IDS。数据库查询缓存则保存
                // HVExtractOwned 后的表达式，因此新生成的等同式也必须先
                // 做同样的 HV 归一化，不能依赖已停用的原始 IDS 回退扫描。
                IDSOwnerList normalized = HVExtractOwned(query.get(), true, true, maximum);
                if(normalized.empty()) normalized.push_back(query->Clone());
                for(const auto& normalizedQuery: normalized) {
                    appendHVAlternative(query.get(), normalizedQuery);
                    bool truncated = false;
                    AppendUniqueOwned(out, normalizedQuery->Clone(), maximum, truncated);
                    if(out.size() >= maximum) return out;
                }
                if(out.size() >= maximum) return out;
            }
        }
    }

    // 合并或嵌套 search 后，原因可能只存在于 any/IDS 子节点；
    // 在返回前递归汇总到最终候选表达式。
    for(const auto& query: out)
        MergeQueryPreprocessRulesTree(query->toString(), query.get());

    // 排列结构的 split point 不参与当前匹配；例如 ▥(AB) 与 ▥(A|B)
    // 的搜索行为相同。只在等价查询展示/执行层去重，不改变原始 IDS 的严格相等。
    IDSOwnerList                            semanticUnique;
    std::unordered_map<std::string, size_t> semanticIndexes;
    for(auto& query: out) {
        const std::string key   = EquivalentQuerySemanticKey(query.get());
        const auto        found = semanticIndexes.find(key);
        if(found == semanticIndexes.end()) {
            semanticIndexes.emplace(key, semanticUnique.size());
            semanticUnique.push_back(std::move(query));
        } else {
            MergeQueryPreprocessRules(semanticUnique[found->second]->toString(), query->toString());
        }
    }
    out = std::move(semanticUnique);

    // search 的 HV/IWDS 候选来自同一个原始条件时，可以合并成一个
    // 查询树；这样数据库只需遍历一次，并且等同查询更容易理解。
    if(out.size() > 1 && IsSearchExpression(ids)) {
        SearchExpression* first     = AsSearchExpression(out.front().get());
        bool              mergeable = first != nullptr;
        for(const auto& query: out) {
            if(!IsSearchExpression(query.get())) {
                mergeable = false;
                break;
            }
            SearchExpression* expression = AsSearchExpression(query.get());
            if(expression->GetMode() != first->GetMode()) {
                mergeable = false;
                break;
            }
            // 不合并带排除条件的 search，避免不同候选的“条件/排除”
            // 配对关系被错误地拆开。
            if(first->GetMode() != SEARCH_EXPRESSION_EXCEPT &&
                expression->GetExceptTerms().size() != first->GetExceptTerms().size()) {
                mergeable = false;
                break;
            }
            if(first->GetMode() != SEARCH_EXPRESSION_EXCEPT && !first->GetExceptTerms().empty()) {
                mergeable = false;
                break;
            }
        }

        if(mergeable) {
            auto appendUniqueSearchChoice = [&](IDSOwnerList& choices, IDS* choice) {
                if(IsSearchExpression(choice) && AsSearchExpression(choice)->GetMode() == SEARCH_EXPRESSION_ANY &&
                    AsSearchExpression(choice)->GetExceptTerms().empty()) {
                    for(const auto& nested: AsSearchExpression(choice)->GetTerms()) {
                        bool truncated = false;
                        AppendUniqueOwned(choices, nested->Clone(), maximum, truncated);
                    }
                } else {
                    bool truncated = false;
                    AppendUniqueOwned(choices, choice->Clone(), maximum, truncated);
                }
            };
            auto makeChoice = [&](IDSOwnerList choices) -> IDSOwner {
                if(choices.empty()) return IDSOwner();
                if(choices.size() == 1) return choices.front()->Clone();
                return IDSOwner(new SearchExpression(std::move(choices), SEARCH_EXPRESSION_ANY));
            };

            IDSOwnerList mergedTerms;
            if(first->GetMode() == SEARCH_EXPRESSION_ALL) {
                const size_t termCount = first->GetTerms().size();
                mergeable              = termCount != 0;
                for(const auto& query: out)
                    mergeable = mergeable && AsSearchExpression(query.get())->GetTerms().size() == termCount;
                if(mergeable) {
                    for(size_t index = 0; index < termCount; index++) {
                        IDSOwnerList choices;
                        for(const auto& query: out)
                            appendUniqueSearchChoice(choices, AsSearchExpression(query.get())->GetTerms()[index].get());
                        IDSOwner selected = makeChoice(std::move(choices));
                        if(selected == nullptr) {
                            mergeable = false;
                            break;
                        }
                        mergedTerms.push_back(std::move(selected));
                    }
                }
            } else {
                for(const auto& query: out) {
                    for(const auto& term: AsSearchExpression(query.get())->GetTerms())
                        appendUniqueSearchChoice(mergedTerms, term.get());
                }
            }

            if(mergeable) {
                const SearchExpressionMode mergedMode = first->GetMode();
                out.clear();
                out.push_back(IDSOwner(new SearchExpression(std::move(mergedTerms), mergedMode)));
            }
        }
    }
    return out;
}

std::vector<IDSMatchDetail> IDSdatabase::MatchDetailed(IDS* ids, const IDSqueryOptions& options) {
    std::vector<IDSMatchDetail> details;
    if(ids == nullptr) return details;

    const std::vector<Ideograph>            matches           = MatchQuery(ids, options);
    const std::vector<std::string>          equivalentQueries = GetEquivalentQueries(ids);
    std::unordered_map<std::string, size_t> equivalentIndexes;
    for(size_t index = 0; index < equivalentQueries.size(); index++)
        equivalentIndexes.emplace(equivalentQueries[index], index);

    ScopedBoolean overlayFilter(_ignoreOverlayStructureForMatch, options.filter.ignoreOverlayStructure);
    details.reserve(matches.size());
    for(const Ideograph& glyph: matches) {
        IDSMatchDetail detail;
        if(BuildMatchDetailForGlyph(glyph, ids, options, equivalentIndexes, detail))
            details.push_back(std::move(detail));
    }
    return details;
}
std::vector<std::string> IDSdatabase::GetEquivalentQueries(IDS* ids) {
    std::vector<std::string> result;
    const IDSOwnerList       queries = BuildEquivalentQueryOwners(ids);
    for(const auto& query: queries) {
        const std::string text = query->toString();
        if(std::find(result.begin(), result.end(), text) == result.end()) result.push_back(text);
    }
    return result;
}

static bool MatchPathResourceOverlaps(const std::string& left, const std::string& right) {
    if(left == right) return true;
    const std::string* shorter = &left;
    const std::string* longer  = &right;
    if(shorter->size() > longer->size()) std::swap(shorter, longer);
    return longer->compare(0, shorter->size(), *shorter) == 0 && longer->size() > shorter->size() &&
        (*longer)[shorter->size()] == '/';
}

static std::vector<std::string> MatchPathResources(const std::string& path) {
    std::vector<std::string> resources;
    auto appendRangeResources = [&resources](
                                    const std::string& prefix, const std::string& marker, const std::string& range) {
        const size_t colon = range.find(':');
        try {
            if(colon == std::string::npos) {
                resources.push_back(prefix + marker + range + "]");
                return;
            }
            const size_t begin = static_cast<size_t>(std::stoul(range.substr(0, colon)));
            const size_t end   = static_cast<size_t>(std::stoul(range.substr(colon + 1)));
            for(size_t index = begin; index < end; index++)
                resources.push_back(prefix + marker + std::to_string(index) + "]");
        } catch(const std::exception&) {
            resources.push_back(prefix + marker + range + "]");
        }
    };
    auto findRangeOpen = [](const std::string& value, size_t limit) {
        const std::array<std::string, 3> markers      = {"/child[", "/ids[", "/["};
        size_t                           bestPosition = std::string::npos;
        size_t                           bestLength   = 0;
        for(const std::string& marker: markers) {
            const size_t position = value.rfind(marker, limit);
            if(position != std::string::npos && (bestPosition == std::string::npos || position > bestPosition)) {
                bestPosition = position;
                bestLength   = marker.size();
            }
        }
        return std::make_pair(bestPosition, bestLength);
    };

    size_t clausePos = 0;
    while(clausePos <= path.size()) {
        const size_t      clauseEnd = path.find("; ", clausePos);
        const std::string clause =
            path.substr(clausePos, clauseEnd == std::string::npos ? std::string::npos : clauseEnd - clausePos);
        if(clause.compare(0, 4, "hv{") != 0 && clause.compare(0, 7, "residue") != 0) {
            const size_t rootPos = clause.find("root");
            if(rootPos != std::string::npos) {
                const std::string value     = clause.substr(rootPos);
                const size_t      arrow     = value.find(">");
                const auto        left      = findRangeOpen(value, arrow == std::string::npos ? value.size() : arrow);
                const size_t      rightOpen = arrow == std::string::npos ? std::string::npos : value.find('[', arrow);
                const size_t      rightClose =
                    rightOpen == std::string::npos ? std::string::npos : value.find(']', rightOpen);
                if(arrow != std::string::npos && left.first != std::string::npos &&
                    value.find(']', left.first) != std::string::npos && rightClose != std::string::npos) {
                    const size_t      leftClose = value.find(']', left.first);
                    const std::string prefix    = value.substr(0, left.first);
                    appendRangeResources(prefix, value.substr(left.first, left.second),
                        value.substr(left.first + left.second, leftClose - left.first - left.second));
                    appendRangeResources(prefix, "/[", value.substr(rightOpen + 1, rightClose - rightOpen - 1));
                } else {
                    const auto range = findRangeOpen(value, value.size());
                    if(range.first != std::string::npos && !value.empty() && value.back() == ']')
                        appendRangeResources(value.substr(0, range.first), value.substr(range.first, range.second),
                            value.substr(range.first + range.second, value.size() - range.first - range.second - 1));
                    else
                        resources.push_back(value);
                }
            }
        }
        if(clauseEnd == std::string::npos) break;
        clausePos = clauseEnd + 2;
    }
    return resources;
}
static std::string SimplifyMatchPath(const std::string& path) {
    std::string simplified;
    size_t      clausePos = 0;
    while(clausePos <= path.size()) {
        const size_t clauseEnd = path.find("; ", clausePos);
        std::string  clause =
            path.substr(clausePos, clauseEnd == std::string::npos ? std::string::npos : clauseEnd - clausePos);
        if(clause == "root" || clause == "." || clause.empty())
            clause = "/";
        else if(clause.compare(0, 5, "root/") == 0)
            clause = "/" + clause.substr(5);
        else if(clause.compare(0, 5, "root.") == 0)
            clause = "/." + clause.substr(5);
        else if(clause.front() != '/')
            clause = "/" + clause;
        if(!simplified.empty()) simplified += "; ";
        simplified += clause;
        if(clauseEnd == std::string::npos) break;
        clausePos = clauseEnd + 2;
    }
    return simplified.empty() ? "/" : simplified;
}
static bool MatchPathAvailable(const std::string& path, const std::vector<std::string>* usedPaths) {
    if(usedPaths == nullptr) return true;
    const std::vector<std::string> resources = MatchPathResources(path);
    for(const std::string& resource: resources)
        for(const std::string& usedPath: *usedPaths)
            if(MatchPathResourceOverlaps(resource, usedPath)) return false;
    return true;
}

static void ReserveMatchPath(const std::string& path, std::vector<std::string>& usedPaths) {
    for(const std::string& resource: MatchPathResources(path))
        if(std::find(usedPaths.begin(), usedPaths.end(), resource) == usedPaths.end()) usedPaths.push_back(resource);
}
static std::string AppendTracePath(
    const std::string& base, const std::string& segment, const std::string& separator = "/") {
    return base + separator + segment;
}

static std::string AppendTraceRange(const std::string& base, size_t begin, size_t end) {
    return base + "/child[" + std::to_string(begin) + ":" + std::to_string(end) + "]";
}

bool IDSdatabase::FindMatchPath(IDS* candidate, IDS* query, const std::string& path, const std::string& queryPath,
    std::vector<IDSMatchPath>& result, const std::vector<std::string>* usedPaths) {
    result.clear();
    if(candidate == nullptr || query == nullptr) return false;

    auto appendNode = [&result, &path, &queryPath]() {
        IDSMatchPath record;
        record.queryPath = queryPath;
        record.path      = path;
        result.push_back(std::move(record));
    };

    if(IsSearchExpression(query)) {
        SearchExpression* search = AsSearchExpression(query);
        if(search->GetMode() == SEARCH_EXPRESSION_EXCEPT) {
            appendNode();
            return true;
        }

        if(search->GetMode() == SEARCH_EXPRESSION_ANY) {
            for(size_t index = 0; index < search->GetTerms().size(); index++) {
                std::vector<IDSMatchPath> termPaths;
                const std::string termQueryPath = AppendTracePath(queryPath, "any[" + std::to_string(index) + "]", ".");
                if(!FindMatchPath(
                       candidate, search->GetTerms()[index].get(), path, termQueryPath, termPaths, usedPaths))
                    continue;
                for(IDSMatchPath& record: termPaths) {
                    record.kind  = IDS_MATCH_PATH_ANY_TERM;
                    record.index = index;
                    result.push_back(std::move(record));
                }
                return true;
            }
            return false;
        }

        std::vector<IDSMatchPath> allPaths;
        std::vector<std::string>  claimedPaths;
        if(usedPaths != nullptr) claimedPaths = *usedPaths;
        for(size_t index = 0; index < search->GetTerms().size(); index++) {
            IDS*              term          = search->GetTerms()[index].get();
            const std::string termQueryPath = AppendTracePath(queryPath, "all[" + std::to_string(index) + "]", ".");
            if(IsResidueCountQuery(term)) {
                IDSMatchPath record;
                record.kind      = IDS_MATCH_PATH_RESIDUE;
                record.index     = index;
                record.queryPath = termQueryPath;
                record.path      = path;
                allPaths.push_back(std::move(record));
                continue;
            }

            std::vector<IDSMatchPath> termPaths;
            if(!FindMatchPath(candidate, term, path, termQueryPath, termPaths, &claimedPaths)) return false;
            for(IDSMatchPath& record: termPaths) {
                record.kind  = IDS_MATCH_PATH_ALL_TERM;
                record.index = index;
                ReserveMatchPath(record.path, claimedPaths);
                allPaths.push_back(std::move(record));
            }
        }
        if(allPaths.empty())
            appendNode();
        else
            result = std::move(allPaths);
        return true;
    }

    // 直接字形的候选可能需要先展开，候选路径保留 ids[i]，查询路径保持在当前节点。
    if(IsIdeograph(candidate) && IsPattern(query)) {
        const Ideograph ideograph = *AsIdeograph(candidate);
        if(_iterStack.find(ideograph) != _iterStack.end()) return false;
        _iterStack.insert(ideograph);
        const std::vector<IDS*> expansions = FindIDS(ideograph);
        for(size_t index = 0; index < expansions.size(); index++) {
            std::vector<IDSMatchPath> expandedPaths;
            if(FindMatchPath(expansions[index], query, AppendTracePath(path, "ids[" + std::to_string(index) + "]", "."),
                   queryPath, expandedPaths, usedPaths)) {
                _iterStack.erase(ideograph);
                result = std::move(expandedPaths);
                return true;
            }
        }
        _iterStack.erase(ideograph);
    }

    // 查询字形在 HV 缓存中展开为连续子节点时，记录查询 IDS 和候选范围的对应关系。
    if(IsPattern(candidate) && IsIdeograph(query)) {
        const Ideograph    ideograph         = *AsIdeograph(query);
        const IDSOwnerList hvAlternatives    = GetIDSOwned(ideograph);
        Pattern*           candidatePattern  = AsPattern(candidate);
        const auto&        candidateChildren = candidatePattern->GetpIDSRef();
        for(size_t alternativeIndex = 0; alternativeIndex < hvAlternatives.size(); alternativeIndex++) {
            IDS* alternative = hvAlternatives[alternativeIndex].get();
            if(!IsPattern(alternative)) continue;
            const auto& queryChildren = AsPattern(alternative)->GetpIDSRef();
            if(queryChildren.empty() || queryChildren.size() > candidateChildren.size()) continue;
            for(size_t begin = 0; begin + queryChildren.size() <= candidateChildren.size(); begin++) {
                bool matchedRange = true;
                for(size_t offset = 0; offset < queryChildren.size(); offset++) {
                    _variableBindings.clear();
                    if(!IDSequal(candidateChildren[begin + offset].get(), queryChildren[offset].get()) &&
                        !IDSmatch(candidateChildren[begin + offset].get(), queryChildren[offset].get())) {
                        matchedRange = false;
                        break;
                    }
                }
                _variableBindings.clear();
                const std::string candidateRange = AppendTraceRange(path, begin, begin + queryChildren.size());
                if(!matchedRange || !MatchPathAvailable(candidateRange, usedPaths)) continue;

                IDSMatchPath record;
                record.queryPath =
                    AppendTraceRange(AppendTracePath(queryPath, "ids[" + std::to_string(alternativeIndex) + "]", "."),
                        0, queryChildren.size());
                record.path = candidateRange;
                result.push_back(std::move(record));
                return true;
            }
        }
    }

    _variableBindings.clear();
    const bool exactMatch      = IDSequal(candidate, query);
    const bool structuralMatch = !exactMatch && IDSmatch(candidate, query);
    if((exactMatch || structuralMatch) && MatchPathAvailable(path, usedPaths)) {
        if(!structuralMatch || !IsPattern(candidate) || !IsPattern(query) ||
            AsPattern(candidate)->GetIDC() != AsPattern(query)->GetIDC() ||
            AsPattern(candidate)->GetpIDSRef().size() != AsPattern(query)->GetpIDSRef().size()) {
            appendNode();
            return true;
        }

        const auto&               candidateChildren = AsPattern(candidate)->GetpIDSRef();
        const auto&               queryChildren     = AsPattern(query)->GetpIDSRef();
        std::vector<IDSMatchPath> childPaths;
        bool                      allChildrenMatched = true;
        for(size_t index = 0; index < queryChildren.size(); index++) {
            std::vector<IDSMatchPath> currentPaths;
            if(!FindMatchPath(candidateChildren[index].get(), queryChildren[index].get(),
                   AppendTracePath(path, "child[" + std::to_string(index) + "]"),
                   AppendTracePath(queryPath, "child[" + std::to_string(index) + "]"), currentPaths, usedPaths)) {
                allChildrenMatched = false;
                break;
            }
            for(IDSMatchPath& record: currentPaths)
                childPaths.push_back(std::move(record));
        }
        if(allChildrenMatched && !childPaths.empty())
            result = std::move(childPaths);
        else
            appendNode();
        return true;
    }

    if(IsIdeograph(candidate)) {
        const Ideograph ideograph = *AsIdeograph(candidate);
        if(_iterStack.find(ideograph) != _iterStack.end()) return false;
        _iterStack.insert(ideograph);
        const std::vector<IDS*> expansions = FindIDS(ideograph);
        for(size_t index = 0; index < expansions.size(); index++) {
            std::vector<IDSMatchPath> expandedPaths;
            if(FindMatchPath(expansions[index], query, AppendTracePath(path, "ids[" + std::to_string(index) + "]", "."),
                   queryPath, expandedPaths, usedPaths)) {
                _iterStack.erase(ideograph);
                result = std::move(expandedPaths);
                return true;
            }
        }
        _iterStack.erase(ideograph);
    } else if(IsPattern(candidate)) {
        const auto& children = AsPattern(candidate)->GetpIDSRef();
        for(size_t index = 0; index < children.size(); index++) {
            std::vector<IDSMatchPath> childPaths;
            if(FindMatchPath(children[index].get(), query,
                   AppendTracePath(path, "child[" + std::to_string(index) + "]"), queryPath, childPaths, usedPaths)) {
                result = std::move(childPaths);
                return true;
            }
        }
    }
    return false;
}
void IDSdatabase::BuildHVMatchPaths(IDS* query, IDS* matched, const std::string& queryPath,
    const std::string& matchedPath, std::vector<IDSMatchPath>& paths) {
    if(query == nullptr || matched == nullptr) return;
    // Detail generation reuses the bindings produced by the successful structural match.
    // Keep any path-only attempts local to this recursion frame.
    VariableBindingScope pathBindings(_variableBindings);

    auto appendRange = [&paths](const std::string& rangeQueryPath, const std::string& rangeMatchedPath,
                           size_t queryBegin, size_t queryEnd, size_t matchedBegin, size_t matchedEnd) {
        if(queryBegin == queryEnd && matchedBegin == matchedEnd) return;
        IDSMatchPath record;
        record.queryPath = AppendTraceRange(rangeQueryPath, queryBegin, queryEnd);
        record.path      = AppendTraceRange(rangeMatchedPath, matchedBegin, matchedEnd);
        for(const IDSMatchPath& existing: paths)
            if(existing.queryPath == record.queryPath && existing.path == record.path) return;
        paths.push_back(std::move(record));
    };
    auto directWidth = [](IDS* value) -> size_t {
        if(IsPattern(value)) return AsPattern(value)->GetpIDSRef().size();
        return 1;
    };

    if(IsIdeograph(query)) {
        const IDSOwnerList alternatives = GetIDSOwned(*AsIdeograph(query));
        for(size_t index = 0; index < alternatives.size(); index++) {
            if(IDSequal(alternatives[index].get(), matched)) {
                appendRange(AppendTracePath(queryPath, "ids[" + std::to_string(index) + "]", "."), matchedPath, 0, 1, 0,
                    directWidth(alternatives[index].get()));
                return;
            }
        }
        return;
    }
    if(!IsPattern(query) || !IsPattern(matched)) return;

    const IDCtype queryArrangement = HVArrangementType(AsPattern(query));
    if(queryArrangement == IDC_UNKNOWN || HVArrangementType(AsPattern(matched)) != queryArrangement) return;

    const auto& queryChildren   = AsPattern(query)->GetpIDSRef();
    const auto& matchedChildren = AsPattern(matched)->GetpIDSRef();
    using SegmentChoices        = std::vector<IDSOwnerList>;
    std::vector<SegmentChoices> choicesByChild;
    choicesByChild.reserve(queryChildren.size());

    auto appendSegment = [](SegmentChoices& choices, IDSOwnerList segment) {
        for(const IDSOwnerList& existing: choices) {
            if(existing.size() != segment.size()) continue;
            bool equal = true;
            for(size_t index = 0; index < existing.size(); index++)
                if(!IDSequal(existing[index].get(), segment[index].get())) {
                    equal = false;
                    break;
                }
            if(equal) return;
        }
        choices.push_back(std::move(segment));
    };

    for(const auto& child: queryChildren) {
        SegmentChoices     choices;
        const IDSOwnerList alternatives = IsIdeograph(child.get()) ? GetIDSOwned(*AsIdeograph(child.get()))
                                                                   : HVExtractOwned(child.get(), true, false, 64);
        for(const auto& alternative: alternatives) {
            IDSOwnerList segment;
            if(IsPattern(alternative.get()) && HVArrangementType(AsPattern(alternative.get())) == queryArrangement)
                AppendClonedOwners(segment, AsPattern(alternative.get())->GetpIDSRef());
            else
                segment.push_back(alternative->Clone());
            appendSegment(choices, std::move(segment));
        }
        if(choices.empty()) {
            IDSOwnerList literal;
            literal.push_back(child->Clone());
            choices.push_back(std::move(literal));
        }
        choicesByChild.push_back(std::move(choices));
    }

    std::vector<size_t>                 selectedChoices(queryChildren.size(), 0);
    std::vector<size_t>                 selectedWidths(queryChildren.size(), 0);
    std::function<bool(size_t, size_t)> align = [&](size_t queryIndex, size_t matchedIndex) {
        if(queryIndex == choicesByChild.size()) return matchedIndex == matchedChildren.size();
        const VariableBindings savedBindings = _variableBindings;

        IDS* queryChild = queryChildren[queryIndex].get();
        if(IsVariable(queryChild)) {
            IDSVariable* variable = AsVariable(queryChild);
            const auto binding = _variableBindings.find(variable->GetName());
            size_t width = 1;
            bool   equal = false;
            if(binding != _variableBindings.end() &&
                binding->second.arrangement == queryArrangement && binding->second.nodes.size() > 1) {
                width = binding->second.nodes.size();
                if(matchedIndex + width <= matchedChildren.size()) {
                    std::vector<IDS*> range;
                    range.reserve(width);
                    for(size_t offset = 0; offset < width; offset++)
                        range.push_back(matchedChildren[matchedIndex + offset].get());
                    equal = VariableBindingEquivalent(binding->second, range, queryArrangement);
                }
            } else if(matchedIndex < matchedChildren.size()) {
                equal = binding != _variableBindings.end()
                    ? VariableBindingEquivalent(binding->second, matchedChildren[matchedIndex].get())
                    : IDSmatch(matchedChildren[matchedIndex].get(), queryChild);
            }
            if(equal) {
                selectedChoices[queryIndex] = 0;
                selectedWidths[queryIndex]  = width;
                if(align(queryIndex + 1, matchedIndex + width)) return true;
            }
            _variableBindings = savedBindings;
            return false;
        }

        for(size_t choiceIndex = 0; choiceIndex < choicesByChild[queryIndex].size(); choiceIndex++) {
            _variableBindings = savedBindings;
            const IDSOwnerList& segment = choicesByChild[queryIndex][choiceIndex];
            if(segment.size() == 1 && IsWildcard(segment.front().get())) {
                for(size_t width = 1; matchedIndex + width <= matchedChildren.size(); width++) {
                    selectedChoices[queryIndex] = choiceIndex;
                    selectedWidths[queryIndex]  = width;
                    if(align(queryIndex + 1, matchedIndex + width)) return true;
                }
                continue;
            }
            if(matchedIndex + segment.size() > matchedChildren.size()) continue;
            bool equal = true;
            for(size_t offset = 0; offset < segment.size(); offset++) {
                if(!IDSequal(segment[offset].get(), matchedChildren[matchedIndex + offset].get()) &&
                    !IDSmatch(matchedChildren[matchedIndex + offset].get(), segment[offset].get())) {
                    equal = false;
                    break;
                }
            }
            if(!equal) continue;
            selectedChoices[queryIndex] = choiceIndex;
            selectedWidths[queryIndex]  = segment.size();
            if(align(queryIndex + 1, matchedIndex + segment.size())) return true;
        }
        _variableBindings = savedBindings;
        return false;
    };

    if(!align(0, 0)) return;
    size_t matchedIndex = 0;
    for(size_t index = 0; index < queryChildren.size(); index++) {
        const IDSOwnerList& segment      = choicesByChild[index][selectedChoices[index]];
        const size_t        segmentWidth = selectedWidths[index] == 0 ? segment.size() : selectedWidths[index];
        appendRange(queryPath, matchedPath, index, index + 1, matchedIndex, matchedIndex + segmentWidth);
        if(segmentWidth == 1 && IsPattern(queryChildren[index].get()) &&
            IsPattern(matchedChildren[matchedIndex].get())) {
            BuildHVMatchPaths(queryChildren[index].get(), matchedChildren[matchedIndex].get(),
                AppendTracePath(queryPath, "child[" + std::to_string(index) + "]"),
                AppendTracePath(matchedPath, "child[" + std::to_string(matchedIndex) + "]"), paths);
        }
        matchedIndex += segmentWidth;
    }
}
bool IDSdatabase::BuildMatchDetailForGlyph(Ideograph glyph, IDS* query, const IDSqueryOptions& options,
    const std::unordered_map<std::string, size_t>& equivalentIndexes, IDSMatchDetail& detail) {
    detail                     = IDSMatchDetail();
    detail.glyph               = glyph;
    const bool trackMatchPaths = options.trackMatchPaths;
    const auto rawFound        = _rawIDSDB.find(glyph);
    const auto cachedFound     = _idsDB.find(glyph);
    if(rawFound != _rawIDSDB.end() && !rawFound->second.empty())
        detail.rawIDS = rawFound->second.front()->GetUniqueSeparator() + rawFound->second.front()->toString();
    else if(cachedFound != _idsDB.end() && !cachedFound->second.empty())
        detail.rawIDS = cachedFound->second.front()->GetUniqueSeparator() + cachedFound->second.front()->toString();

    auto queryExpressionIndex = [&](IDS* expression) {
        const auto found = equivalentIndexes.find(expression->toString());
        return found == equivalentIndexes.end() ? std::numeric_limits<size_t>::max() : found->second;
    };
    auto setRootMatchPath = [&](IDS* expression) {
        if(!trackMatchPaths) return;
        IDSMatchPath record;
        record.queryExpressionIndex = queryExpressionIndex(expression);
        detail.matchPaths.push_back(std::move(record));
    };

    if(IsSearchExpression(query) && AsSearchExpression(query)->GetMode() == SEARCH_EXPRESSION_EXCEPT) {
        detail.matchedIDS = detail.rawIDS;
        detail.matchKind  = IDS_MATCH_KIND_EXCEPT_CONDITION;
        setRootMatchPath(query);
        return true;
    }

    const IDSOwnerList alternatives        = BuildEquivalentQueryOwners(query);
    auto               copyPreprocessRules = [&](const IDSOwner& alternative) {
        detail.preprocessRules.clear();
        if(!trackMatchPaths || !IsSearchExpression(alternative.get()) || detail.matchPaths.empty()) {
            CollectQueryPreprocessRulesTree(alternative.get(), detail.preprocessRules);
            return;
        }

        bool collected = false;
        for(const IDSMatchPath& record: detail.matchPaths) {
            IDS*   selected           = alternative.get();
            bool   valid              = true;
            bool   selectedSearchTerm = false;
            size_t segmentBegin       = 0;
            while(segmentBegin <= record.queryPath.size()) {
                const size_t      segmentEnd = record.queryPath.find_first_of("/.", segmentBegin);
                const std::string segment    = record.queryPath.substr(
                    segmentBegin, segmentEnd == std::string::npos ? std::string::npos : segmentEnd - segmentBegin);
                const bool isSearchSelector = segment.compare(0, 4, "all[") == 0 || segment.compare(0, 4, "any[") == 0;
                if(isSearchSelector) {
                    const size_t close = segment.find(']');
                    if(close == std::string::npos || close <= 4 || !IsSearchExpression(selected)) {
                        valid = false;
                        break;
                    }
                    size_t termIndex = 0;
                    try {
                        termIndex = static_cast<size_t>(std::stoul(segment.substr(4, close - 4)));
                    } catch(const std::exception&) {
                        valid = false;
                        break;
                    }
                    SearchExpression* search = AsSearchExpression(selected);
                    if(termIndex >= search->GetTerms().size()) {
                        valid = false;
                        break;
                    }
                    selected           = search->GetTerms()[termIndex].get();
                    selectedSearchTerm = true;
                }
                if(segmentEnd == std::string::npos) break;
                segmentBegin = segmentEnd + 1;
            }
            if(valid && selectedSearchTerm) {
                CollectQueryPreprocessRulesTree(selected, detail.preprocessRules);
                collected = true;
            }
        }
        if(!collected) CollectQueryPreprocessRulesTree(alternative.get(), detail.preprocessRules);
    };
    auto tryEntries = [&](const IDSOwnerList& entries, IDSMatchSource source, const IDSOwner& alternative) {
        for(const auto& entry: entries) {
            _variableBindings.clear();
            if(!IDSmatch(entry.get(), alternative.get())) continue;

            detail.matchedIDS  = entry->GetUniqueSeparator() + entry->toString();
            detail.matchSource = source;
            if(IsSearchExpression(query))
                detail.matchKind = IDS_MATCH_KIND_COMPONENT_SEARCH;
            else if(IDSequal(entry.get(), alternative.get()))
                detail.matchKind = IDS_MATCH_KIND_EXACT_IDS;
            else
                detail.matchKind = IDS_MATCH_KIND_STRUCTURAL_IDS;
            if(trackMatchPaths) {
                std::vector<IDSMatchPath> paths;
                auto                      appendPath = [&](IDSMatchPath path) {
                    path.queryPath = SimplifyMatchPath(path.queryPath);
                    path.path      = SimplifyMatchPath(path.path);
                    for(IDSMatchPath& existing: paths) {
                        if(existing.kind != path.kind || existing.index != path.index ||
                            existing.queryPath != path.queryPath || existing.path != path.path)
                            continue;
                        // 同一位置由原查询和等效式都命中时，优先返回可公开索引的等效式。
                        if(existing.queryExpressionIndex == std::numeric_limits<size_t>::max() &&
                            path.queryExpressionIndex != std::numeric_limits<size_t>::max())
                            existing = std::move(path);
                        return;
                    }
                    paths.push_back(std::move(path));
                };
                auto appendHVPaths = [&](IDS* expression) {
                    std::vector<IDSMatchPath> generated;
                    BuildHVMatchPaths(expression, entry.get(), "root", "root", generated);
                    const size_t expressionIndex = queryExpressionIndex(expression);
                    for(IDSMatchPath& path: generated) {
                        path.queryExpressionIndex = expressionIndex;
                        appendPath(std::move(path));
                    }
                };
                if(source == IDS_MATCH_SOURCE_HV_CACHE) {
                    appendHVPaths(query);
                    if(!IDSequal(query, alternative.get())) appendHVPaths(alternative.get());
                }
                if(paths.empty()) {
                    std::vector<IDSMatchPath> generated;
                    _iterStack.clear();
                    if(!FindMatchPath(entry.get(), alternative.get(), "root", "root", generated))
                        generated.push_back(IDSMatchPath());
                    const size_t expressionIndex = queryExpressionIndex(alternative.get());
                    for(IDSMatchPath& path: generated) {
                        path.queryExpressionIndex = expressionIndex;
                        appendPath(std::move(path));
                    }
                }
                detail.matchPaths = std::move(paths);
            }
            copyPreprocessRules(alternative);
            if(source == IDS_MATCH_SOURCE_STROKE_NEUTRAL_CACHE &&
                std::find(detail.preprocessRules.begin(), detail.preprocessRules.end(),
                    IDS_PREPROCESS_STROKE_NEUTRAL_COMPOSITION) == detail.preprocessRules.end())
                detail.preprocessRules.push_back(IDS_PREPROCESS_STROKE_NEUTRAL_COMPOSITION);
            return true;
        }
        return false;
    };

    for(const auto& alternative: alternatives) {
        if(cachedFound != _idsDB.end() && tryEntries(cachedFound->second, IDS_MATCH_SOURCE_HV_CACHE, alternative))
            return true;
        const auto composedFound = _strokeNeutralCompositionDB.find(glyph);
        if(composedFound != _strokeNeutralCompositionDB.end() &&
            config.fuzzyMatch.strokeNeutralComposition &&
            tryEntries(composedFound->second, IDS_MATCH_SOURCE_STROKE_NEUTRAL_CACHE, alternative))
            return true;
        if(rawFound != _rawIDSDB.end() && tryEntries(rawFound->second, IDS_MATCH_SOURCE_RAW_IDS, alternative))
            return true;
    }

    const std::string originalQuery = query->toString();
    const auto        sameFound     = _sameIDSHashIndex.find(IDSExpressionHash(originalQuery));
    if(sameFound != _sameIDSHashIndex.end()) {
        for(const SameIDSHashGroup& group: sameFound->second) {
            if(config.fuzzyMatch.excludeNonEquivalentSameIDS &&
                group.notEquivalentGlyphs.find(glyph) != group.notEquivalentGlyphs.end())
                continue;
            if(group.expression != originalQuery ||
                std::find(group.glyphs.begin(), group.glyphs.end(), glyph) == group.glyphs.end())
                continue;
            detail.matchedIDS      = originalQuery;
            detail.matchKind       = IDS_MATCH_KIND_SAME_IDS;
            detail.preprocessRules = {IDS_PREPROCESS_SAME_IDS};
            setRootMatchPath(query);
            return true;
        }
    }
    detail.matchedIDS = detail.rawIDS;
    detail.matchKind  = IDS_MATCH_KIND_MATCHED;
    setRootMatchPath(query);
    return true;
}
std::vector<Ideograph> IDSdatabase::MatchQuery(IDS* ids, const IDSqueryOptions& options) {
    std::vector<Ideograph> out;
    if(ids == nullptr) return out;

    // Strict raw IDS duplicates are indexed separately so they are always
    // returned, even when their HV cache took a different expansion path.
    AppendSameIDSMatches(ids, options, out);
    IDSOwnerList alternatives = BuildEquivalentQueryOwners(ids);
    // 严格 locale 筛选必须等所有等效表达式合并后再执行。
    IDSqueryOptions alternativeOptions = options;
    if(options.filter.resultFilter == IDS_RESULT_IGNORE_OTHER_LOCALES_BASE_ONLY ||
        options.filter.resultFilter == IDS_RESULT_IGNORE_OTHER_LOCALES_KEEP_IVS)
        alternativeOptions.filter.resultFilter = IDS_RESULT_ALL;
    for(const auto& alternative: alternatives) {
        const std::vector<Ideograph> matches = Match(alternative.get(), alternativeOptions);
        for(const Ideograph& match: matches)
            if(std::find(out.begin(), out.end(), match) == out.end()) out.push_back(match);
    }
    ApplyResultFilter(out, options);
    return out;
}

std::vector<Ideograph> IDSdatabase::Match(IDS* ids, std::function<void(int)> progressCallback) {
    return Match(ids, IDSqueryOptions(), progressCallback);
}

std::vector<Ideograph> IDSdatabase::Match(
    IDS* ids, const IDSqueryOptions& options, std::function<void(int)> progressCallback) {
    std::vector<Ideograph> out;
    if(ids == nullptr) return out;

    // Match() 也可能被上层直接调用，因此不能只依赖 MatchQuery 的预处理。
    // 预处理后的多个结果会收拢为一个 <any=...>，只进行一次数据库遍历。
    IDSOwner queryOnlyPreprocessed;
    if(ContainsReplaceSubtractOperator(ids)) {
        queryOnlyPreprocessed = PreprocessReplaceSubtractQuery(ids, std::numeric_limits<size_t>::max());
        if(queryOnlyPreprocessed == nullptr) return out;
        ids = queryOnlyPreprocessed.get();
    }

    AppendSameIDSMatches(ids, options, out);
    // Variables are scoped to one top-level query, never to database cache construction.
    _variableBindings.clear();
    _searchTermMemo.clear();
    ScopedBoolean overlayFilter(_ignoreOverlayStructureForMatch, options.filter.ignoreOverlayStructure);
    IDSOwnerList  queries;
    queries.push_back(ids->Clone());

    IdeographSet indexedCandidates;
    const bool hasIndexedCandidates = FindIndexedSearchCandidates(ids, indexedCandidates);

    const size_t queryCount   = queries.size();
    const size_t dbCount      = _idsDB.size();
    int          prevProgress = -1;
    size_t       queryIndex   = 0;
    for(const auto& query: queries) {
        const double      queryProgress = queryCount == 0 ? 0.0 : static_cast<double>(queryIndex) * 100.0 / queryCount;
        SearchExpression* topLevelExcept =
            IsSearchExpression(query.get()) && AsSearchExpression(query.get())->GetMode() == SEARCH_EXPRESSION_EXCEPT
            ? AsSearchExpression(query.get())
            : nullptr;
        size_t dbIndex = 0;
        for(const auto& i: _idsDB) {
            if(progressCallback) {
                const double currentProgressValue =
                    queryProgress + (dbCount == 0 ? 0.0 : static_cast<double>(dbIndex) * 100.0 / dbCount / queryCount);
                const int progressValue = static_cast<int>(currentProgressValue);
                if(progressValue != prevProgress) {
                    progressCallback(progressValue);
                    prevProgress = progressValue;
                }
            }
            if(!ShouldIncludeResult(i.first, options)) {
                if(progressCallback) ++dbIndex;
                continue;
            }
            if(hasIndexedCandidates && indexedCandidates.find(i.first) == indexedCandidates.end()) {
                if(progressCallback) ++dbIndex;
                continue;
            }
            bool       matched                  = false;
            bool       hasEligibleIDS           = false;
            bool       excludedByTopLevelExcept = false;
            const auto rawEntries               = _rawIDSDB.find(i.first);
            const auto composedEntries         = _strokeNeutralCompositionDB.find(i.first);
            auto       evaluateEntries          = [&](const IDSOwnerList& entries, bool matchQuery) {
                for(const auto& j: entries) {
                    _variableBindings.clear();
                    if(options.filter.ignoreOverlayStructure && ContainsOverlay(j.get())) continue;
                    hasEligibleIDS = true;
                    if(topLevelExcept != nullptr) {
                        if(MatchExceptTerms(j.get(), topLevelExcept->GetTerms())) {
                            excludedByTopLevelExcept = true;
                            continue;
                        }
                        continue;
                    }
                    const bool entryMatched = matchQuery && IDSmatch(j.get(), query.get());
                    if(entryMatched) return true;
                }
                return false;
            };

            if(topLevelExcept != nullptr) {
                evaluateEntries(i.second, false);
                if(rawEntries != _rawIDSDB.end()) evaluateEntries(rawEntries->second, false);
                if(config.fuzzyMatch.strokeNeutralComposition && composedEntries != _strokeNeutralCompositionDB.end())
                    evaluateEntries(composedEntries->second, false);
                matched = hasEligibleIDS && !excludedByTopLevelExcept;
            } else {
                matched = evaluateEntries(i.second, true);
                if(!matched && config.fuzzyMatch.strokeNeutralComposition &&
                    composedEntries != _strokeNeutralCompositionDB.end())
                    matched = evaluateEntries(composedEntries->second, true);
            }
            if(!hasEligibleIDS) matched = false;
            if(matched) {
                bool alreadyMatched = false;
                for(const auto& existing: out)
                    if(existing == i.first) {
                        alreadyMatched = true;
                        break;
                    }
                if(!alreadyMatched) out.push_back(i.first);
            }
            if(progressCallback) ++dbIndex;
        }
        if(progressCallback) ++queryIndex;
    }
    ApplyResultFilter(out, options);
    return out;
}

std::vector<Ideograph> IDSdatabase::Search(std::vector<Ideograph> ideographs) {
    IDSOwnerList terms;
    terms.reserve(ideographs.size());
    for(const Ideograph& ideograph: ideographs)
        terms.push_back(IDSOwner(new Ideograph(ideograph)));
    return Search(terms);
}

std::vector<Ideograph> IDSdatabase::Search(const IDSOwnerList& terms) {
    std::vector<Ideograph> out;
    if(terms.empty()) return out;

    _searchTermMemo.clear();
    bool hasResidue = false;
    for(const auto& term: terms) {
        if(IsResidueCountQuery(term.get())) {
            if(hasResidue) return out;
            hasResidue = true;
        }
    }
    for(const auto& entry: _idsDB) {
        _variableBindings.clear();
        const auto composedEntries = _strokeNeutralCompositionDB.find(entry.first);
        bool matched = false;
        if(!hasResidue) {
            _iterStack.clear();
            IDSOwner candidate(new Ideograph(entry.first));
            matched = MatchSearchTerms(candidate.get(), terms);
            if(!matched && config.fuzzyMatch.strokeNeutralComposition &&
                composedEntries != _strokeNeutralCompositionDB.end()) {
                for(const auto& ids: composedEntries->second) {
                    _iterStack.clear();
                    if(MatchSearchTerms(ids.get(), terms)) {
                        matched = true;
                        break;
                    }
                }
            }
        } else {
            for(const auto& ids: entry.second) {
                _iterStack.clear();
                if(MatchSearchTerms(ids.get(), terms)) {
                    matched = true;
                    break;
                }
            }
            if(!matched && config.fuzzyMatch.strokeNeutralComposition &&
                composedEntries != _strokeNeutralCompositionDB.end()) {
                for(const auto& ids: composedEntries->second) {
                    _iterStack.clear();
                    if(MatchSearchTerms(ids.get(), terms)) {
                        matched = true;
                        break;
                    }
                }
            }
        }
        if(matched) out.push_back(entry.first);
    }
    return out;
}

bool IDSdatabase::isEmpty() {
    return _idsDB.empty();
}

std::vector<Ideograph> IDSdatabase::IdeoSearchByCodepoint(Ideograph ideo) {
    std::vector<Ideograph> result;
    for(const auto& entry: _idsDB) {
        if(entry.first.GetPured() == ideo.GetPured()) result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end(), IdeographCmp);
    return result;
}
