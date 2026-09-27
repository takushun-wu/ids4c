#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "ids4c/ids4c.h"
#include "ids4c/idsdb.h"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

// Python 对象拥有一棵独立的 IDS 树；复制时深拷贝，避免暴露 C++ 内部所有权。
class IDSNodeHandle {
private:
    IDSOwner node_;

public:
    explicit IDSNodeHandle(IDSOwner node): node_(std::move(node)) {
        if(node_ == nullptr) throw std::invalid_argument("IDS node cannot be empty");
    }

    IDSNodeHandle(const IDSNodeHandle& other): node_(other.node_->Clone()) {}
    IDSNodeHandle(IDSNodeHandle&&) noexcept = default;
    IDSNodeHandle& operator=(const IDSNodeHandle& other) {
        if(this != &other) node_ = other.node_->Clone();
        return *this;
    }
    IDSNodeHandle& operator=(IDSNodeHandle&&) noexcept = default;

    IDS* get() const { return node_.get(); }

    IDSNodeHandle clone() const { return IDSNodeHandle(node_->Clone()); }

    std::string text() const { return node_->toString(); }

    std::string unique_separator() const { return node_->GetUniqueSeparator(); }

    std::string kind() const {
        switch(node_->GetType()) {
        case IDS_STROKE:       return "stroke";
        case IDS_IDEOGRAPH:   return "ideograph";
        case IDS_PATTERN:     return "pattern";
        case IDS_SEARCHPARAM: return "search_param";
        case IDS_SEARCH:      return "search";
        case IDS_VARIABLE:    return "variable";
        case IDS_PARSINGTOKEN:return "parsing_token";
        default:              return "unknown";
        }
    }

    std::string idc() const {
        if(node_->GetType() != IDS_PATTERN) return {};
        return IDCtype2char(static_cast<const Pattern*>(node_.get())->GetIDC());
    }

    int idc_type() const {
        if(node_->GetType() != IDS_PATTERN) return static_cast<int>(IDC_UNKNOWN);
        return static_cast<int>(static_cast<const Pattern*>(node_.get())->GetIDC());
    }

    std::vector<IDSNodeHandle> children() const {
        std::vector<IDSNodeHandle> result;
        if(node_->GetType() == IDS_PATTERN) {
            const std::vector<IDS*> children = static_cast<const Pattern*>(node_.get())->GetpIDS();
            result.reserve(children.size());
            for(IDS* child: children) result.emplace_back(child->Clone());
        } else if(node_->GetType() == IDS_SEARCH) {
            const IDSOwnerList& terms = static_cast<const SearchExpression*>(node_.get())->GetTerms();
            result.reserve(terms.size());
            for(const IDSOwner& term: terms) result.emplace_back(term->Clone());
        }
        return result;
    }

    std::vector<IDSNodeHandle> terms() const {
        if(node_->GetType() != IDS_SEARCH) return {};
        const IDSOwnerList& terms = static_cast<const SearchExpression*>(node_.get())->GetTerms();
        std::vector<IDSNodeHandle> result;
        result.reserve(terms.size());
        for(const IDSOwner& term: terms) result.emplace_back(term->Clone());
        return result;
    }

    std::vector<IDSNodeHandle> except_terms() const {
        if(node_->GetType() != IDS_SEARCH) return {};
        const IDSOwnerList& terms = static_cast<const SearchExpression*>(node_.get())->GetExceptTerms();
        std::vector<IDSNodeHandle> result;
        result.reserve(terms.size());
        for(const IDSOwner& term: terms) result.emplace_back(term->Clone());
        return result;
    }

    std::string mode() const {
        if(node_->GetType() != IDS_SEARCH) return {};
        switch(static_cast<const SearchExpression*>(node_.get())->GetMode()) {
        case SEARCH_EXPRESSION_ANY:    return "any";
        case SEARCH_EXPRESSION_EXCEPT: return "except";
        default:                       return "all";
        }
    }

    std::string glyph() const {
        if(node_->GetType() != IDS_IDEOGRAPH) return {};
        return static_cast<const Ideograph*>(node_.get())->toString();
    }

    uint32_t codepoint() const {
        if(node_->GetType() != IDS_IDEOGRAPH) return 0;
        return static_cast<const Ideograph*>(node_.get())->GetIdeo();
    }

    uint32_t variation_selector() const {
        if(node_->GetType() != IDS_IDEOGRAPH) return 0;
        return static_cast<const Ideograph*>(node_.get())->GetVS();
    }

    std::string suffix() const {
        if(node_->GetType() != IDS_IDEOGRAPH) return {};
        return static_cast<const Ideograph*>(node_.get())->GetSuffix();
    }

    std::string abstract_name() const {
        if(node_->GetType() != IDS_IDEOGRAPH) return {};
        return static_cast<const Ideograph*>(node_.get())->GetAbstractName();
    }

    std::vector<std::pair<bool, std::string>> stroke_data() const {
        if(node_->GetType() != IDS_STROKE) return {};
        const std::vector<Stroke_Data> data = static_cast<Stroke*>(node_.get())->GetStroke();
        std::vector<std::pair<bool, std::string>> result;
        result.reserve(data.size());
        for(const Stroke_Data& item: data) result.emplace_back(item.neg, item.stroke);
        return result;
    }

    std::vector<size_t> break_positions() const {
        if(node_->GetType() != IDS_STROKE) return {};
        return static_cast<Stroke*>(node_.get())->GetBreakPos();
    }

    std::vector<std::pair<size_t, size_t>> cross_data() const {
        if(node_->GetType() != IDS_STROKE) return {};
        const std::vector<Stroke_CrossData> data = static_cast<Stroke*>(node_.get())->GetCrossData();
        std::vector<std::pair<size_t, size_t>> result;
        result.reserve(data.size());
        for(const Stroke_CrossData& item: data) result.emplace_back(item.pos, item.cross);
        return result;
    }

    bool enclosed() const {
        if(node_->GetType() != IDS_STROKE) return false;
        return static_cast<Stroke*>(node_.get())->isEnclosed();
    }

    std::string param() const {
        if(node_->GetType() != IDS_SEARCHPARAM) return {};
        switch(static_cast<SearchParam*>(node_.get())->GetParam()) {
        case SPARAM_QUESTIONMARK:        return "questionmark";
        case SPARAM_STROKE_COUNT:        return "stroke";
        case SPARAM_RESIDUE_STROKE_COUNT:return "residue";
        default:                         return "unknown";
        }
    }

    uint32_t stroke_minimum() const {
        if(node_->GetType() != IDS_SEARCHPARAM) return 0;
        return static_cast<const SearchParam*>(node_.get())->GetStrokeMinimum();
    }

    uint32_t stroke_maximum() const {
        if(node_->GetType() != IDS_SEARCHPARAM) return 0;
        return static_cast<const SearchParam*>(node_.get())->GetStrokeMaximum();
    }

    std::string variable_name() const {
        if(node_->GetType() != IDS_VARIABLE) return {};
        return static_cast<const IDSVariable*>(node_.get())->GetName();
    }

    IDSNodeHandle with_children(const std::vector<IDSNodeHandle>& children) const {
        if(node_->GetType() != IDS_PATTERN)
            throw py::type_error("with_children() is only available for pattern nodes");
        const Pattern* pattern = static_cast<const Pattern*>(node_.get());
        std::vector<IDS*> rawChildren;
        rawChildren.reserve(children.size());
        for(const IDSNodeHandle& child: children) rawChildren.push_back(child.get());
        int overlayRange[2] = {0, 0};
        pattern->GetOverlayRange(overlayRange);
        IDSOwner result(new Pattern(pattern->GetIDC(), rawChildren, pattern->GetPreferSplitPoint(), overlayRange,
            pattern->GetOverlayType(), pattern->GetOptionalInt()));
        return IDSNodeHandle(std::move(result));
    }

    bool operator==(const IDSNodeHandle& other) const { return IDSequal(node_.get(), other.node_.get()); }
};

class DatabaseHandle {
private:
    std::unique_ptr<IDSdatabase> database_;

public:
    explicit DatabaseHandle(std::string name): database_(new IDSdatabase(std::move(name))) {}

    IDSdatabase& get() { return *database_; }
    const IDSdatabase& get() const { return *database_; }

    const std::string& name() const { return database_->name; }
};

static std::string MatchKindName(IDSMatchKind kind) {
    return IDSMatchKindName(kind);
}

static std::string MatchSourceName(IDSMatchSource source) {
    return IDSMatchSourceName(source);
}

struct MatchPathInfo {
    IDSMatchPathKind kind = IDS_MATCH_PATH_NODE;
    size_t index = std::numeric_limits<size_t>::max();
    size_t query_expression_index = std::numeric_limits<size_t>::max();
    std::string query_path;
    std::string path;

    py::object index_value() const {
        if(index == std::numeric_limits<size_t>::max()) return py::none();
        return py::int_(index);
    }

    py::object query_expression_index_value() const {
        if(query_expression_index == std::numeric_limits<size_t>::max()) return py::none();
        return py::int_(query_expression_index);
    }

    py::dict to_dict() const {
        py::dict result;
        result["kind"] = IDSMatchPathKindName(kind);
        result["index"] = index_value();
        result["query_expression_index"] = query_expression_index_value();
        result["query_path"] = query_path;
        result["path"] = path;
        return result;
    }
};

struct MatchDetailInfo {
    std::string glyph;
    std::string matched_ids;
    std::string raw_ids;
    std::string match_kind;
    std::string match_source;
    std::vector<MatchPathInfo> match_paths;
    std::vector<std::string> preprocess_rules;

    py::dict to_dict() const {
        py::dict result;
        result["glyph"] = glyph;
        result["matched_ids"] = matched_ids;
        result["raw_ids"] = raw_ids;
        result["match_kind"] = match_kind;
        result["match_source"] = match_source;
        result["preprocess_rules"] = preprocess_rules;
        result["match_paths"] = match_paths;
        return result;
    }
};

struct QueryResultInfo {
    std::vector<std::string> equivalent_syntax;
    std::vector<MatchDetailInfo> matches;

    py::dict to_dict() const {
        py::dict result;
        result["equivalent_syntax"] = equivalent_syntax;
        result["matches"] = matches;
        return result;
    }
};

static MatchPathInfo ConvertMatchPath(const IDSMatchPath& path) {
    MatchPathInfo result;
    result.kind = path.kind;
    result.index = path.index;
    result.query_expression_index = path.queryExpressionIndex;
    result.query_path = path.queryPath;
    result.path = path.path;
    return result;
}

static MatchDetailInfo ConvertMatchDetail(const IDSMatchDetail& detail) {
    MatchDetailInfo result;
    result.glyph = detail.glyph.toString();
    result.matched_ids = detail.matchedIDS;
    result.raw_ids = detail.rawIDS;
    result.match_kind = MatchKindName(detail.matchKind);
    result.match_source = MatchSourceName(detail.matchSource);
    result.preprocess_rules.reserve(detail.preprocessRules.size());
    for(IDSPreprocessRule rule: detail.preprocessRules)
        result.preprocess_rules.push_back(IDSPreprocessRuleName(rule));
    result.match_paths.reserve(detail.matchPaths.size());
    for(const IDSMatchPath& path: detail.matchPaths) result.match_paths.push_back(ConvertMatchPath(path));
    return result;
}

static std::vector<IDSNodeHandle> ConvertOwnedNodes(IDSOwnerList nodes) {
    std::vector<IDSNodeHandle> result;
    result.reserve(nodes.size());
    for(IDSOwner& node: nodes) result.emplace_back(std::move(node));
    return result;
}

static Ideograph GlyphFromObject(py::handle object) {
    if(py::isinstance<IDSNodeHandle>(object)) {
        const IDSNodeHandle& node = object.cast<const IDSNodeHandle&>();
        if(node.get()->GetType() != IDS_IDEOGRAPH)
            throw py::type_error("expected an ideograph IDSNode or a glyph string");
        return *static_cast<const Ideograph*>(node.get());
    }
    return Ideograph(object.cast<std::string>());
}

static IDSNodeHandle ParseNode(const std::string& text) {
    IDSParseError error;
    IDSOwner node = ParseIDSOwned(text, &error);
    if(node != nullptr) return IDSNodeHandle(std::move(node));
    if(error.hasPosition)
        throw py::value_error("invalid IDS at code-point " + std::to_string(error.position) + ": " + error.message);
    throw py::value_error("invalid IDS: " + error.message);
}

static std::vector<std::string> MatchGlyphs(DatabaseHandle& database, const IDSNodeHandle& query,
    const IDSqueryOptions& options) {
    const std::vector<Ideograph> matches = database.get().MatchQuery(query.get(), options);
    std::vector<std::string> result;
    result.reserve(matches.size());
    for(const Ideograph& glyph: matches) result.push_back(glyph.toString());
    return result;
}

static std::vector<MatchDetailInfo> MatchDetails(DatabaseHandle& database, const IDSNodeHandle& query,
    const IDSqueryOptions& options) {
    const std::vector<IDSMatchDetail> details = database.get().MatchDetailed(query.get(), options);
    std::vector<MatchDetailInfo> result;
    result.reserve(details.size());
    for(const IDSMatchDetail& detail: details) result.push_back(ConvertMatchDetail(detail));
    return result;
}

} // namespace

PYBIND11_MODULE(ids4c, module) {
    module.doc() = "Basic Python bindings for ids4c IDS trees and database queries.";
    module.attr("IDSDB_DEFAULT") = py::int_(static_cast<int>(IDSDB_DEFAULT));
    module.attr("IDSDB_YIBAI") = py::int_(static_cast<int>(IDSDB_YIBAI));

    py::enum_<IDSdbFormat>(module, "DatabaseFormat")
        .value("DEFAULT", IDSDB_DEFAULT)
        .value("YIBAI", IDSDB_YIBAI);

    py::enum_<IDSresultFilter>(module, "ResultFilter")
        .value("ALL", IDS_RESULT_ALL)
        .value("IGNORE_LC_SUFFIX", IDS_RESULT_IGNORE_LC_SUFFIX)
        .value("IGNORE_OTHER_LOCALES_BASE_ONLY", IDS_RESULT_IGNORE_OTHER_LOCALES_BASE_ONLY)
        .value("IGNORE_OTHER_LOCALES_KEEP_IVS", IDS_RESULT_IGNORE_OTHER_LOCALES_KEEP_IVS);

    py::enum_<IDSglyphDomain>(module, "GlyphDomain")
        .value("ALL", IDS_GLYPH_DOMAIN_ALL)
        .value("UNICODE", IDS_GLYPH_DOMAIN_UNICODE)
        .value("PRIVATE", IDS_GLYPH_DOMAIN_PRIVATE)
        .value("ABSTRACT", IDS_GLYPH_DOMAIN_ABSTRACT);

    py::enum_<IDSunicodeBlock>(module, "UnicodeBlock")
        .value("ALL", IDS_UNICODE_BLOCK_ALL)
        .value("CJK", IDS_UNICODE_BLOCK_CJK)
        .value("CJK_BASIC", IDS_UNICODE_BLOCK_CJK_BASIC)
        .value("CJK_EXT_A", IDS_UNICODE_BLOCK_CJK_EXT_A)
        .value("CJK_EXT_B", IDS_UNICODE_BLOCK_CJK_EXT_B)
        .value("CJK_EXT_C", IDS_UNICODE_BLOCK_CJK_EXT_C)
        .value("CJK_EXT_D", IDS_UNICODE_BLOCK_CJK_EXT_D)
        .value("CJK_EXT_E", IDS_UNICODE_BLOCK_CJK_EXT_E)
        .value("CJK_EXT_F", IDS_UNICODE_BLOCK_CJK_EXT_F)
        .value("CJK_EXT_G", IDS_UNICODE_BLOCK_CJK_EXT_G)
        .value("CJK_EXT_H", IDS_UNICODE_BLOCK_CJK_EXT_H)
        .value("CJK_EXT_I", IDS_UNICODE_BLOCK_CJK_EXT_I)
        .value("CJK_EXT_J", IDS_UNICODE_BLOCK_CJK_EXT_J)
        .value("CJK_COMPATIBILITY", IDS_UNICODE_BLOCK_CJK_COMPATIBILITY)
        .value("CJK_RADICALS", IDS_UNICODE_BLOCK_CJK_RADICALS)
        .value("CJK_STROKES", IDS_UNICODE_BLOCK_CJK_STROKES)
        .value("PRIVATE_BMP", IDS_UNICODE_BLOCK_PRIVATE_BMP)
        .value("PRIVATE_PLANE15", IDS_UNICODE_BLOCK_PRIVATE_PLANE15)
        .value("PRIVATE_PLANE16", IDS_UNICODE_BLOCK_PRIVATE_PLANE16)
        .value("ABSTRACT", IDS_UNICODE_BLOCK_ABSTRACT)
        .value("OTHER", IDS_UNICODE_BLOCK_OTHER);

    py::enum_<IWDSUnificationLevel>(module, "IWDSUnificationLevel")
        .value("NONE", IWDS_UNIFICATION_NONE)
        .value("SOURCE_CODE_SEPARATION", IWDS_UNIFICATION_SOURCE_CODE_SEPARATION)
        .value("LV1", IWDS_UNIFICATION_LV1)
        .value("LV2", IWDS_UNIFICATION_LV2);

    py::class_<IDSFilterOptions>(module, "FilterOptions")
        .def(py::init<>())
        .def_readwrite("ignore_overlay_structure", &IDSFilterOptions::ignoreOverlayStructure)
        .def_readwrite("result_filter", &IDSFilterOptions::resultFilter)
        .def_readwrite("glyph_domain", &IDSFilterOptions::glyphDomain)
        .def_readwrite("unicode_blocks", &IDSFilterOptions::unicodeBlocks)
        .def_readwrite("custom_ranges", &IDSFilterOptions::customRanges)
        .def_readwrite("locale_suffix_fallback_order", &IDSFilterOptions::localeSuffixFallbackOrder);

    py::class_<IDSqueryOptions>(module, "QueryOptions")
        .def(py::init<>())
        .def_readwrite("filter", &IDSqueryOptions::filter)
        .def_readwrite("track_match_paths", &IDSqueryOptions::trackMatchPaths);

    py::class_<IDSFuzzyMatchOptions>(module, "FuzzyMatchOptions")
        .def(py::init<>())
        .def_readwrite("unification_level", &IDSFuzzyMatchOptions::unificationLevel)
        .def_readwrite("default_region", &IDSFuzzyMatchOptions::defaultRegion)
        .def_readwrite("stroke_neutral_composition", &IDSFuzzyMatchOptions::strokeNeutralComposition)
        .def_readwrite("exclude_non_equivalent_same_ids", &IDSFuzzyMatchOptions::excludeNonEquivalentSameIDS)
        .def_readwrite("locale_suffix_fallback_order", &IDSFuzzyMatchOptions::localeSuffixFallbackOrder);

    py::class_<IDSMiscOptions>(module, "MiscOptions")
        .def(py::init<>())
        .def_readwrite("enable_cache", &IDSMiscOptions::enableCache)
        .def_readwrite("sym_fallback", &IDSMiscOptions::symFallback)
        .def_readwrite("suffix_is_alt_form", &IDSMiscOptions::suffixRisAltForm);

    py::class_<IDSdbConfig>(module, "DatabaseConfig")
        .def(py::init<>())
        .def_readwrite("fuzzy_match", &IDSdbConfig::fuzzyMatch)
        .def_readwrite("misc", &IDSdbConfig::misc);

    py::class_<IDSNodeHandle>(module, "IDSNode")
        .def_property_readonly("kind", &IDSNodeHandle::kind)
        .def_property_readonly("unique_separator", &IDSNodeHandle::unique_separator)
        .def_property_readonly("text", &IDSNodeHandle::text)
        .def_property_readonly("idc", &IDSNodeHandle::idc)
        .def_property_readonly("idc_type", &IDSNodeHandle::idc_type)
        .def_property_readonly("children", &IDSNodeHandle::children)
        .def_property_readonly("terms", &IDSNodeHandle::terms)
        .def_property_readonly("except_terms", &IDSNodeHandle::except_terms)
        .def_property_readonly("mode", &IDSNodeHandle::mode)
        .def_property_readonly("glyph", &IDSNodeHandle::glyph)
        .def_property_readonly("codepoint", &IDSNodeHandle::codepoint)
        .def_property_readonly("variation_selector", &IDSNodeHandle::variation_selector)
        .def_property_readonly("suffix", &IDSNodeHandle::suffix)
        .def_property_readonly("abstract_name", &IDSNodeHandle::abstract_name)
        .def_property_readonly("stroke_data", &IDSNodeHandle::stroke_data)
        .def_property_readonly("break_positions", &IDSNodeHandle::break_positions)
        .def_property_readonly("cross_data", &IDSNodeHandle::cross_data)
        .def_property_readonly("enclosed", &IDSNodeHandle::enclosed)
        .def_property_readonly("param", &IDSNodeHandle::param)
        .def_property_readonly("stroke_minimum", &IDSNodeHandle::stroke_minimum)
        .def_property_readonly("stroke_maximum", &IDSNodeHandle::stroke_maximum)
        .def_property_readonly("variable_name", &IDSNodeHandle::variable_name)
        .def("clone", &IDSNodeHandle::clone)
        .def("with_children", &IDSNodeHandle::with_children, py::arg("children"))
        .def("__str__", &IDSNodeHandle::text)
        .def("__repr__", [](const IDSNodeHandle& node) { return "IDSNode(" + node.text() + ")"; })
        .def("__eq__", &IDSNodeHandle::operator==)
        .def("__ne__", [](const IDSNodeHandle& left, const IDSNodeHandle& right) { return !(left == right); });

    py::class_<MatchPathInfo>(module, "MatchPath")
        .def_property_readonly("kind", [](const MatchPathInfo& value) { return IDSMatchPathKindName(value.kind); })
        .def_property_readonly("index", &MatchPathInfo::index_value)
        .def_property_readonly("query_expression_index", &MatchPathInfo::query_expression_index_value)
        .def_readonly("query_path", &MatchPathInfo::query_path)
        .def_readonly("path", &MatchPathInfo::path)
        .def("to_dict", &MatchPathInfo::to_dict);

    py::class_<MatchDetailInfo>(module, "MatchDetail")
        .def_readonly("glyph", &MatchDetailInfo::glyph)
        .def_readonly("matched_ids", &MatchDetailInfo::matched_ids)
        .def_readonly("raw_ids", &MatchDetailInfo::raw_ids)
        .def_readonly("match_kind", &MatchDetailInfo::match_kind)
        .def_readonly("match_source", &MatchDetailInfo::match_source)
        .def_readonly("preprocess_rules", &MatchDetailInfo::preprocess_rules)
        .def_readonly("match_paths", &MatchDetailInfo::match_paths)
        .def("to_dict", &MatchDetailInfo::to_dict);

    py::class_<QueryResultInfo>(module, "QueryResult")
        .def_readonly("equivalent_syntax", &QueryResultInfo::equivalent_syntax)
        .def_readonly("matches", &QueryResultInfo::matches)
        .def("to_dict", &QueryResultInfo::to_dict);

    py::class_<DatabaseHandle>(module, "Database")
        .def(py::init<std::string>(), py::arg("name"))
        .def_property_readonly("name", &DatabaseHandle::name)
        .def_property("config",
            [](DatabaseHandle& database) -> IDSdbConfig& { return database.get().config; },
            [](DatabaseHandle& database, const IDSdbConfig& config) { database.get().config = config; },
            py::return_value_policy::reference_internal)
        .def_property_readonly("last_error", [](const DatabaseHandle& database) { return database.get().GetLastError(); })
        .def_property_readonly("last_import_report", [](const DatabaseHandle& database) {
            const IDSimportReport& report = database.get().GetLastImportReport();
            py::dict result;
            result["input_lines"] = report.inputLines;
            result["data_lines"] = report.dataLines;
            result["source_expressions"] = report.sourceExpressions;
            result["accepted_expressions"] = report.acceptedExpressions;
            result["rejected_expressions"] = report.rejectedExpressions;
            result["query_cache_entries"] = report.queryCacheEntries;
            result["rebuilt_cache_glyphs"] = report.rebuiltCacheGlyphs;
            result["cache_truncations"] = report.cacheTruncations;
            py::list issues;
            for(const IDSimportIssue& issue: report.issues) {
                py::dict item;
                item["line"] = issue.line;
                item["ids_index"] = issue.idsIndex;
                item["character_index"] = issue.characterIndex;
                item["unexpected_character"] = issue.unexpectedCharacter;
                item["glyph"] = issue.glyph;
                item["expression"] = issue.expression;
                item["message"] = issue.message;
                issues.append(item);
            }
            result["issues"] = issues;
            return result;
        })
        .def("is_empty", [](DatabaseHandle& database) { return database.get().isEmpty(); })
        .def("import_file", [](DatabaseHandle& database, const std::string& filename, IDSdbFormat format) {
            const int status = database.get().ImportDB(filename, format);
            if(status != 0) throw std::runtime_error("IDS database import failed: " + database.get().GetLastError());
            return status;
        }, py::arg("filename"), py::arg("format") = IDSDB_DEFAULT)
        .def("import_private_file", [](DatabaseHandle& database, const std::string& filename, IDSdbFormat format,
            bool replace) {
            const int status = replace ? database.get().ReimportPrivateDB(filename, format)
                                       : database.get().ImportPrivateDB(filename, format);
            if(status != 0) throw std::runtime_error("private IDS database import failed: " + database.get().GetLastError());
            return status;
        }, py::arg("filename"), py::arg("format") = IDSDB_DEFAULT, py::arg("replace") = false)
        .def("raw_ids", [](DatabaseHandle& database, py::handle glyph, bool ignore_suffix) {
            return ConvertOwnedNodes(database.get().GetRawIDSOwned(GlyphFromObject(glyph), ignore_suffix));
        }, py::arg("glyph"), py::arg("ignore_suffix") = false)
        .def("ids", [](DatabaseHandle& database, py::handle glyph, bool ignore_suffix) {
            return ConvertOwnedNodes(database.get().GetIDSOwned(GlyphFromObject(glyph), ignore_suffix));
        }, py::arg("glyph"), py::arg("ignore_suffix") = false)
        .def("hv_extract", [](DatabaseHandle& database, const IDSNodeHandle& node, bool first_layer,
            bool preserve_ambiguous, size_t maximum) {
            bool truncated = false;
            IDSOwnerList result = database.get().HVExtractOwned(node.get(), first_layer, preserve_ambiguous, maximum, &truncated);
            return ConvertOwnedNodes(std::move(result));
        }, py::arg("node"), py::arg("first_layer") = true, py::arg("preserve_ambiguous") = false,
            py::arg("maximum") = 64)
        .def("equivalent_queries", [](DatabaseHandle& database, const IDSNodeHandle& query) {
            return database.get().GetEquivalentQueries(query.get());
        })
        .def("match", &MatchGlyphs, py::arg("query"), py::arg("options") = IDSqueryOptions())
        .def("match_detailed", &MatchDetails, py::arg("query"), py::arg("options") = IDSqueryOptions())
        .def("query", [](DatabaseHandle& database, const IDSNodeHandle& query, const IDSqueryOptions& options) {
            QueryResultInfo result;
            result.equivalent_syntax = database.get().GetEquivalentQueries(query.get());
            result.matches = MatchDetails(database, query, options);
            return result;
        }, py::arg("query"), py::arg("options") = IDSqueryOptions());

    module.def("parse", &ParseNode, py::arg("text"));
    module.def("equal", [](const IDSNodeHandle& left, const IDSNodeHandle& right) { return left == right; });
}
