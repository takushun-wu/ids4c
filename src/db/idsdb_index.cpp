#include "ids4c/idsconst.h"
#include "idsdb_internal.h"

#include <functional>
#include <unordered_set>

namespace {

std::string ComponentKey(const Ideograph& glyph) {
    if(!glyph.inUnicode()) return Ideograph(glyph.GetAbstractName()).toString();
    uint32_t cp = glyph.GetIdeo();
    const auto fallback = CJKsymFallbackTable.find(cp);
    if(fallback != CJKsymFallbackTable.end()) cp = fallback->second;
    return Ideograph(cp).toString();
}

void CollectDirectKeys(IDS* ids, std::unordered_set<std::string>& keys) {
    if(ids == nullptr) return;
    if(IsIdeograph(ids)) {
        keys.insert(ComponentKey(*AsIdeograph(ids)));
        return;
    }
    if(IsStroke(ids)) {
        for(const Stroke_Data& token: AsStroke(ids)->GetStroke()) {
            IDSOwner parsed = ParseIDSOwned(token.stroke);
            if(parsed != nullptr && IsIdeograph(parsed.get()))
                keys.insert(ComponentKey(*AsIdeograph(parsed.get())));
            else
                keys.insert(token.stroke);
        }
        return;
    }
    if(!IsPattern(ids)) return;

    Pattern* pattern = AsPattern(ids);
    for(const HVOriginRange& origin: pattern->GetHVOriginRanges()) {
        IDSOwner parsed = ParseIDSOwned(origin.glyph);
        if(parsed != nullptr && IsIdeograph(parsed.get()))
            keys.insert(ComponentKey(*AsIdeograph(parsed.get())));
    }
    for(const auto& child: pattern->GetpIDSRef()) CollectDirectKeys(child.get(), keys);
}

} // namespace

void IDSdatabase::BuildDirectComponentIndex() {
    for(auto& source: _directComponentIndex) source.clear();
    const IDSStorage* sources[] = {&_rawIDSDB, &_idsDB, &_strokeNeutralCompositionDB};
    for(size_t sourceIndex = 0; sourceIndex < 3; sourceIndex++) {
        for(const auto& entry: *sources[sourceIndex]) {
            std::unordered_set<std::string> keys;
            for(const auto& ids: entry.second) CollectDirectKeys(ids.get(), keys);
            if(sourceIndex == 1) keys.insert(ComponentKey(entry.first));
            for(const std::string& key: keys) _directComponentIndex[sourceIndex][key].insert(entry.first);
        }
    }
    _directComponentIndexReady = true;
}

bool IDSdatabase::FindRawLiteralSearchCandidates(IDS* query, IdeographSet& candidates) const {
    candidates.clear();
    if(!_directComponentIndexReady || !IsSearchExpression(query)) return false;

    const auto& postings = _directComponentIndex[0];
    const auto& variants = GetSameIDSVariantIndex();
    std::vector<std::string> pending;
    std::unordered_set<std::string> seen;
    std::function<void(IDS*)> collect = [&](IDS* node) {
        if(IsIdeograph(node)) {
            const Ideograph& glyph = *AsIdeograph(node);
            auto append = [&](const Ideograph& variant) {
                candidates.insert(variant);
                const std::string key = ComponentKey(variant);
                if(seen.insert(key).second) pending.push_back(key);
            };
            append(glyph);
            const auto foundVariants = variants.find(glyph);
            if(foundVariants != variants.end())
                for(const Ideograph& variant: foundVariants->second) append(variant);
        } else if(IsPattern(node)) {
            for(const auto& child: AsPattern(node)->GetpIDSRef()) collect(child.get());
        } else if(IsSearchExpression(node)) {
            for(const auto& term: AsSearchExpression(node)->GetTerms()) collect(term.get());
        }
    };
    collect(query);
    while(!pending.empty()) {
        const std::string key = std::move(pending.back());
        pending.pop_back();
        const auto found = postings.find(key);
        if(found == postings.end()) continue;
        for(const Ideograph& owner: found->second) {
            if(!candidates.insert(owner).second) continue;
            const std::string ownerKey = ComponentKey(owner);
            if(seen.insert(ownerKey).second) pending.push_back(ownerKey);
        }
        if(candidates.size() >= _idsDB.size() - _idsDB.size() / 4) return false;
    }
    return true;
}

bool IDSdatabase::FindIndexedSearchCandidates(IDS* query, IdeographSet& candidates) {
    candidates.clear();
    if(!_directComponentIndexReady || !IsSearchExpression(query) || _idsDB.empty()) return false;

    auto directCandidates = [&](const Ideograph& glyph, IdeographSet& output, bool requireStrokeOnly) {
        std::vector<Ideograph> seeds(1, glyph);
        const auto& variants = GetSameIDSVariantIndex();
        const auto foundVariants = variants.find(glyph);
        if(foundVariants != variants.end())
            seeds.insert(seeds.end(), foundVariants->second.begin(), foundVariants->second.end());

        if(requireStrokeOnly)
            for(const Ideograph& seed: seeds)
                for(IDS* definition: FindIDS(seed))
                    if(!IsStroke(definition)) return false;

        std::vector<std::string> pending;
        std::unordered_set<std::string> seen;
        for(const Ideograph& seed: seeds) {
            const std::string key = ComponentKey(seed);
            if(seen.insert(key).second) pending.push_back(key);
        }

        while(!pending.empty()) {
            const std::string key = std::move(pending.back());
            pending.pop_back();
            for(size_t sourceIndex = 0; sourceIndex < _directComponentIndex.size(); sourceIndex++) {
                if(sourceIndex == 2 && !config.fuzzyMatch.strokeNeutralComposition) continue;
                const auto found = _directComponentIndex[sourceIndex].find(key);
                if(found == _directComponentIndex[sourceIndex].end()) continue;
                for(const Ideograph& owner: found->second) {
                    if(!output.insert(owner).second) continue;
                    const std::string ownerKey = ComponentKey(owner);
                    if(seen.insert(ownerKey).second) pending.push_back(ownerKey);
                }
            }
            // 常见部件的闭包接近全库时，继续全扫描通常更便宜。
            if(output.size() >= _idsDB.size() - _idsDB.size() / 4) return false;
        }
        return true;
    };

    auto glyphCandidates = [&](const Ideograph& glyph, IdeographSet& output) {
        std::vector<Ideograph> seeds(1, glyph);
        const auto& variants = GetSameIDSVariantIndex();
        const auto foundVariants = variants.find(glyph);
        if(foundVariants != variants.end())
            seeds.insert(seeds.end(), foundVariants->second.begin(), foundVariants->second.end());

        if(!directCandidates(glyph, output, false)) return false;
        for(const Ideograph& seed: seeds) {
            for(IDS* definition: FindIDS(seed)) {
                if(IsStroke(definition)) continue;
                if(!IsPattern(definition)) return false;
                Pattern* pattern = AsPattern(definition);
                const auto& children = pattern->GetpIDSRef();
                if(!pattern->GetHVOriginRanges().empty() || pattern->GetOptionalInt() != 0 || children.size() < 2)
                    return false;
                for(const auto& child: children)
                    if(!IsIdeograph(child.get())) return false;

                // 固定结构的任一子部件都必须匹配；选闭包最小者作为不漏项的候选锚点。
                IdeographSet bestAnchor;
                bool haveAnchor = false;
                for(const auto& child: children) {
                    IdeographSet anchor;
                    if(!directCandidates(*AsIdeograph(child.get()), anchor, true)) continue;
                    if(!haveAnchor || anchor.size() < bestAnchor.size()) {
                        bestAnchor = std::move(anchor);
                        haveAnchor = true;
                    }
                }
                if(!haveAnchor) return false;
                output.insert(bestAnchor.begin(), bestAnchor.end());
                if(output.size() >= _idsDB.size() - _idsDB.size() / 4) return false;
            }
        }
        return true;
    };

    auto termCandidates = [&](IDS* term, IdeographSet& output) {
        if(IsIdeograph(term)) return glyphCandidates(*AsIdeograph(term), output);
        if(!IsSearchExpression(term)) return false;
        SearchExpression* any = AsSearchExpression(term);
        if(any->GetMode() != SEARCH_EXPRESSION_ANY || !any->GetExceptTerms().empty() || any->GetTerms().empty())
            return false;
        for(const auto& choice: any->GetTerms()) {
            if(!IsIdeograph(choice.get())) return false;
            IdeographSet branch;
            if(!glyphCandidates(*AsIdeograph(choice.get()), branch)) return false;
            output.insert(branch.begin(), branch.end());
        }
        return true;
    };

    SearchExpression* search = AsSearchExpression(query);
    if(search->GetMode() == SEARCH_EXPRESSION_EXCEPT || search->GetTerms().empty()) return false;
    bool haveRequiredTerm = false;
    for(const auto& term: search->GetTerms()) {
        IdeographSet termMatches;
        if(!termCandidates(term.get(), termMatches)) {
            if(search->GetMode() == SEARCH_EXPRESSION_ANY) return false;
            continue;
        }
        if(search->GetMode() == SEARCH_EXPRESSION_ANY) {
            candidates.insert(termMatches.begin(), termMatches.end());
        } else if(!haveRequiredTerm || termMatches.size() < candidates.size()) {
            candidates = std::move(termMatches);
        }
        haveRequiredTerm = true;
    }
    return haveRequiredTerm;
}
