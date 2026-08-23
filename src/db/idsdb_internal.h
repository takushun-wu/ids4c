#ifndef IDS4C_DB_INTERNAL_H
#define IDS4C_DB_INTERNAL_H

#include "ids4c/idsdb.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 仅供 src/db 内部文件共享的辅助函数和构建器。
IDSImportReader MakeTextIDSReader(const std::string& filename, IDSdbFormat dbformat);

// FNV-1a 64-bit is deterministic across processes. The exact IDS text remains
// the collision guard, so the hash is only an index and never the equality test.
inline uint64_t IDSExpressionHash(const std::string& expression) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for(size_t index = 0; index < expression.size(); index++) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(expression[index]));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

inline void ConfigureDatabaseFormat(IDSdbConfig& config, IDSdbFormat format) {
    config = IDSdbConfig();
    if(format == IDSDB_YIBAI)
        config.misc.suffixRisAltForm = true;
}

class UnificationGroupBuilder {
public:
    void AddGroup(const std::vector<Ideograph>& glyphs) {
        if(glyphs.size() < 2) return;
        const size_t first = AddGlyph(glyphs.front());
        for(size_t index = 1; index < glyphs.size(); index++)
            Unite(first, AddGlyph(glyphs[index]));
    }

    void AddPair(const Ideograph& left, const Ideograph& right) { Unite(AddGlyph(left), AddGlyph(right)); }

    std::vector<std::vector<Ideograph>> BuildGroups() {
        std::unordered_map<size_t, size_t>  groupIndexByRoot;
        std::vector<std::vector<Ideograph>> groups;
        for(size_t index = 0; index < _glyphs.size(); index++) {
            const size_t root  = Find(index);
            const auto   found = groupIndexByRoot.find(root);
            if(found == groupIndexByRoot.end()) {
                groupIndexByRoot.insert({root, groups.size()});
                groups.push_back(std::vector<Ideograph>());
                groups.back().push_back(_glyphs[index]);
            } else
                groups[found->second].push_back(_glyphs[index]);
        }

        std::vector<std::vector<Ideograph>> nontrivialGroups;
        for(auto& group: groups)
            if(group.size() >= 2) nontrivialGroups.push_back(std::move(group));
        return nontrivialGroups;
    }

private:
    size_t AddGlyph(const Ideograph& glyph) {
        const auto found = _glyphIndex.find(glyph);
        if(found != _glyphIndex.end()) return found->second;
        const size_t index = _glyphs.size();
        _glyphs.push_back(glyph);
        _parents.push_back(index);
        _glyphIndex.insert({glyph, index});
        return index;
    }

    size_t Find(size_t index) {
        size_t root = index;
        while(_parents[root] != root)
            root = _parents[root];
        while(_parents[index] != index) {
            const size_t parent = _parents[index];
            _parents[index]     = root;
            index               = parent;
        }
        return root;
    }

    void Unite(size_t left, size_t right) {
        left  = Find(left);
        right = Find(right);
        if(left != right) _parents[right] = left;
    }

    std::vector<Ideograph>                                _glyphs;
    std::vector<size_t>                                   _parents;
    std::unordered_map<Ideograph, size_t, Ideograph_Hash> _glyphIndex;
};
class UnificationExpressionGroupBuilder {
public:
    void AddGroup(const std::vector<std::string>& expressions) {
        if(expressions.size() < 2) return;
        const size_t first = AddExpression(expressions.front());
        for(size_t index = 1; index < expressions.size(); index++)
            Unite(first, AddExpression(expressions[index]));
    }

    std::vector<std::vector<std::string>> BuildGroups() {
        std::unordered_map<size_t, size_t>    groupIndexByRoot;
        std::vector<std::vector<std::string>> groups;
        for(size_t index = 0; index < _expressions.size(); index++) {
            const size_t root  = Find(index);
            const auto   found = groupIndexByRoot.find(root);
            if(found == groupIndexByRoot.end()) {
                groupIndexByRoot.insert({root, groups.size()});
                groups.push_back(std::vector<std::string>());
                groups.back().push_back(_expressions[index]);
            } else
                groups[found->second].push_back(_expressions[index]);
        }

        std::vector<std::vector<std::string>> nontrivialGroups;
        for(auto& group: groups)
            if(group.size() >= 2) nontrivialGroups.push_back(std::move(group));
        return nontrivialGroups;
    }

private:
    size_t AddExpression(const std::string& expression) {
        const auto found = _expressionIndex.find(expression);
        if(found != _expressionIndex.end()) return found->second;
        const size_t index = _expressions.size();
        _expressions.push_back(expression);
        _parents.push_back(index);
        _expressionIndex.insert({expression, index});
        return index;
    }

    size_t Find(size_t index) {
        size_t root = index;
        while(_parents[root] != root)
            root = _parents[root];
        while(_parents[index] != index) {
            const size_t parent = _parents[index];
            _parents[index]     = root;
            index               = parent;
        }
        return root;
    }

    void Unite(size_t left, size_t right) {
        left  = Find(left);
        right = Find(right);
        if(left != right) _parents[right] = left;
    }

    std::vector<std::string>                _expressions;
    std::vector<size_t>                     _parents;
    std::unordered_map<std::string, size_t> _expressionIndex;
};

inline bool IsType(IDS* ids, IDStype type) {
    return ids != nullptr && ids->GetType() == type;
}

inline bool IsIdeograph(IDS* ids) {
    return IsType(ids, IDS_IDEOGRAPH);
}

inline bool IsStroke(IDS* ids) {
    return IsType(ids, IDS_STROKE);
}

inline bool IsPattern(IDS* ids) {
    return IsType(ids, IDS_PATTERN);
}

inline bool IsSearchParam(IDS* ids) {
    return IsType(ids, IDS_SEARCHPARAM);
}

inline bool IsSearchExpression(IDS* ids) {
    return IsType(ids, IDS_SEARCH);
}

inline bool IsVariable(IDS* ids) {
    return IsType(ids, IDS_VARIABLE);
}

inline Ideograph* AsIdeograph(IDS* ids) {
    return (Ideograph*)ids;
}

inline Stroke* AsStroke(IDS* ids) {
    return (Stroke*)ids;
}

inline Pattern* AsPattern(IDS* ids) {
    return (Pattern*)ids;
}

inline SearchParam* AsSearchParam(IDS* ids) {
    return (SearchParam*)ids;
}

inline SearchExpression* AsSearchExpression(IDS* ids) {
    return (SearchExpression*)ids;
}

inline IDSVariable* AsVariable(IDS* ids) {
    return (IDSVariable*)ids;
}

#endif
