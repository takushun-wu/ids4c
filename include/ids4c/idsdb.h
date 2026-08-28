#ifndef _IDSDB_H
#define _IDSDB_H

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utf8.h"

#include "ids4c/ids4c.h"

/**
 * @file idsdb.h
 * @brief IDS 数据库导入、查询、筛选和结果详情接口。
 *
 * API 分层：稳定 API 是生命周期、配置、导入、查询和结果值类型；
 * C++ 扩展 API 是自定义读取器、缓存重建和进度回调；private 成员不属于 API。
 * IDSdatabase 不承诺同一实例上的并发修改或并发查询，上层应使用外部同步。
 */
typedef enum { IDSDB_DEFAULT, IDSDB_YIBAI } IDSdbFormat;

typedef enum { IDS_RESULT_ALL, IDS_RESULT_IGNORE_LC_SUFFIX, IDS_RESULT_IGNORE_OTHER_LOCALES } IDSresultFilter;

// 解析地区后缀回退顺序；'>' 表示下一级，'=' 表示同级，例如 C=G>.=H；"." 表示无后缀。
// 顺序由上层程序提供，不在数据库或库内部写死。
bool ParseLocaleSuffixFallbackOrder(const std::string& value, std::vector<std::string>& order);

// IWDS 汉字统合的严格程度；目前只实现 SourceCodeSeparation。
typedef enum {
    IWDS_UNIFICATION_NONE,
    IWDS_UNIFICATION_SOURCE_CODE_SEPARATION,
    IWDS_UNIFICATION_LV1,
    IWDS_UNIFICATION_LV2,
    IWDS_UNIFICATION_LEVEL_COUNT,
} IWDSUnificationLevel;

const char* IWDSUnificationLevelName(IWDSUnificationLevel level);
bool        ParseIWDSUnificationLevel(const std::string& value, IWDSUnificationLevel& level);

typedef enum {
    IDS_PREPROCESS_NONE,
    IDS_PREPROCESS_CJK_SYMBOL_FALLBACK,
    IDS_PREPROCESS_SAME_IDS,
    IDS_PREPROCESS_HV_EXTRACT,
    IDS_PREPROCESS_YIBAI_COMPATIBILITY,
    IDS_PREPROCESS_IWDS_SOURCE_CODE_SEPARATION,
    IDS_PREPROCESS_IWDS_LV1_COMPONENT,
    IDS_PREPROCESS_IWDS_LV2_COMPONENT,
    // Derived cache that ignores lowercase stroke suffixes when composing components.
    IDS_PREPROCESS_STROKE_NEUTRAL_COMPOSITION,
} IDSPreprocessRule;

const char* IDSPreprocessRuleName(IDSPreprocessRule rule);

typedef enum {
    IDS_MATCH_KIND_EXACT_IDS,
    IDS_MATCH_KIND_STRUCTURAL_IDS,
    IDS_MATCH_KIND_COMPONENT_SEARCH,
    IDS_MATCH_KIND_SAME_IDS,
    IDS_MATCH_KIND_EXCEPT_CONDITION,
    IDS_MATCH_KIND_MATCHED,
} IDSMatchKind;

const char* IDSMatchKindName(IDSMatchKind kind);

typedef enum {
    IDS_MATCH_SOURCE_NONE,
    IDS_MATCH_SOURCE_RAW_IDS,
    IDS_MATCH_SOURCE_HV_CACHE,
    IDS_MATCH_SOURCE_STROKE_NEUTRAL_CACHE,
} IDSMatchSource;

const char* IDSMatchSourceName(IDSMatchSource source);

typedef enum {
    IDS_GLYPH_DOMAIN_ALL,
    IDS_GLYPH_DOMAIN_UNICODE,
    IDS_GLYPH_DOMAIN_PRIVATE,
    IDS_GLYPH_DOMAIN_ABSTRACT,
} IDSglyphDomain;

typedef enum {
    IDS_UNICODE_BLOCK_ALL,
    IDS_UNICODE_BLOCK_CJK,
    IDS_UNICODE_BLOCK_CJK_BASIC,
    IDS_UNICODE_BLOCK_CJK_EXT_A,
    IDS_UNICODE_BLOCK_CJK_EXT_B,
    IDS_UNICODE_BLOCK_CJK_EXT_C,
    IDS_UNICODE_BLOCK_CJK_EXT_D,
    IDS_UNICODE_BLOCK_CJK_EXT_E,
    IDS_UNICODE_BLOCK_CJK_EXT_F,
    IDS_UNICODE_BLOCK_CJK_EXT_G,
    IDS_UNICODE_BLOCK_CJK_EXT_H,
    IDS_UNICODE_BLOCK_CJK_EXT_I,
    IDS_UNICODE_BLOCK_CJK_EXT_J,
    IDS_UNICODE_BLOCK_CJK_COMPATIBILITY,
    IDS_UNICODE_BLOCK_CJK_RADICALS,
    IDS_UNICODE_BLOCK_CJK_STROKES,
    IDS_UNICODE_BLOCK_PRIVATE_BMP,
    IDS_UNICODE_BLOCK_PRIVATE_PLANE15,
    IDS_UNICODE_BLOCK_PRIVATE_PLANE16,
    IDS_UNICODE_BLOCK_ABSTRACT,
    IDS_UNICODE_BLOCK_OTHER,
} IDSunicodeBlock;

const char* IDSglyphDomainName(IDSglyphDomain domain);
bool        ParseIDSglyphDomain(const std::string& value, IDSglyphDomain& domain);
const char* IDSunicodeBlockName(IDSunicodeBlock block);
bool        ParseIDSunicodeBlock(const std::string& value, IDSunicodeBlock& block);
bool        ParseIDSunicodeBlocks(const std::string& value, std::vector<IDSunicodeBlock>& blocks);

struct IDSCodepointRange {
    uint32_t first = 0;
    uint32_t last  = 0;
};

struct IDSCustomGlyphRange {
    // 注册名；查询时通过 IDSFilterOptions::customRanges 选择。
    std::string name;
    // 支持一个名称对应多个 Unicode 区间。
    std::vector<IDSCodepointRange> codepointRanges;
    // 精确字形字符串，可包含变体选择器、后缀或抽象字形格式。
    std::vector<std::string> exactGlyphs;
    // 不含大括号的抽象字形名，例如“u-...”。
    std::vector<std::string> abstractGlyphs;
    // 后缀名单，例如“J”“G”“r”；空字符串表示无后缀。
    std::vector<std::string> suffixes;
};

/// 查询结果筛选条件；字段均为值语义，适合直接映射到绑定层。
struct IDSFilterOptions {
    bool                         ignoreOverlayStructure = false;
    IDSresultFilter              resultFilter           = IDS_RESULT_ALL;
    IDSglyphDomain               glyphDomain            = IDS_GLYPH_DOMAIN_ALL;
    std::vector<IDSunicodeBlock> unicodeBlocks;
    std::vector<std::string>     customRanges;
    // 仅在 IDS_RESULT_IGNORE_OTHER_LOCALES 下生效；未列出的后缀排在已列出的后缀之后。
    // 使用 '>' 表示回退到下一级，使用 '=' 表示同一级，例如 C=G>.=H。
    // 由于历史 API 使用 vector，解析后的同级组以一个字符串保存，如 {"C=G", ".=H"}。
    std::vector<std::string>     localeSuffixFallbackOrder;
};

typedef struct {
    IDSFilterOptions filter;
    // 关闭后仍返回匹配详情，但跳过路径递归和 HV 范围追踪。
    bool trackMatchPaths = true;
} IDSqueryOptions;

using StrokeCountSet = std::unordered_set<uint32_t>;

typedef enum {
    IDS_MATCH_PATH_NODE,
    IDS_MATCH_PATH_ALL_TERM,
    IDS_MATCH_PATH_ANY_TERM,
    IDS_MATCH_PATH_RESIDUE,
} IDSMatchPathKind;

/// 单条匹配分支的结构化路径；不依赖展示用字符串格式。
struct IDSMatchPath {
    IDSMatchPathKind kind = IDS_MATCH_PATH_NODE;
    size_t           index = std::numeric_limits<size_t>::max();
    // equivalent_syntax 中参与匹配的表达式下标；max() 表示原始输入。
    size_t queryExpressionIndex = std::numeric_limits<size_t>::max();
    // 查询表达式中的位置；根节点为 /，结构层使用 /，查询选择器使用 .。
    std::string queryPath = "/";
    // 数据库候选表达式中的位置；路径格式与 queryPath 相同。
    std::string path = "/";
};

const char* IDSMatchPathKindName(IDSMatchPathKind kind);

typedef struct {
    Ideograph                  glyph = Ideograph(0);
    std::string                matchedIDS;
    std::string                rawIDS;
    IDSMatchKind               matchKind   = IDS_MATCH_KIND_MATCHED;
    IDSMatchSource             matchSource = IDS_MATCH_SOURCE_NONE;

    // 每个匹配分支只输出结构化路径，避免同时维护字符串路径和解析结果。
    std::vector<IDSMatchPath>      matchPaths;
    std::vector<IDSPreprocessRule> preprocessRules;
} IDSMatchDetail;
typedef struct {
    uint32_t    vs;
    std::string suffix;
} VSandSuffix;

typedef struct {
    bool hasBaseGlyph = false;
} GlyphLocaleInfo;

/// IWDS 和 YiBai 模糊匹配配置。
struct IDSFuzzyMatchOptions {
    IWDSUnificationLevel unificationLevel = IWDS_UNIFICATION_NONE;
    // Use the derived cache that treats lowercase stroke suffixes as neutral.
    // Raw and HV definitions remain unchanged.
    bool strokeNeutralComposition = true;
    std::string          defaultRegion; // YiBai 部件 fallback 区域。
    // 可选的 FindIDS 地区后缀回退顺序；为空时保持现有 lv0/传统回退行为。
    // '>' 表示回退到下一级，'=' 表示同一级；无后缀可写作 '.'，例如 C=G>.=H。
    // 解析后的同级组以一个字符串保存，例如 {"C=G", ".=H"}。
    // 直接构造 vector 时，空字符串仍表示单独一级的无后缀。
    std::vector<std::string> localeSuffixFallbackOrder;
};

/// 与具体模糊等级无关的数据库选项。
struct IDSMiscOptions {
    bool enableCache      = true;
    bool symFallback      = true;
    bool suffixRisAltForm = false; // YiBai
};

typedef struct {
    IDSFuzzyMatchOptions fuzzyMatch;
    IDSMiscOptions        misc;
} IDSdbConfig;

typedef struct {
    size_t      line              = 0;
    size_t      idsIndex          = 0; // 1-based IDS expression index within the source line.
    size_t      characterIndex    = 0; // 1-based Unicode code-point position within the IDS expression.
    std::string unexpectedCharacter;
    std::string glyph;
    std::string expression;
    std::string message;
} IDSimportIssue;

typedef struct {
    size_t                      inputLines          = 0;
    size_t                      dataLines           = 0;
    size_t                      sourceExpressions   = 0;
    size_t                      acceptedExpressions = 0;
    size_t                      rejectedExpressions = 0;
    size_t                      queryCacheEntries   = 0;
    size_t                      rebuiltCacheGlyphs  = 0;
    size_t                      cacheTruncations    = 0;
    std::vector<IDSimportIssue> issues;

    bool        HasIssues() const { return !issues.empty(); }
    std::string Summary() const;
} IDSimportReport;

// 外部 IDS 读取器输出的统一记录。ids 为空时使用 expression 作为待解析表达式。
// glyphs 允许一个 IDS 同时归属于多个字形，用于兼容 YiBai 的后缀展开。
typedef struct {
    size_t                   line       = 0;
    size_t                   idsIndex   = 0; // 1-based IDS expression index within the source line.
    std::vector<std::string> glyphs;
    std::string              expression; // 原始表达式，用于错误报告。
    std::string              ids;        // 实际交给 IDS 解析器的表达式。
    // IDS.pdf 7.1 的根部唯一化分隔符，例如 {士}。为空表示没有明确区分。
    // 自定义读取器如果在预处理时保留了该标记，可以直接填写此字段。
    std::string              uniqueSeparator;
} IDSImportRecord;

using IDSImportRecordCallback = std::function<bool(const IDSImportRecord& record)>;
using IDSImportReader         = std::function<bool(
    const IDSImportRecordCallback& emit, std::string& error)>;

/**
 * @brief IDS 数据库的主要公共对象。
 *
 * 稳定绑定入口是构造/销毁、配置、导入、字形读取、MatchQuery、MatchDetailed 和
 * GetEquivalentQueries。数据库对象拥有查询缓存；调用者拥有传入的 IDSOwner。
 */
class IDSdatabase {
private:
    using IDSStorage       = std::unordered_map<Ideograph, IDSOwnerList, Ideograph_Hash>;
    // 与每个字形的原始 IDS 按 ordinal 对齐；空字符串表示该 IDS 没有 7.1 唯一化标记。
    using IDSUniqueSeparatorStorage = std::unordered_map<Ideograph, std::vector<std::string>, Ideograph_Hash>;
    using IdeographSet     = std::unordered_set<Ideograph, Ideograph_Hash>;
    struct StrokeCountRange {
        uint32_t minimum = 0;
        uint32_t maximum = std::numeric_limits<uint32_t>::max();
        bool     bounded = false;
    };
    using VariableBindings = std::unordered_map<std::string, std::string>;
    struct SameIDSHashGroup {
        std::string            expression;
        std::vector<Ideograph> glyphs;
        // 仅记录明确写成 {字} 的字形；同一表达式组中的其他字形仍可能互认。
        IdeographSet           notEquivalentGlyphs;
    };
    using SameIDSHashIndex = std::unordered_map<uint64_t, std::vector<SameIDSHashGroup>>;
    // 一个字形可以有多个原始 IDS；集合之间不做传递合并。
    using SameIDSVariantIndex = std::unordered_map<Ideograph, std::vector<Ideograph>, Ideograph_Hash>;

    // 原始 IDS 是可追溯的 lv0 基线，笔画数也只从这里推导。
    IDSStorage _rawIDSDB;
    // 根部 {字} 不是 IDS 树的部件；单独保存，避免与普通同式表达式自动归并。
    IDSUniqueSeparatorStorage _rawIDSUniqueSeparators;
    // 查询缓存保存每条原始 IDS 的一个或多个 HV 展开候选。
    // Derived cache used only by matching and component search, never by FindIDS/HV extraction.
    IDSStorage _strokeNeutralCompositionDB;
    IDSStorage _idsDB;
    // 标记可被增量更新的私有扩展字形，基础 IDS 不可被私有导入覆盖。
    IdeographSet                                              _privateGlyphs;
    // Strict raw-IDS duplicate index. The expression text guards hash collisions.
    SameIDSHashIndex                                          _sameIDSHashIndex;
    SameIDSVariantIndex                                       _sameIDSVariantIndex;
    std::unordered_map<std::string, std::vector<std::string>> _strokeDB;
    std::unordered_map<std::string, std::string>              _strokeRevDB;
    std::unordered_map<std::string, IDSCustomGlyphRange>        _customGlyphRanges;
    // 字形可能有多条 IDS；缓存必须保留每条可解析 IDS 推得的全部笔画数。
    std::unordered_map<Ideograph, StrokeCountSet, Ideograph_Hash> _strokeCountCache;
    std::unordered_set<Ideograph, Ideograph_Hash>                 _pendingStrokeCountCache;
    using UnifiableGroup           = std::vector<Ideograph>;
    using UnifiableGroups          = std::vector<UnifiableGroup>;
    using UnifiableGroupIndex      = std::unordered_map<Ideograph, size_t, Ideograph_Hash>;
    using UnifiableIDSGroup        = std::vector<std::string>;
    using UnifiableIDSGroups       = std::vector<UnifiableIDSGroup>;
    using UnifiableIDSGroupIndex   = std::unordered_map<std::string, size_t>;
    using UnifiableIDSParsedGroup  = std::vector<IDSOwner>;
    using UnifiableIDSParsedGroups = std::vector<UnifiableIDSParsedGroup>;
    using UnifiableIDSShapeIndex   = std::unordered_map<std::string, std::vector<size_t>>;
    using IWDSSubtreeIndex         = std::unordered_map<std::string, IdeographSet>;
    using IWDSSubtreeKeySet        = std::unordered_set<std::string>;
    // SourceCodeSeparation 主要是字形组；components 还可能包含完整 IDS 表达式。
    std::array<UnifiableGroups, IWDS_UNIFICATION_LEVEL_COUNT>          _unifiableIdeoGroups;
    std::array<UnifiableGroupIndex, IWDS_UNIFICATION_LEVEL_COUNT>      _unifiableIdeoGroupIndex;
    std::array<UnifiableIDSGroups, IWDS_UNIFICATION_LEVEL_COUNT>       _unifiableIDSGroups;
    std::array<UnifiableIDSGroupIndex, IWDS_UNIFICATION_LEVEL_COUNT>   _unifiableIDSGroupIndex;
    std::array<UnifiableIDSParsedGroups, IWDS_UNIFICATION_LEVEL_COUNT> _unifiableIDSParsedGroups;
    std::array<UnifiableIDSShapeIndex, IWDS_UNIFICATION_LEVEL_COUNT>   _unifiableIDSShapeIndex;
    // 按 IWDS 等价关系建立的子树倒排索引。索引只用于缩小候选集，最终结果仍由原匹配器确认。
    std::array<IWDSSubtreeIndex, IWDS_UNIFICATION_LEVEL_COUNT>  _iwdsSubtreeIndex;
    std::array<IWDSSubtreeKeySet, IWDS_UNIFICATION_LEVEL_COUNT> _iwdsSubtreeIndexBuiltKeys;
    std::string                                                 _iwdsSubtreeIndexSignature;
    IDSimportReport                                             _lastImportReport;
    std::string                                                 _lastError;

    std::unordered_set<Ideograph, Ideograph_Hash>                           _cache_BasicIdeo;
    std::unordered_map<Ideograph, std::vector<VSandSuffix>, Ideograph_Hash> _cache_IdeoSuffixLookup;
    // Cache whether a code point has a suffixless base glyph for locale filtering.
    std::unordered_map<Ideograph, GlyphLocaleInfo, Ideograph_Hash> _cache_GlyphLocale;

    std::unordered_set<Ideograph, Ideograph_Hash> _iterStack;
    // Query-local bindings for <var=...>. They are cleared for every candidate glyph.
    VariableBindings _variableBindings;
    // Query-local memoization for fixed single-term component searches.
    std::unordered_map<std::string, bool> _searchTermMemo;
    // 等价查询预处理的临时溯源；按表达式文本索引，不写入 SQLite。
    std::unordered_map<std::string, std::vector<IDSPreprocessRule>> _queryPreprocessRules;
    // 无统合时用于快速判断“候选树的最大笔画数不足以满足查询下界”。
    std::unordered_map<std::string, uint32_t>        _strokeMaximumCache;
    std::unordered_map<std::string, StrokeCountRange> _strokeRangeCache;
    // Active only while Match() evaluates a query with overlay filtering enabled.
    bool        _ignoreOverlayStructureForMatch = false;
    IDSdbFormat _format                         = IDSDB_DEFAULT;
    std::string _idsDataSignature;

    bool LoadSqliteDatabase(const std::string& filename);
    bool SaveSqliteDatabase(const std::string& filename);
    bool SaveStrokeCountCache(const std::string& filename);
    bool LoadUnifiableSqlite(const std::string& filename);
    bool SaveUnifiableSqlite(const std::string& filename);
    void BuildUnificationGroupIndexes();
    void BuildUnificationIDSGroupIndexes();

    void           MakeStrokeDB();
    void           MakeStrokeRevDB();
    void           MakeStrokeCountCache();
    StrokeCountSet CalculateStrokeCounts(IDS* ids, std::unordered_set<Ideograph, Ideograph_Hash>& recursionStack);
    StrokeCountSet   GetStrokeCounts(IDS* ids);
    StrokeCountRange GetStrokeRange(IDS* ids);
    bool             HasDisjointStrokeRange(IDS* candidate, IDS* query);
    bool             MatchStrokeCount(IDS* ids, SearchParam* query);
    bool           AddRawIDS(Ideograph ideo, IDSOwner ids, std::string uniqueSeparator = "");
    void           BuildSameIDSHashIndex();
    IDSOwnerList ExpandSameIDSQuery(IDS* ids, size_t maximum = 64);
    void           ResetRuntimeCaches();
    bool           IsHVAmbiguousOrigin(Ideograph glyph) const;
    bool           BuildQueryCacheFromRaw();
    void           BuildQueryCacheForGlyphs(const IdeographSet& glyphs, IDSStorage& output);
    void           BuildStrokeNeutralCompositionCacheForGlyphs(const IdeographSet& glyphs, IDSStorage& output);
    void           CollectReferencedIdeographs(IDS* ids, IdeographSet& references) const;
    IdeographSet   FindAffectedCacheGlyphs(const IdeographSet& changedGlyphs) const;
    void           RebuildAffectedQueryCache(const IdeographSet& changedGlyphs);
    bool           ParseIDSFile(const std::string& filename, IDSdbFormat dbformat, IDSStorage& output,
        IDSUniqueSeparatorStorage& uniqueSeparators);
    bool           ParseIDSReader(const IDSImportReader& reader, IDSStorage& output,
        IDSUniqueSeparatorStorage& uniqueSeparators);
    int            ImportPrivateDBImpl(IDSImportReader reader, IDSdbFormat dbformat, bool replaceExisting);
    IDSOwnerList   HVExtractAlternativesInternal(
        IDS* ids, bool firstLayer, bool preserveAmbiguousGlyphs, size_t maximum, bool& truncated);
    // 从当前查询缓存中查找；构建缓存时会临时指向原始 IDS。
    std::vector<IDS*> FindIDS(Ideograph ideo, bool ignoreSuffix = false, bool noFallback = false) const;
    bool              HasSuffixVariants(Ideograph ideo);
    bool              MatchesCustomGlyphRange(
        const Ideograph& ideograph, const IDSCustomGlyphRange& range) const;

    bool     IdeoEqual(
        Ideograph ideo1, Ideograph ideo2, bool cpOnly, bool strictStrokeSuffix = true);
    bool     MatchIWDSUnificationPattern(IDS* candidate, IDS* pattern, bool querySide);
    uint32_t CJKsymFallback(uint32_t cp) const;
    bool     IsSingleStrokeIdeograph(const Ideograph& ideograph);
    bool     ContainsOverlay(IDS* ids) const;
    void     AppendSameIDSMatches(IDS* ids, const IDSqueryOptions& options, std::vector<Ideograph>& output);
    bool     ShouldIncludeResult(Ideograph ideograph, const IDSqueryOptions& options) const;
    void     ApplyResultFilter(std::vector<Ideograph>& result, const IDSqueryOptions& options) const;

    bool IDSarrayMatch(const std::vector<IDS*>& s, const std::vector<IDS*>& p, size_t sIdx = 0, size_t pIdx = 0);
    bool IDSmatch(IDS* idsInDB, IDS* ids, bool surroundEqual = true);
    bool MatchVariable(IDS* idsInDB, IDSVariable* variable);
    bool VariableEquivalent(IDS* bound, IDS* candidate);
    bool MatchesSameIDS(IDS* idsInDB, Ideograph query, bool ignoreSuffix = false);
    bool MatchSearchExpression(IDS* idsInDB, SearchExpression* search);
    bool MatchAnyExpression(IDS* idsInDB, SearchExpression* search);
    bool MatchExceptTerms(IDS* idsInDB, const IDSOwnerList& terms);
    bool MatchSearchTermsWithAny(IDS* idsInDB, const IDSOwnerList& terms);
    bool MatchSearchTerms(IDS* idsInDB, const IDSOwnerList& terms);
    bool CanMatchIWDSShape(IDS* idsInDB, IDS* term) const;
    bool MatchSearchTermCached(IDS* idsInDB, IDS* term);
    bool MatchIdeograph(IDS* idsInDB, IDS* ids);
    bool MatchStroke(IDS* idsInDB, IDS* ids);
    bool MatchPattern(IDS* idsInDB, IDS* ids, bool surroundEqual);
    bool MatchPatternPair(Pattern* pidsInDB, Pattern* pids, bool surroundEqual);
    bool ContainsQueryOnlyOperator(IDS* ids);
    IDSOwnerList              BuildReplaceQueries(Pattern* replaceQuery);
    IDSOwnerList              BuildSubtractQueries(Pattern* subtractQuery);
    IDSOwnerList              ExpandQueryOnlyTerm(IDS* term);
    std::vector<IDSOwnerList> ExpandQueryOnlyTerms(const IDSOwnerList& terms);
    IDSOwnerList              ExpandIWDSQuery(IDS* ids, size_t maximum = 64);
    IDSOwnerList              BuildEquivalentQueryOwners(IDS* ids, size_t maximum = 64);
    IDSOwnerList              BuildComponentVariants(Ideograph component);
    IDSOwner ReplaceComponent(IDS* ids, const Ideograph& sourceComponent, const IDSOwnerList& componentVariants,
        IDS* replacement, bool& replaced);
    IDSOwner ReplaceArrangementComponents(Pattern* pattern, const Ideograph& sourceComponent,
        const IDSOwnerList& componentVariants, IDS* replacement, bool& replaced);
    // 处理“⿰首部件⬚”与“⿺包围部件任意”的受限首部件展开匹配。
    bool   MatchEnclosingComponentExpansion(Pattern* pidsInDB, Pattern* pids);
    bool   MatchNormalizedPatterns(Pattern* pidsInDB, Pattern* pids);
    bool   IDSsurroundMatch(Pattern* pidsInDB, Pattern* pids);
    size_t IDSsubarray(const std::vector<IDS*>& s, const std::vector<IDS*>& ideo);
    bool   HasImpossibleStrokeLowerBound(IDS* remain, const std::vector<IDS*>& terms);
    bool   IDSsearch(IDS* idsInDB, std::vector<IDS*>& terms, IDSOwner* newRemain = nullptr);
    bool   FindMatchPath(IDS* candidate, IDS* query, const std::string& path, const std::string& queryPath,
        std::vector<IDSMatchPath>& result,
        const std::vector<std::string>* usedPaths = nullptr);
    bool   BuildMatchDetailForGlyph(
        Ideograph glyph, IDS* query, const IDSqueryOptions& options,
        const std::unordered_map<std::string, size_t>& equivalentIndexes, IDSMatchDetail& detail);
    void   BuildHVMatchPaths(IDS* query, IDS* matched, const std::string& queryPath, const std::string& matchedPath,
        std::vector<IDSMatchPath>& paths);
    void   RecordQueryPreprocessRule(const std::string& expression, IDSPreprocessRule rule);
    void   MergeQueryPreprocessRules(const std::string& target, const std::string& source);
    void   MergeQueryPreprocessRulesTree(const std::string& target, IDS* source);
    void   CollectQueryPreprocessRulesTree(IDS* source, std::vector<IDSPreprocessRule>& rules) const;

public:
    // 兼容现有 C++ 调用；binding 建议将这两个字段包装为整体属性。
    std::string name;
    IDSdbConfig config;

    IDSdatabase(std::string name);
    ~IDSdatabase();

    // [稳定 API：筛选配置]
    // 注册供结果筛选使用的自定义字形范围；同名注册会替换旧定义。
    bool RegisterGlyphRange(const IDSCustomGlyphRange& range);
    bool RegisterUnicodeRange(const std::string& name, uint32_t first, uint32_t last);
    bool UnregisterGlyphRange(const std::string& name);
    std::vector<IDSCustomGlyphRange> GetGlyphRanges() const;

    // [C++ 扩展 API：缓存维护]
    void MakeCache();

    // [稳定 API：数据库导入]
    int ImportDB(std::string filename, IDSdbFormat dbformat = IDSDB_DEFAULT);
    // 第三方可通过读取器导入任意格式；读取器只负责产生统一 IDSimportRecord。
    int ImportDB(IDSImportReader reader, IDSdbFormat dbformat = IDSDB_DEFAULT);
    // 增量导入私有扩展字形；输入存在任一无效 IDS 时不修改数据库。
    int ImportPrivateDB(std::string filename, IDSdbFormat dbformat = IDSDB_DEFAULT);
    int ImportPrivateDB(IDSImportReader reader, IDSdbFormat dbformat = IDSDB_DEFAULT);
    // 以通过校验的文件替换数据库中全部既有私有扩展字形。
    int ReimportPrivateDB(std::string filename, IDSdbFormat dbformat = IDSDB_DEFAULT);
    int ReimportPrivateDB(IDSImportReader reader, IDSdbFormat dbformat = IDSDB_DEFAULT);

    // [稳定 API：导入状态]
    const IDSimportReport& GetLastImportReport() const { return _lastImportReport; }
    const std::string&     GetLastError() const { return _lastError; }
    // 根据原始 IDS 重新生成 HV 查询候选；调用者可随后保存数据库。
    bool RebuildQueryCache();

    // [C++ 扩展 API：外部数据导入]
    // 从 IWDS XML 提取 SourceCodeSeparation 关系并保存到 db/unifiable.sqlite。
    bool ImportIWDSXml(std::string filename);

    // [稳定 API：字形与 IDS 读取]
    IDSOwnerList           GetRawIDSOwned(Ideograph ideo, bool ignoreSuffix = false, bool noFallback = false);
    IDSOwnerList           GetIDSOwned(Ideograph ideo, bool ignoreSuffix = false, bool noFallback = false);
    std::vector<IDS*>      GetIDS(Ideograph ideo, bool ignoreSuffix = false, bool noFallback = false);
    std::vector<Ideograph> GetChar(IDS* ids);
    std::vector<Ideograph> GetContainingCharacters(
        Ideograph ideo, const IDSqueryOptions& options = IDSqueryOptions());
    std::vector<Ideograph> GetSameIDSCharacters(Ideograph ideo) const;

    // [稳定 API：IDS/笔画工具]
    Stroke StrokeSimplify(Stroke stroke);
    // HV 展开并非总是唯一：返回全部去重后的候选；maximum 防止组合展开失控。
    IDSOwnerList             HVExtractOwned(IDS* ids, bool firstLayer = true, bool preserveAmbiguousGlyphs = false,
        size_t maximum = 64, bool* truncated = nullptr);
    // [稳定 API：查询与结果]
    std::vector<Ideograph>   MatchQuery(IDS* ids, const IDSqueryOptions& options = IDSqueryOptions());
    std::vector<IDSMatchDetail> MatchDetailed(
        IDS* ids, const IDSqueryOptions& options = IDSqueryOptions());
    std::vector<std::string> GetEquivalentQueries(IDS* ids);
    // [兼容 API：旧版 Match/Search 调用]
    std::vector<Ideograph>   Match(IDS* ids, std::function<void(int)> progressCallback = nullptr);
    std::vector<Ideograph>   Match(
        IDS* ids, const IDSqueryOptions& options, std::function<void(int)> progressCallback = nullptr);
    std::vector<Ideograph> Search(std::vector<Ideograph> ideo);
    std::vector<Ideograph> Search(const IDSOwnerList& terms);
    std::vector<Ideograph> IdeoSearchByCodepoint(Ideograph ideo);

    bool isEmpty();
};

#endif
