#include "ids4c/ids4c.h"

#include <limits>
#include <unordered_map>

const char*  IDCchar[] = {"<invalidIDC>", "⿰", "⿱", "⿲", "⿳", "⿴", "⿵", "⿶", "⿷", "⿼", "⿸", "⿹", "⿺", "⿽",
    "⿻", "⿾", "⿿", "㇯", "〾", "▥", "▤", "🔄"};
const size_t IDCargs[] = {0, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 2, 1, SIZE_MAX, SIZE_MAX, 3};

IDStype IDS::GetType() {
    return _type;
}

std::unique_ptr<IDS> IDS::Clone() const {
    return std::unique_ptr<IDS>(DeepCopy());
}

Ideograph::Ideograph(uint32_t code, uint32_t vs, std::string suffix) {
    _type = IDS_IDEOGRAPH;
    if(code <= 0xD7FF || (code >= 0xE000 && code <= 0x10FFFF)) _ideo = code;
    if((vs >= 0xFE00 && vs <= 0xFE0F) || (vs >= 0xE0100 && vs <= 0xE01EF)) _vs = vs;
    // 单独的点后缀表示基础字形，不应形成独立的字形键。
    _suffix = suffix == "." ? "" : std::move(suffix);
}

Ideograph::Ideograph(std::string glyphName) {
    _type = IDS_IDEOGRAPH;
    if(glyphName.find_first_not_of(VAILD_ASCII_CHAR) == std::string::npos) {
        // abstract glyph name
        std::string::size_type dot = glyphName.find(".");
        if(dot == std::string::npos)
            _abstractName = glyphName;
        else
            _abstractName = glyphName.substr(0, dot), _suffix = glyphName.substr(dot + 1);
    } else {
        // ideograph string(UTF-8)
        uint32_t       cp = -1, vs = -1;
        std::u32string codepoint = utf8::utf8to32(glyphName);
        if(codepoint.length() >= 1) cp = codepoint[0];
        if(codepoint.length() >= 2) vs = codepoint[1];
        if(cp <= 0xD7FF || (cp >= 0xE000 && cp <= 0x10FFFF)) {
            _ideo = cp;
            if((vs >= 0xFE00 && vs <= 0xFE0F) || (vs >= 0xE0100 && vs <= 0xE01EF)) _vs = vs;
            auto pGn = glyphName.data();
            if(_vs == 0 && codepoint.length() > 1) {
                utf8::advance(pGn, 1, glyphName.data() + glyphName.length());
                _suffix = glyphName.substr(pGn - glyphName.data());
            } else if(_vs != 0 && codepoint.length() > 2) {
                utf8::advance(pGn, 2, glyphName.data() + glyphName.length());
                _suffix = glyphName.substr(pGn - glyphName.data());
            }
        }
    }

    // 统一基础字形和单独点后缀字形的键。
    if(_suffix == ".") _suffix.clear();
}

IDS* Ideograph::DeepCopy() const {
    IDS* pIDS = new Ideograph(*this);
    return pIDS;
}

uint32_t Ideograph::GetIdeo() const {
    return _ideo;
}

uint32_t Ideograph::GetVS() const {
    return _vs;
}

std::string Ideograph::GetSuffix() const {
    return _suffix;
}

std::string Ideograph::GetAbstractName() const {
    return _abstractName;
}

bool Ideograph::operator==(const Ideograph& ideo) const {
    if(_ideo == 0 && ideo._ideo == 0)
        return _abstractName == ideo._abstractName;
    else if(_ideo != 0 && ideo._ideo != 0)
        return _ideo == ideo._ideo && _vs == ideo._vs && _suffix == ideo._suffix;
    else
        return false;
}

std::string Ideograph::toString() const {
    if(_ideo == 0)
        return "{" + _abstractName + "}";
    else {
        std::u32string outChar = {_ideo};
        if(_vs != 0) outChar.append({_vs});
        return utf8::utf32to8(outChar).append(_suffix);
    }
}

bool Ideograph::inUnicode() const {
    return _ideo != 0;
}

bool Ideograph::inPUA() const {
    return (_ideo >= 0xE000 && _ideo <= 0xF8FF) || (_ideo >= 0xF0000 && _ideo <= 0xFFFFD) ||
        (_ideo >= 0x100000 && _ideo <= 0x10FFFD);
}

bool IdeographCmp(Ideograph x, Ideograph y) {
    if(x._ideo == y._ideo) {
        if(x._ideo == 0) return x._abstractName < y._abstractName;
        if(x._vs == y._vs)
            return x._suffix < y._suffix;
        else
            return x._vs < y._vs;
    } else
        return x._ideo < y._ideo;
}

Stroke::Stroke(
    std::vector<Stroke_Data> stroke, std::vector<size_t> breakPos, std::vector<Stroke_CrossData> cross, bool enclosed) {
    _type   = IDS_STROKE;
    _stroke = stroke, _breakPos = breakPos, _crossData = cross, _enclosed = enclosed;
}

IDS* Stroke::DeepCopy() const {
    IDS* pIDS = new Stroke(*this);
    return pIDS;
}

std::vector<Stroke_Data> Stroke::GetStroke() {
    return _stroke;
}

std::vector<Stroke_Data>& Stroke::GetStrokeRef() {
    return _stroke;
}

std::vector<size_t> Stroke::GetBreakPos() {
    return _breakPos;
}

std::vector<Stroke_CrossData> Stroke::GetCrossData() {
    return _crossData;
}

bool Stroke::isEnclosed() {
    return _enclosed;
}

bool Stroke::operator==(const Stroke& stroke) {
    return _stroke == stroke._stroke && _breakPos == stroke._breakPos && _crossData == stroke._crossData &&
        _enclosed == stroke._enclosed;
}

std::string Stroke::toString() const {
    std::string out = "#(";
    for(size_t i = 0; i < _stroke.size(); i++) {
        if(_stroke[i].neg) out += '-';
        out += _stroke[i].stroke;
        for(size_t j = 0; j < _crossData.size(); j++)
            if(_crossData[j].pos == i) out += 'x' + std::to_string(_crossData[j].cross);
        for(size_t j = 0; j < _breakPos.size(); j++)
            if(_breakPos[j] == i) out += 'b';
    }
    if(_enclosed) out += 'z';
    return out + ")";
}

bool Stroke::isBasicStroke() {
    for(auto i: _stroke)
        if(i.stroke.find_first_not_of(VAILD_ASCII_CHAR) != std::string::npos) return false;
    return true;
}

SearchParam::SearchParam(SearchParamType param) {
    _type  = IDS_SEARCHPARAM;
    _param = param;
}

SearchParam::SearchParam(uint32_t strokeMinimum, uint32_t strokeMaximum) {
    _type          = IDS_SEARCHPARAM;
    _param         = SPARAM_STROKE_COUNT;
    _strokeMinimum = strokeMinimum;
    _strokeMaximum = strokeMaximum;
}

SearchParam::SearchParam(SearchParamType param, uint32_t strokeMinimum, uint32_t strokeMaximum) {
    _type          = IDS_SEARCHPARAM;
    _param         = param;
    _strokeMinimum = strokeMinimum;
    _strokeMaximum = strokeMaximum;
}

IDS* SearchParam::DeepCopy() const {
    IDS* pIDS = new SearchParam(*this);
    return pIDS;
}

SearchParamType SearchParam::GetParam() {
    return _param;
}

uint32_t SearchParam::GetStrokeMinimum() const {
    return _strokeMinimum;
}

uint32_t SearchParam::GetStrokeMaximum() const {
    return _strokeMaximum;
}

bool SearchParam::operator==(const SearchParam& sparam) {
    return _param == sparam._param && _strokeMinimum == sparam._strokeMinimum &&
        _strokeMaximum == sparam._strokeMaximum;
}

bool SearchParam::operator==(const SearchParamType& param) {
    return _param == param;
}

std::string SearchParam::toString() const {
    switch(_param) {
    case SPARAM_QUESTIONMARK:         return u8"\u2B1A";
    case SPARAM_STROKE_COUNT:
    case SPARAM_RESIDUE_STROKE_COUNT: {
        const std::string prefix = _param == SPARAM_STROKE_COUNT ? "<stroke=" : "<residue=";
        if(_strokeMinimum == 0) return prefix + "-" + std::to_string(_strokeMaximum) + ">";
        if(_strokeMaximum == std::numeric_limits<uint32_t>::max())
            return prefix + std::to_string(_strokeMinimum) + "->";
        if(_strokeMinimum == _strokeMaximum) return prefix + std::to_string(_strokeMinimum) + ">";
        return prefix + std::to_string(_strokeMinimum) + "-" + std::to_string(_strokeMaximum) + ">";
    }
    default: return "<invalidSearchParam>";
    }
}

IDSVariable::IDSVariable(std::string name): _name(std::move(name)) {
    _type = IDS_VARIABLE;
}

IDS* IDSVariable::DeepCopy() const {
    return new IDSVariable(*this);
}

std::string IDSVariable::toString() const {
    return "<var=" + _name + ">";
}

const std::string& IDSVariable::GetName() const {
    return _name;
}

bool IDSVariable::operator==(const IDSVariable& variable) const {
    return _name == variable._name;
}
SearchExpression::SearchExpression(IDSOwnerList terms, SearchExpressionMode mode, IDSOwnerList exceptTerms):
    _terms(std::move(terms)),
    _exceptTerms(std::move(exceptTerms)),
    _mode(mode) {
    _type = IDS_SEARCH;
}

IDS* SearchExpression::DeepCopy() const {
    IDSOwnerList terms;
    IDSOwnerList exceptTerms;
    terms.reserve(_terms.size());
    exceptTerms.reserve(_exceptTerms.size());
    for(const auto& term: _terms)
        terms.push_back(term->Clone());
    for(const auto& term: _exceptTerms)
        exceptTerms.push_back(term->Clone());
    return new SearchExpression(std::move(terms), _mode, std::move(exceptTerms));
}

static bool SearchTermNeedsQuotes(const std::string& term) {
    return term.find(',') != std::string::npos;
}

std::string SearchExpression::toString() const {
    std::string output =
        _mode == SEARCH_EXPRESSION_ANY ? "<any=" : (_mode == SEARCH_EXPRESSION_EXCEPT ? "<except=" : "<search=");
    for(size_t index = 0; index < _terms.size(); index++) {
        const std::string term = _terms[index]->toString();
        if(SearchTermNeedsQuotes(term)) output += '"';
        output += term;
        if(SearchTermNeedsQuotes(term)) output += '"';
        if(index + 1 != _terms.size()) output += ",";
    }
    if(!_exceptTerms.empty()) {
        if(!_terms.empty()) output += ",";
        output += "<except=";
        for(size_t index = 0; index < _exceptTerms.size(); index++) {
            const std::string term = _exceptTerms[index]->toString();
            if(SearchTermNeedsQuotes(term)) output += '"';
            output += term;
            if(SearchTermNeedsQuotes(term)) output += '"';
            if(index + 1 != _exceptTerms.size()) output += ",";
        }
        output += ">";
    }
    return output + ">";
}
const IDSOwnerList& SearchExpression::GetTerms() const {
    return _terms;
}

const IDSOwnerList& SearchExpression::GetExceptTerms() const {
    return _exceptTerms;
}

SearchExpressionMode SearchExpression::GetMode() const {
    return _mode;
}

bool SearchExpression::operator==(const SearchExpression& search) const {
    if(_mode != search._mode) return false;
    if(_terms.size() != search._terms.size() || _exceptTerms.size() != search._exceptTerms.size()) return false;
    for(size_t index = 0; index < _terms.size(); index++)
        if(!IDSequal(_terms[index].get(), search._terms[index].get())) return false;
    for(size_t index = 0; index < _exceptTerms.size(); index++)
        if(!IDSequal(_exceptTerms[index].get(), search._exceptTerms[index].get())) return false;
    return true;
}

Pattern::Pattern(IDCtype idc, std::vector<IDS*> pids, size_t preferSplitPoint, int* overlayRange,
    std::vector<std::string> overlayType, int optionalInt, std::vector<HVOriginRange> hvOriginRanges) {
    _type = IDS_PATTERN;
    _hvOriginRanges = std::move(hvOriginRanges);
    _idc = idc, _optionalInt = optionalInt;
    if(IDCargs[_idc] != SIZE_MAX) _pids.reserve(IDCargs[_idc]);
    for(size_t i = 0; i < IDCargs[_idc] && i < pids.size(); i++)
        _pids.push_back(pids[i]->Clone());
    if(_idc == IDC_HORIZONAL_ARRANGE || _idc == IDC_VERTICAL_ARRANGE) _preferSplitPoint = preferSplitPoint;
    if(_idc == IDC_OVERLAY) {
        if(overlayRange != nullptr) _overlayRange[0] = overlayRange[0], _overlayRange[1] = overlayRange[1];
        _overlayType = overlayType;
    }
}

Pattern::Pattern(const Pattern& pattern) {
    _type = IDS_PATTERN;
    _idc = pattern._idc, _optionalInt = pattern._optionalInt;
    _hvOriginRanges = pattern._hvOriginRanges;
    _preferSplitPoint = pattern._preferSplitPoint;
    _overlayType      = pattern._overlayType;
    for(const auto& i: pattern._pids)
        _pids.push_back(i->Clone());
    _overlayRange[0] = pattern._overlayRange[0], _overlayRange[1] = pattern._overlayRange[1];
}

Pattern::Pattern(Pattern&& pattern) noexcept:
    _idc(pattern._idc),
    _pids(std::move(pattern._pids)),
    _preferSplitPoint(pattern._preferSplitPoint),
    _overlayType(std::move(pattern._overlayType)),
    _optionalInt(pattern._optionalInt),
    _hvOriginRanges(std::move(pattern._hvOriginRanges)) {
    _overlayRange[0] = pattern._overlayRange[0];
    _overlayRange[1] = pattern._overlayRange[1];
}

Pattern& Pattern::operator=(const Pattern& pattern) {
    if(this == &pattern) return *this;
    _pids.clear();
    _type             = IDS_PATTERN;
    _idc              = pattern._idc;
    _optionalInt      = pattern._optionalInt;
    _hvOriginRanges   = pattern._hvOriginRanges;
    _preferSplitPoint = pattern._preferSplitPoint;
    _overlayType      = pattern._overlayType;
    _overlayRange[0]  = pattern._overlayRange[0];
    _overlayRange[1]  = pattern._overlayRange[1];
    for(const auto& i: pattern._pids)
        _pids.push_back(i->Clone());
    return *this;
}

Pattern& Pattern::operator=(Pattern&& pattern) noexcept {
    if(this == &pattern) return *this;
    _type             = pattern._type;
    _idc              = pattern._idc;
    _pids             = std::move(pattern._pids);
    _preferSplitPoint = pattern._preferSplitPoint;
    _overlayRange[0]  = pattern._overlayRange[0];
    _overlayRange[1]  = pattern._overlayRange[1];
    _overlayType      = std::move(pattern._overlayType);
    _optionalInt      = pattern._optionalInt;
    _hvOriginRanges   = std::move(pattern._hvOriginRanges);
    return *this;
}

Pattern::~Pattern() = default;

IDS* Pattern::DeepCopy() const {
    IDS* pIDS = new Pattern(*this);
    return pIDS;
}

std::string Pattern::toString() const {
    std::string out(IDCchar[_idc]);
    if(_idc == IDC_OVERLAY && !(_overlayRange[0] == 0 && _overlayRange[1] == 0 && _overlayType.empty())) {
        out += "[";
        if(!(_overlayRange[0] == 0 && _overlayRange[1] == 0)) {
            if(_overlayRange[0] != 0) out += std::to_string(_overlayRange[0]);
            out += ":";
            if(_overlayRange[1] != 0) out += std::to_string(_overlayRange[1]);
        }
        if((_overlayRange[0] != 0 || _overlayRange[0] != 0) && !_overlayType.empty()) out += "|";
        for(size_t i = 0; i < _overlayType.size(); i++) {
            out += _overlayType[i];
            if(i != _overlayType.size() - 1) out += ",";
        }
        out += "]";
    }
    const bool isArrange = _idc == IDC_HORIZONAL_ARRANGE || _idc == IDC_VERTICAL_ARRANGE;
    if(isArrange && !_hvOriginRanges.empty()) {
        out += "[";
        for(size_t index = 0; index < _hvOriginRanges.size(); index++) {
            if(index != 0) out += ",";
            out += _hvOriginRanges[index].glyph + "=" + std::to_string(_hvOriginRanges[index].first) + ":" +
                std::to_string(_hvOriginRanges[index].last);
        }
        out += "]";
    }
    if(isArrange) out += "(";
    if(_idc != IDC_OVERLAY && _optionalInt != 0) out += "[" + std::to_string(_optionalInt) + "]";
    for(size_t i = 0; i < _pids.size(); i++) {
        out += _pids[i]->toString();
        if(isArrange && i == _preferSplitPoint) out += "|";
    }
    if(isArrange) out += ")";
    return out;
}

IDCtype Pattern::GetIDC() const {
    return _idc;
}

std::vector<IDS*> Pattern::GetpIDS() const {
    std::vector<IDS*> out;
    out.reserve(_pids.size());
    for(const auto& i: _pids)
        out.push_back(i.get());
    return out;
}

IDSOwnerList& Pattern::GetpIDSRef() {
    return _pids;
}

const IDSOwnerList& Pattern::GetpIDSRef() const {
    return _pids;
}

size_t Pattern::GetPreferSplitPoint() const {
    return _preferSplitPoint;
}

void Pattern::GetOverlayRange(int* output) const {
    output[0] = _overlayRange[0], output[1] = _overlayRange[1];
}

std::vector<std::string> Pattern::GetOverlayType() const {
    return _overlayType;
}

int Pattern::GetOptionalInt() const {
    return _optionalInt;
}

bool Pattern::operator==(const Pattern& pattern) {
    if(_idc != pattern._idc || _pids.size() != pattern._pids.size()) return false;
    if(_hvOriginRanges.size() != pattern._hvOriginRanges.size()) return false;
    for(size_t index = 0; index < _hvOriginRanges.size(); index++)
        if(_hvOriginRanges[index].glyph != pattern._hvOriginRanges[index].glyph ||
            _hvOriginRanges[index].first != pattern._hvOriginRanges[index].first ||
            _hvOriginRanges[index].last != pattern._hvOriginRanges[index].last)
            return false;
    for(size_t i = 0; i < _pids.size(); i++) {
        if(_pids[i]->GetType() != pattern._pids[i]->GetType()) return false;
        if(_pids[i]->GetType() == IDS_IDEOGRAPH &&
            !(*(Ideograph*)_pids[i].get() == *(Ideograph*)pattern._pids[i].get()))
            return false;
        if(_pids[i]->GetType() == IDS_STROKE && !(*(Stroke*)_pids[i].get() == *(Stroke*)pattern._pids[i].get()))
            return false;
        if(_pids[i]->GetType() == IDS_SEARCHPARAM &&
            !(*(SearchParam*)_pids[i].get() == *(SearchParam*)pattern._pids[i].get()))
            return false;
        if(_pids[i]->GetType() == IDS_SEARCH &&
            !(*(SearchExpression*)_pids[i].get() == *(SearchExpression*)pattern._pids[i].get()))
            return false;
        if(_pids[i]->GetType() == IDS_VARIABLE &&
            !(*(IDSVariable*)_pids[i].get() == *(IDSVariable*)pattern._pids[i].get()))
            return false;
        if(_pids[i]->GetType() == IDS_PATTERN && !(*(Pattern*)_pids[i].get() == *(Pattern*)pattern._pids[i].get()))
            return false;
    }
    if(_idc == IDC_OVERLAY)
        return _overlayRange[0] == pattern._overlayRange[0] && _overlayRange[1] == pattern._overlayRange[1] &&
            _overlayType == pattern._overlayType;
    else {
        if(_optionalInt != pattern._optionalInt) return false;
        if((_idc == IDC_HORIZONAL_ARRANGE || _idc == IDC_VERTICAL_ARRANGE) &&
            _preferSplitPoint != pattern._preferSplitPoint)
            return false;
        return true;
    }
}

_ParsingToken::_ParsingToken(std::string token) {
    _type  = IDS_PARSINGTOKEN;
    _token = token;
}

IDS* _ParsingToken::DeepCopy() const {
    IDS* pIDS = new _ParsingToken(*this);
    return pIDS;
}

std::string _ParsingToken::toString() const {
    return "<token=" + _token + ">";
}

std::string _ParsingToken::GetToken() {
    return _token;
}

bool _ParsingToken::operator==(const _ParsingToken& parseToken) {
    return _token == parseToken._token;
}

IDCtype IDCchar2type(std::string idc) {
    for(size_t i = 0; i < (sizeof(IDCchar) / sizeof(char*)); i++)
        if(idc == IDCchar[i]) return IDCtype(i);
    return IDCtype(IDC_UNKNOWN);
}

const char* IDCtype2char(IDCtype idc) {
    const size_t index = static_cast<size_t>(idc);
    if(index >= sizeof(IDCchar) / sizeof(IDCchar[0])) return IDCchar[0];
    return IDCchar[index];
}
std::vector<std::string> StringSplit(std::string str, char32_t split) {
    std::u32string           strU32 = utf8::utf8to32(str), buffer = U"";
    std::vector<std::string> out;
    for(size_t i = 0; i < strU32.length() + 1; i++)
        if(i == strU32.length() || strU32[i] == split) {
            out.push_back(utf8::utf32to8(buffer));
            buffer = U"";
        } else
            buffer += strU32[i];
    return out;
}

static std::vector<IDS*> BorrowIDS(const IDSOwnerList& ids) {
    std::vector<IDS*> out;
    out.reserve(ids.size());
    for(const auto& i: ids)
        out.push_back(i.get());
    return out;
}

#define RETURN_NULLPTR_IF_STACK_IS_EMPTY(__stack, __tokenIndex)                                        \
    {                                                                                                  \
        if(__stack.empty()) {                                                                          \
            SetIDSParseError(parseError, idsU32, tokenPositions[__tokenIndex], "missing IDS operand"); \
            return IDSOwner();                                                                         \
        }                                                                                              \
    }

static bool ParseStrokeCountBound(const std::string& text, uint32_t& value) {
    if(text.empty()) return false;
    try {
        size_t              parsedLength = 0;
        const unsigned long parsedValue  = std::stoul(text, &parsedLength);
        if(parsedLength != text.length() || parsedValue > std::numeric_limits<uint32_t>::max()) return false;
        value = static_cast<uint32_t>(parsedValue);
        return true;
    } catch(const std::exception&) {
        return false;
    }
}

static bool ParseStrokeCountToken(
    const std::string& token, SearchParamType& param, uint32_t& minimum, uint32_t& maximum) {
    static const std::string strokePrefix  = "<stroke=";
    static const std::string residuePrefix = "<residue=";
    std::string              prefix;
    if(token.compare(0, strokePrefix.length(), strokePrefix) == 0) {
        param  = SPARAM_STROKE_COUNT;
        prefix = strokePrefix;
    } else if(token.compare(0, residuePrefix.length(), residuePrefix) == 0) {
        param  = SPARAM_RESIDUE_STROKE_COUNT;
        prefix = residuePrefix;
    } else
        return false;
    if(token.length() <= prefix.length() + 1 || token[token.length() - 1] != '>') return false;

    const std::string value = token.substr(prefix.length(), token.length() - prefix.length() - 1);
    const size_t      delta = value.find('~');
    if(delta != std::string::npos) {
        // <stroke=21~1> and <residue=12~2> are normalized during parsing.
        // The resulting range is inclusive and clamps the lower bound at zero.
        if(delta == 0 || value.find('~', delta + 1) != std::string::npos || value.find('-') != std::string::npos)
            return false;
        uint32_t center = 0;
        uint32_t radius = 0;
        if(!ParseStrokeCountBound(value.substr(0, delta), center) ||
            !ParseStrokeCountBound(value.substr(delta + 1), radius))
            return false;
        minimum = center > radius ? center - radius : 0;
        maximum = center > std::numeric_limits<uint32_t>::max() - radius ? std::numeric_limits<uint32_t>::max()
                                                                         : center + radius;
        return true;
    }

    const size_t dash = value.find('-');
    if(dash == std::string::npos) {
        if(!ParseStrokeCountBound(value, minimum)) return false;
        maximum = minimum;
    } else if(dash == 0) {
        minimum = 0;
        if(!ParseStrokeCountBound(value.substr(1), maximum)) return false;
    } else if(dash == value.length() - 1) {
        if(!ParseStrokeCountBound(value.substr(0, dash), minimum)) return false;
        maximum = std::numeric_limits<uint32_t>::max();
    } else {
        if(value.find('-', dash + 1) != std::string::npos || !ParseStrokeCountBound(value.substr(0, dash), minimum) ||
            !ParseStrokeCountBound(value.substr(dash + 1), maximum))
            return false;
    }
    return minimum <= maximum;
}
static int HexValue(char character) {
    if(character >= '0' && character <= '9') return character - '0';
    if(character >= 'a' && character <= 'f') return character - 'a' + 10;
    if(character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static std::string DescribeParseCharacter(char32_t character) {
    if(character == 0) return "end of expression";
    return "'" + utf8::utf32to8(std::u32string(1, character)) + "'";
}

static void SetIDSParseError(
    IDSParseError* error, const std::u32string& input, size_t position, const std::string& message) {
    if(error == nullptr || !error->message.empty()) return;
    error->hasPosition = position < input.length();
    error->position    = position;
    error->character   = error->hasPosition ? input[position] : 0;
    error->message     = message;
}

static char32_t ExpectedClosingDelimiter(char32_t opening) {
    switch(opening) {
    case U'(': return U')';
    case U'[': return U']';
    case U'{': return U'}';
    case U'<': return U'>';
    default:   return 0;
    }
}

static bool ExpandUnicodeEscapes(const std::string& input, std::string& output) {
    output.clear();
    output.reserve(input.length());
    for(size_t index = 0; index < input.length();) {
        if(input[index] != '\\') {
            output += input[index++];
            continue;
        }
        if(index + 2 > input.length()) return false;
        const char   marker     = input[index + 1];
        const size_t digitCount = marker == 'u' ? 4 : (marker == 'U' ? 8 : 0);
        if(digitCount == 0 || index + 2 + digitCount > input.length()) return false;

        uint32_t codePoint = 0;
        for(size_t digit = 0; digit < digitCount; digit++) {
            const int value = HexValue(input[index + 2 + digit]);
            if(value < 0) return false;
            codePoint = (codePoint << 4) | static_cast<uint32_t>(value);
        }
        if(codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return false;
        output += utf8::utf32to8(std::u32string(1, static_cast<char32_t>(codePoint)));
        index  += digitCount + 2;
    }
    return true;
}

static bool IsSearchWhitespace(char32_t character) {
    return character == U' ' || character == U'\t' || character == U'\n' || character == U'\r';
}

static bool IsSearchExpressionStart(const std::u32string& input, size_t start) {
    return input.compare(start, 8, U"<search=") == 0 || input.compare(start, 5, U"<any=") == 0;
}

static bool IsExceptExpressionStart(const std::u32string& input, size_t start) {
    return input.compare(start, 8, U"<except=") == 0;
}

static bool GetSearchExpressionPrefix(
    const std::u32string& input, size_t start, SearchExpressionMode& mode, size_t& prefixLength) {
    if(input.compare(start, 8, U"<search=") == 0) {
        mode         = SEARCH_EXPRESSION_ALL;
        prefixLength = 8;
        return true;
    }
    if(input.compare(start, 5, U"<any=") == 0) {
        mode         = SEARCH_EXPRESSION_ANY;
        prefixLength = 5;
        return true;
    }
    return false;
}

static bool ContainsResidueCondition(IDS* ids) {
    if(ids == nullptr) return false;
    if(ids->GetType() == IDS_SEARCHPARAM)
        return static_cast<SearchParam*>(ids)->GetParam() == SPARAM_RESIDUE_STROKE_COUNT;
    if(ids->GetType() == IDS_SEARCH) {
        SearchExpression* search = static_cast<SearchExpression*>(ids);
        for(const auto& term: search->GetTerms())
            if(ContainsResidueCondition(term.get())) return true;
        for(const auto& term: search->GetExceptTerms())
            if(ContainsResidueCondition(term.get())) return true;
    }
    if(ids->GetType() == IDS_PATTERN) {
        for(const auto& child: static_cast<Pattern*>(ids)->GetpIDSRef())
            if(ContainsResidueCondition(child.get())) return true;
    }
    return false;
}

static bool ParseSearchExpressionAt(
    const std::u32string& input, size_t start, IDSOwner& expression, size_t& end, IDSParseError* error);

static bool ParseExceptExpressionAt(
    const std::u32string& input, size_t start, IDSOwnerList& terms, size_t& end, IDSParseError* error) {
    auto fail = [&](size_t position, const std::string& message) {
        SetIDSParseError(error, input, position, message);
        return false;
    };
    if(!IsExceptExpressionStart(input, start)) return fail(start, "expected '<except='");

    // <except=...> shares <search=...> term syntax. Both prefixes are eight code points long.
    std::u32string rewritten = U"<search=";
    rewritten               += input.substr(start + 8);
    IDSOwner parsed;
    size_t   parsedEnd = 0;
    if(!ParseSearchExpressionAt(rewritten, 0, parsed, parsedEnd, error) || parsed == nullptr ||
        parsed->GetType() != IDS_SEARCH)
        return false;

    SearchExpression* search = static_cast<SearchExpression*>(parsed.get());
    if(search->GetMode() != SEARCH_EXPRESSION_ALL || !search->GetExceptTerms().empty())
        return fail(start, "<except=...> cannot contain another except expression");
    terms.reserve(search->GetTerms().size());
    for(const auto& term: search->GetTerms())
        terms.push_back(term->Clone());
    end = start + parsedEnd;
    return !terms.empty() ? true : fail(start, "<except=...> requires at least one IDS term");
}

static bool ParseSearchExpressionAt(
    const std::u32string& input, size_t start, IDSOwner& expression, size_t& end, IDSParseError* error) {
    auto fail = [&](size_t position, const std::string& message) {
        SetIDSParseError(error, input, position, message);
        return false;
    };

    SearchExpressionMode mode         = SEARCH_EXPRESSION_ALL;
    size_t               prefixLength = 0;
    if(!GetSearchExpressionPrefix(input, start, mode, prefixLength))
        return fail(start, "expected '<search=' or '<any='");

    IDSOwnerList terms;
    IDSOwnerList exceptTerms;
    bool         hasExcept = false;
    size_t       index     = start + prefixLength;
    while(true) {
        while(index < input.length() && IsSearchWhitespace(input[index]))
            index++;
        if(index >= input.length()) return fail(index, "unexpected end of search expression");

        if(IsExceptExpressionStart(input, index)) {
            if(hasExcept) return fail(index, "duplicate <except=...> in search expression");
            if(!ParseExceptExpressionAt(input, index, exceptTerms, index, error)) return false;
            if(mode == SEARCH_EXPRESSION_ANY)
                for(const auto& term: exceptTerms)
                    if(ContainsResidueCondition(term.get()))
                        return fail(index, "<residue=...> is not allowed inside <any=...>");
            hasExcept = true;
        } else {
            size_t         termStart = index;
            size_t         termEnd   = index;
            const char32_t quote     = input[index];
            if(quote == (char32_t)0x60 || quote == U'\'' || quote == U'"') {
                termStart = ++index;
                while(index < input.length() && input[index] != quote) {
                    if(IsSearchExpressionStart(input, index)) {
                        IDSOwner nested;
                        size_t   nestedEnd = 0;
                        if(!ParseSearchExpressionAt(input, index, nested, nestedEnd, error)) return false;
                        index = nestedEnd;
                    } else
                        index++;
                }
                if(index >= input.length()) return fail(termStart, "unterminated quoted search term");
                termEnd = index++;
            } else {
                size_t parenthesisDepth = 0;
                size_t bracketDepth     = 0;
                size_t braceDepth       = 0;
                size_t angleDepth       = 0;
                size_t parenthesisStart = SIZE_MAX;
                size_t bracketStart     = SIZE_MAX;
                size_t braceStart       = SIZE_MAX;
                size_t angleStart       = SIZE_MAX;
                while(index < input.length()) {
                    const char32_t character = input[index];
                    if(IsSearchExpressionStart(input, index)) {
                        IDSOwner nested;
                        size_t   nestedEnd = 0;
                        if(!ParseSearchExpressionAt(input, index, nested, nestedEnd, error)) return false;
                        index = nestedEnd;
                        continue;
                    }
                    if(character == U'<') {
                        if(angleDepth == 0) angleStart = index;
                        angleDepth++;
                    } else if(character == U'>') {
                        if(angleDepth != 0)
                            angleDepth--;
                        else if(parenthesisDepth == 0 && bracketDepth == 0 && braceDepth == 0)
                            break;
                        else
                            return fail(index, "unexpected " + DescribeParseCharacter(character));
                    } else if(character == U'(') {
                        if(parenthesisDepth == 0) parenthesisStart = index;
                        parenthesisDepth++;
                    } else if(character == U')') {
                        if(parenthesisDepth == 0) return fail(index, "unexpected " + DescribeParseCharacter(character));
                        parenthesisDepth--;
                    } else if(character == U'[') {
                        if(bracketDepth == 0) bracketStart = index;
                        bracketDepth++;
                    } else if(character == U']') {
                        if(bracketDepth == 0) return fail(index, "unexpected " + DescribeParseCharacter(character));
                        bracketDepth--;
                    } else if(character == U'{') {
                        if(braceDepth == 0) braceStart = index;
                        braceDepth++;
                    } else if(character == U'}') {
                        if(braceDepth == 0) return fail(index, "unexpected " + DescribeParseCharacter(character));
                        braceDepth--;
                    } else if(character == U',' && parenthesisDepth == 0 && bracketDepth == 0 && braceDepth == 0 &&
                        angleDepth == 0) {
                        break;
                    }
                    index++;
                }
                if(parenthesisDepth != 0) return fail(parenthesisStart, "unclosed '(' in search term");
                if(bracketDepth != 0) return fail(bracketStart, "unclosed '[' in search term");
                if(braceDepth != 0) return fail(braceStart, "unclosed '{' in search term");
                if(angleDepth != 0) return fail(angleStart, "unclosed '<' in search term");
                termEnd = index;
            }

            while(termStart < termEnd && IsSearchWhitespace(input[termStart]))
                termStart++;
            while(termEnd > termStart && IsSearchWhitespace(input[termEnd - 1]))
                termEnd--;
            if(termStart == termEnd) return fail(termStart, "empty search term");

            IDSParseError termError;
            IDSOwner term = ParseIDSOwned(utf8::utf32to8(input.substr(termStart, termEnd - termStart)), &termError);
            if(term == nullptr) {
                if(!termError.message.empty()) {
                    const size_t termPosition = termError.hasPosition ? termStart + termError.position : termEnd;
                    SetIDSParseError(error, input, termPosition, termError.message);
                } else
                    fail(termStart, "invalid IDS term");
                return false;
            }
            if(mode == SEARCH_EXPRESSION_ANY && ContainsResidueCondition(term.get()))
                return fail(termStart, "<residue=...> is not allowed inside <any=...>");
            terms.push_back(std::move(term));
        }

        while(index < input.length() && IsSearchWhitespace(input[index]))
            index++;
        if(index >= input.length()) return fail(index, "unexpected end of search expression");
        if(input[index] == U',') {
            index++;
            continue;
        }
        if(input[index] != U'>') return fail(index, "expected ',' or '>'");
        if(terms.empty()) return fail(index, "search expression requires at least one IDS term");
        expression.reset(new SearchExpression(std::move(terms), mode, std::move(exceptTerms)));
        end = index + 1;
        return true;
    }
}
static bool IsAsciiUppercase(char32_t character) {
    return character >= U'A' && character <= U'Z';
}

static bool IsAsciiLowercase(char32_t character) {
    return character >= U'a' && character <= U'z';
}

static bool ParseVariableToken(const std::string& token, std::string& name) {
    static const std::string prefix = "<var=";
    if(token.length() <= prefix.length() + 1 || token.compare(0, prefix.length(), prefix) != 0 || token.back() != '>')
        return false;

    name = token.substr(prefix.length(), token.length() - prefix.length() - 1);
    for(const unsigned char character: name)
        if(!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_'))
            return false;
    return true;
}

static bool ParseHVOriginRangeToken(const std::string& token, std::vector<HVOriginRange>& ranges) {
    if(token.size() < 2 || token.front() != '[' || token.back() != ']') return false;
    const std::string content = token.substr(1, token.size() - 2);
    if(content.empty()) return true;
    for(const std::string& item: StringSplit(content, U',')) {
        const size_t equal = item.find('=');
        const size_t colon = equal == std::string::npos ? std::string::npos : item.find(':', equal + 1);
        if(equal == std::string::npos || colon == std::string::npos || equal == 0 || colon <= equal + 1 ||
            colon + 1 >= item.size())
            return false;
        HVOriginRange range;
        range.glyph = item.substr(0, equal);
        try {
            size_t firstConsumed = 0;
            size_t lastConsumed  = 0;
            const unsigned long long first = std::stoull(item.substr(equal + 1, colon - equal - 1), &firstConsumed);
            const unsigned long long last  = std::stoull(item.substr(colon + 1), &lastConsumed);
            if(firstConsumed != colon - equal - 1 || lastConsumed != item.size() - colon - 1 ||
                first > last || first > std::numeric_limits<size_t>::max() ||
                last > std::numeric_limits<size_t>::max())
                return false;
            range.first = static_cast<size_t>(first);
            range.last  = static_cast<size_t>(last);
        } catch(const std::exception&) {
            return false;
        }
        ranges.push_back(std::move(range));
    }
    return true;
}
IDSOwner ParseIDSOwned(std::string ids) {
    return ParseIDSOwned(std::move(ids), nullptr);
}

IDSOwner ParseIDSOwned(std::string ids, IDSParseError* parseError) {
    if(parseError != nullptr) *parseError = IDSParseError();

    std::string expandedIds;
    if(!ExpandUnicodeEscapes(ids, expandedIds)) {
        std::u32string original;
        try {
            original = utf8::utf8to32(ids);
        } catch(const std::exception&) {}
        const size_t position = original.find(U'\\');
        SetIDSParseError(
            parseError, original, position == std::u32string::npos ? 0 : position, "invalid Unicode escape");
        return nullptr;
    }
    ids = std::move(expandedIds);

    // Parenthesis check
    std::u32string idsU32;
    try {
        idsU32 = utf8::utf8to32(ids);
    } catch(const std::exception&) {
        SetIDSParseError(parseError, std::u32string(), 0, "invalid UTF-8 sequence");
        return nullptr;
    }
    std::stack<std::pair<char32_t, size_t>> parenStack;
    for(size_t position = 0; position < idsU32.length(); position++) {
        const char32_t character = idsU32[position];
        if(character == U'(' || character == U'[' || character == U'{' || character == U'<')
            parenStack.push(std::make_pair(character, position));
        else if(character == U')' || character == U']' || character == U'}' || character == U'>') {
            if(parenStack.empty()) {
                SetIDSParseError(parseError, idsU32, position, "unexpected " + DescribeParseCharacter(character));
                return nullptr;
            }
            const char32_t expected = ExpectedClosingDelimiter(parenStack.top().first);
            if(expected != character) {
                SetIDSParseError(parseError, idsU32, position,
                    "unexpected " + DescribeParseCharacter(character) + " (expected " +
                        DescribeParseCharacter(expected) + ")");
                return nullptr;
            }
            parenStack.pop();
        }
    }
    if(!parenStack.empty()) {
        SetIDSParseError(
            parseError, idsU32, parenStack.top().second, "unclosed " + DescribeParseCharacter(parenStack.top().first));
        return nullptr;
    }
    // Tokenize
    std::vector<std::string>                  tokens;
    std::vector<size_t>                       tokenPositions;
    std::unordered_map<std::string, IDSOwner> searchExpressions;
    size_t                                    searchExpressionIndex = 0;
    std::u32string                            strBuffer             = U"";
    bool                                      YBdisrepeat           = false;
    std::string                               rootUniqueSeparator;
    bool   strokeMode = false, strokeUpper = false, strokeNeg = false, strokeCross = false;
    bool   bracketMode  = false;
    size_t strokeDepth  = 0;
    size_t curlyPos     = SIZE_MAX;
    size_t bracketStart = SIZE_MAX;
    auto   pushToken    = [&](std::string token, size_t position) {
        tokens.push_back(std::move(token));
        tokenPositions.push_back(position);
    };
    for(size_t i = 0; i < idsU32.length(); i++) {
        // 组件搜索表达式是查询专用节点；逗号分隔的每一项都是完整 IDS。
        if(idsU32[i] == U'<' && IsSearchExpressionStart(idsU32, i)) {
            IDSOwner searchExpression;
            size_t   searchEnd = 0;
            if(!ParseSearchExpressionAt(idsU32, i, searchExpression, searchEnd, parseError)) return nullptr;
            const std::string searchToken = "{__ids4c_search_" + std::to_string(searchExpressionIndex++) + "}";
            searchExpressions.emplace(searchToken, std::move(searchExpression));
            pushToken(searchToken, i);
            i = searchEnd - 1;
        }
        // A root <except=...> excludes every glyph that satisfies its component-search terms.
        else if(idsU32[i] == U'<' && IsExceptExpressionStart(idsU32, i)) {
            if(i != 0 || !tokens.empty()) {
                SetIDSParseError(parseError, idsU32, i, "root <except=...> must be the only expression");
                return nullptr;
            }
            IDSOwnerList exceptTerms;
            size_t       exceptEnd = 0;
            if(!ParseExceptExpressionAt(idsU32, i, exceptTerms, exceptEnd, parseError)) return nullptr;
            const std::string searchToken = "{__ids4c_search_" + std::to_string(searchExpressionIndex++) + "}";
            searchExpressions.emplace(
                searchToken, IDSOwner(new SearchExpression(std::move(exceptTerms), SEARCH_EXPRESSION_EXCEPT)));
            pushToken(searchToken, i);
            i = exceptEnd - 1;
        }
        // 笔画数条件是查询专用原子节点，整体保留，不能按一般后缀拆分。
        else if(idsU32[i] == U'<' &&
            (idsU32.compare(i, 8, U"<stroke=") == 0 || idsU32.compare(i, 9, U"<residue=") == 0)) {
            const size_t tokenEnd = idsU32.find(U'>', i + 8);
            if(tokenEnd == std::u32string::npos) {
                SetIDSParseError(parseError, idsU32, i, "unclosed stroke-count condition");
                return nullptr;
            }
            pushToken(utf8::utf32to8(idsU32.substr(i, tokenEnd - i + 1)), i);
            i = tokenEnd;
        }
        // Variables stay atomic. IWDS preprocessing owns any fullwidth placeholder conversion.
        else if(idsU32[i] == U'<' && idsU32.compare(i, 5, U"<var=") == 0) {
            const size_t tokenEnd = idsU32.find(U'>', i + 5);
            if(tokenEnd == std::u32string::npos) {
                SetIDSParseError(parseError, idsU32, i, "unclosed variable expression");
                return nullptr;
            }
            pushToken(utf8::utf32to8(idsU32.substr(i, tokenEnd - i + 1)), i);
            i = tokenEnd;
        }
        // Curly at the beginning of IDS
        else if(idsU32[i] == U'{' && i == 0) {
            const size_t closing = idsU32.find(U'}', i + 1);
            if(closing != std::u32string::npos && closing + 1 < idsU32.size())
                rootUniqueSeparator = utf8::utf32to8(idsU32.substr(i, closing - i + 1));
            YBdisrepeat = true;
            continue;
        } else if(YBdisrepeat) {
            if(idsU32[i] == U'}') YBdisrepeat = false;
            continue;
        }
        // Abstract Ideograph
        else if(idsU32[i] == U'{') // Andrew West: {abstract}
            curlyPos = i + 1;
        else if(idsU32[i] == U'}') {
            if(curlyPos == SIZE_MAX) {
                SetIDSParseError(parseError, idsU32, i, "unexpected " + DescribeParseCharacter(idsU32[i]));
                return nullptr;
            }
            pushToken('{' + utf8::utf32to8(idsU32.substr(curlyPos, i - curlyPos)) + '}', curlyPos - 1);
            curlyPos = SIZE_MAX;
        } else if(curlyPos != SIZE_MAX)
            continue;
        // Stroke
        else if(strokeMode) {
            if(idsU32[i] == U'(') {
                // 笔画串内部的括号只用于分组；Stroke 结构本身不保留该层次。
                if(strokeDepth++ == 0) pushToken("(", i);
            } else if(idsU32[i] == U')') {
                if(strokeDepth == 0) {
                    SetIDSParseError(parseError, idsU32, i, "unexpected " + DescribeParseCharacter(idsU32[i]));
                    return nullptr;
                }
                if(--strokeDepth == 0) {
                    strokeMode = false;
                    pushToken(")", i);
                }
            }
            // Stroke in Letter form
            else if(IsAsciiUppercase(idsU32[i])) {
                strBuffer = idsU32[i], strokeUpper = true;
                if(i + 1 == idsU32.length() || !IsAsciiLowercase(idsU32[i + 1])) {
                    pushToken((strokeNeg ? "-" : "") + utf8::utf32to8(strBuffer), i);
                    strBuffer = U"", strokeUpper = false, strokeNeg = false;
                }
            } else if(IsAsciiLowercase(idsU32[i]) && strokeUpper) {
                strBuffer += idsU32[i];
                if(i + 1 == idsU32.length() || !IsAsciiLowercase(idsU32[i + 1])) {
                    pushToken((strokeNeg ? "-" : "") + utf8::utf32to8(strBuffer), i);
                    strBuffer = U"", strokeUpper = false, strokeNeg = false;
                }
            }
            // Stroke in Ideograph form
            else if(idsU32[i] == U'-')
                strokeNeg = true;
            else if(idsU32[i] == U'x')
                strokeCross = true, strBuffer = U"x";
            else if(strokeCross && std::isdigit(idsU32[i])) {
                strBuffer += idsU32[i];
                if(!std::isdigit(idsU32[i + 1])) {
                    pushToken(utf8::utf32to8(strBuffer), i);
                    strBuffer = U"", strokeCross = false;
                }
            }
            // z,b mode & Stroke char.
            else {
                pushToken((strokeNeg ? "-" : "") + utf8::utf32to8({idsU32[i]}), i);
                strokeNeg = false;
            }
        } else if(idsU32[i] == U'#') {
            strokeMode  = true;
            strokeDepth = 0;
            pushToken("#", i);
        }
        // Bracket (optional parameter)
        else if(bracketMode) {
            strBuffer += idsU32[i];
            if(idsU32[i] == U']') {
                pushToken(utf8::utf32to8(strBuffer), bracketStart);
                strBuffer = U"", bracketMode = false, bracketStart = SIZE_MAX;
            }
        } else if(idsU32[i] == U'[')
            strBuffer = U"[", bracketMode = true, bracketStart = i;
        // Suffix of Ideograph
        else if(idsU32[i] == U'.' || idsU32[i] == U'_' || (idsU32[i] >= U'0' && idsU32[i] <= U'9') ||
            (idsU32[i] >= U'A' && idsU32[i] <= U'Z') || (idsU32[i] >= U'a' && idsU32[i] <= U'z')) {
            if(tokens.empty()) {
                SetIDSParseError(parseError, idsU32, i, "unexpected " + DescribeParseCharacter(idsU32[i]));
                return nullptr;
            }
            tokens.back() += utf8::utf32to8({idsU32[i]});
        }
        // SVS & IVS
        else if((idsU32[i] >= 0xFE00 && idsU32[i] <= 0xFE0F) || (idsU32[i] >= 0xE0100 && idsU32[i] <= 0xE01EF)) {
            if(tokens.empty()) {
                SetIDSParseError(parseError, idsU32, i, "unexpected variation selector");
                return nullptr;
            }
            tokens.back() += utf8::utf32to8({idsU32[i]});
        }
        // IDC, (), |, Ideograph, Stroke, ...
        else
            pushToken(utf8::utf32to8({idsU32[i]}), i);
    }
    if(curlyPos != SIZE_MAX) {
        SetIDSParseError(parseError, idsU32, curlyPos - 1, "unclosed abstract glyph");
        return IDSOwner();
    }
    if(bracketMode) {
        SetIDSParseError(parseError, idsU32, bracketStart, "unclosed '['");
        return IDSOwner();
    }
    if(strokeMode || strokeDepth != 0) {
        SetIDSParseError(parseError, idsU32, idsU32.length(), "unclosed stroke expression");
        return IDSOwner();
    }

    // Parsing
    std::stack<IDSOwner> stack;
    IDSOwner             temp;
    std::string          jToken;
    for(size_t i = tokens.size() - 1; i < tokens.size(); i--) {
        IDSOwnerList pids = {};
        // IDC
        // 解析器从右向左扫描；笔画链必须先消费左括号，再收集到右括号。
        const auto searchExpression = searchExpressions.find(tokens[i]);
        if(searchExpression != searchExpressions.end())
            stack.push(searchExpression->second->Clone());
        else if(tokens[i] == "#") {
            std::vector<Stroke_Data>      strokeVec      = {};
            std::vector<size_t>           strokeBVec     = {};
            std::vector<Stroke_CrossData> strokeXVec     = {};
            bool                          strokeEnclosed = false;

            RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
            if(stack.top()->GetType() != IDS_PARSINGTOKEN || ((_ParsingToken*)stack.top().get())->GetToken() != "(") {
                SetIDSParseError(
                    parseError, idsU32, tokenPositions[i], "expected '(' after " + DescribeParseCharacter(U'#'));
                return IDSOwner();
            }
            stack.pop();

            while(true) {
                RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
                if(stack.top()->GetType() == IDS_PARSINGTOKEN &&
                    ((_ParsingToken*)stack.top().get())->GetToken() == ")") {
                    stack.pop();
                    break;
                }
                temp = std::move(stack.top());
                stack.pop();
                if(temp->GetType() != IDS_PARSINGTOKEN) {
                    SetIDSParseError(parseError, idsU32, tokenPositions[i], "invalid stroke token");
                    return IDSOwner();
                }
                pids.push_back(std::move(temp));
            }

            for(auto& j: pids) {
                jToken = ((_ParsingToken*)j.get())->GetToken();
                if(jToken.empty()) {
                    SetIDSParseError(parseError, idsU32, tokenPositions[i], "empty stroke token");
                    return IDSOwner();
                }
                if(jToken[0] == 'x') {
                    if(strokeVec.empty()) {
                        SetIDSParseError(parseError, idsU32, tokenPositions[i], "cross marker has no preceding stroke");
                        return IDSOwner();
                    }
                    try {
                        strokeXVec.push_back((Stroke_CrossData){strokeVec.size() - 1, std::stoul(jToken.substr(1))});
                    } catch(const std::exception&) {
                        SetIDSParseError(parseError, idsU32, tokenPositions[i], "invalid stroke cross marker");
                        return IDSOwner();
                    }
                } else if(jToken == "b") {
                    if(strokeVec.empty()) {
                        SetIDSParseError(parseError, idsU32, tokenPositions[i], "break marker has no preceding stroke");
                        return IDSOwner();
                    }
                    strokeBVec.push_back(strokeVec.size() - 1);
                } else if(jToken == "z")
                    strokeEnclosed = true;
                else if(jToken[0] == '-')
                    strokeVec.push_back((Stroke_Data){true, jToken.substr(1)});
                else
                    strokeVec.push_back((Stroke_Data){false, jToken});
            }
            stack.push(IDSOwner(new Stroke(strokeVec, strokeBVec, strokeXVec, strokeEnclosed)));
        } else if(IDCchar2type(tokens[i]) == IDC_HORIZONAL_ARRANGE || IDCchar2type(tokens[i]) == IDC_VERTICAL_ARRANGE) {
            RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
            size_t preferSplit = -1;
            bool   arrangementOpenParenSeen = false;
            while(!(
                stack.top()->GetType() == IDS_PARSINGTOKEN && ((_ParsingToken*)stack.top().get())->GetToken() == ")")) {
                RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
                temp = std::move(stack.top());
                stack.pop();
                if(!(temp->GetType() == IDS_PARSINGTOKEN && ((_ParsingToken*)temp.get())->GetToken() == "(")) {
                    if(temp->GetType() == IDS_PARSINGTOKEN && ((_ParsingToken*)temp.get())->GetToken() == "|") {
                        preferSplit = pids.size() - 1;
                        temp.reset();
                    } else if(temp->GetType() == IDS_PARSINGTOKEN) {
                        const std::string token = ((_ParsingToken*)temp.get())->GetToken();
                        if(!token.empty() && token.front() == '[' && pids.empty()) {
                            if(arrangementOpenParenSeen) {
                                bool numericOptional = false;
                                if(token.size() >= 3 && token.back() == ']') {
                                    try {
                                        size_t consumed = 0;
                                        std::stoi(token.substr(1, token.size() - 2), &consumed);
                                        numericOptional = consumed == token.size() - 2;
                                    } catch(const std::exception&) {}
                                }
                                if(!numericOptional) {
                                    SetIDSParseError(parseError, idsU32, tokenPositions[i],
                                        "HV origin range must precede '('");
                                    return IDSOwner();
                                }
                            }
                            pids.push_back(std::move(temp));
                        } else
                            pids.push_back(IDSOwner(new Ideograph(token)));
                    } else
                        pids.push_back(std::move(temp));
                } else
                {
                    arrangementOpenParenSeen = true;
                    temp.reset();
                }
            }
            RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
            int arrangeOptionalInt = 0;
            std::vector<HVOriginRange> hvOriginRanges;
            // 排列结构的方括号参数在反向解析时会先进入 pids，不能再从 ) 后的栈顶读取。
            if(!pids.empty() && pids.front()->GetType() == IDS_PARSINGTOKEN &&
                !((_ParsingToken*)pids.front().get())->GetToken().empty() &&
                ((_ParsingToken*)pids.front().get())->GetToken().front() == '[') {
                temp = std::move(pids.front());
                pids.erase(pids.begin());
                const std::string optional = ((_ParsingToken*)temp.get())->GetToken();
                if(!ParseHVOriginRangeToken(optional, hvOriginRanges)) {
                    const std::string value = optional.substr(1, optional.size() - 2);
                    try {
                        size_t consumed = 0;
                        arrangeOptionalInt = std::stoi(value, &consumed);
                        if(consumed != value.size()) throw std::invalid_argument("not an integer");
                    } catch(const std::exception&) {
                        SetIDSParseError(parseError, idsU32, tokenPositions[i], "invalid HV origin range");
                        return IDSOwner();
                    }
                }
            }
            stack.pop(); // )
            std::vector<IDS*> pidsRaw = BorrowIDS(pids);
            stack.push(IDSOwner(new Pattern(IDCchar2type(tokens[i]), pidsRaw, preferSplit, nullptr, {},
                arrangeOptionalInt, std::move(hvOriginRanges))));
        } else if(IDCchar2type(tokens[i]) != IDC_UNKNOWN) {
            int                      ocType = 0;
            std::string              optional;
            std::vector<std::string> splitTemp, overlayType;
            int                      overlayRange[2] = {0, 0};
            RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
            IDS* top = stack.top().get();
            if(top->GetType() == IDS_PARSINGTOKEN && ((_ParsingToken*)top)->GetToken()[0] == '[') {
                temp = std::move(stack.top());
                stack.pop();
                // Overlay
                if(IDCchar2type(tokens[i]) == IDC_OVERLAY) {
                    optional = ((_ParsingToken*)temp.get())->GetToken();
                    // Optional String parser
                    splitTemp = StringSplit(optional.substr(1, optional.length() - 2), U'|');
                    for(auto j: splitTemp) {
                        // Overlay Range
                        overlayRange[0] = 0, overlayRange[1] = 0;
                        if(j.find(':') != std::string::npos) {
                            overlayType = StringSplit(j, U':');
                            if(overlayType[0] != "") {
                                try {
                                    overlayRange[0] = std::stoi(overlayType[0]);
                                } catch(const std::exception& e) {}
                            }
                            if(overlayType[1] != "") {
                                try {
                                    overlayRange[1] = std::stoi(overlayType[1]);
                                } catch(const std::exception& e) {}
                            }
                            overlayType = {};
                        }
                        // Overlay Type
                        else
                            overlayType = StringSplit(j, U',');
                    }
                }
                // Other IDC
                else {
                    jToken = ((_ParsingToken*)temp.get())->GetToken();
                    if(jToken.substr(1, jToken.length() - 2) != "") {
                        try {
                            ocType = std::stoi(jToken.substr(1, jToken.length() - 2));
                        } catch(const std::exception& e) {}
                    }
                }
                temp.reset();
            }
            for(size_t j = 0; j < IDCargs[IDCchar2type(tokens[i])]; j++) {
                RETURN_NULLPTR_IF_STACK_IS_EMPTY(stack, i);
                temp = std::move(stack.top());
                stack.pop();
                if(temp->GetType() == IDS_PARSINGTOKEN) {
                    const std::string childToken = ((_ParsingToken*)temp.get())->GetToken();
                    if(childToken.empty() || childToken == "(" || childToken == ")" || childToken == "|" ||
                        childToken[0] == '[') {
                        size_t childPosition = tokenPositions[i];
                        for(size_t childIndex = i + 1; childIndex < tokens.size(); childIndex++)
                            if(tokens[childIndex] == childToken) {
                                childPosition = tokenPositions[childIndex];
                                break;
                            }
                        const char32_t childCharacter = childPosition < idsU32.length() ? idsU32[childPosition] : 0;
                        SetIDSParseError(parseError, idsU32, childPosition,
                            "unexpected " + DescribeParseCharacter(childCharacter) + " as IDS operand");
                        return IDSOwner();
                    }
                    pids.push_back(IDSOwner(new Ideograph(childToken)));
                    temp.reset();
                } else
                    pids.push_back(std::move(temp));
            }
            std::vector<IDS*> pidsRaw = BorrowIDS(pids);
            stack.push(IDSOwner(new Pattern(IDCchar2type(tokens[i]), pidsRaw, -1, overlayRange, overlayType, ocType)));
        }
        // non IDC
        else if(tokens[i].compare(0, 8, "<stroke=") == 0 || tokens[i].compare(0, 9, "<residue=") == 0) {
            SearchParamType param         = SPARAM_UNKNOWN;
            uint32_t        strokeMinimum = 0, strokeMaximum = 0;
            if(!ParseStrokeCountToken(tokens[i], param, strokeMinimum, strokeMaximum)) {
                SetIDSParseError(parseError, idsU32, tokenPositions[i], "invalid stroke-count condition");
                return IDSOwner();
            }
            stack.push(IDSOwner(new SearchParam(param, strokeMinimum, strokeMaximum)));
        } else {
            std::string variableName;
            if(ParseVariableToken(tokens[i], variableName))
                stack.push(IDSOwner(new IDSVariable(std::move(variableName))));
            else if(tokens[i].compare(0, 5, "<var=") == 0) {
                SetIDSParseError(parseError, idsU32, tokenPositions[i], "invalid variable name");
                return IDSOwner();
            } else if(tokens[i][0] == '{')
                stack.push(IDSOwner(new Ideograph(tokens[i].substr(1, tokens[i].length() - 2))));
            else if(tokens[i] == "(" || tokens[i] == ")" || tokens[i] == "|" || tokens[i][0] == '[')
                stack.push(IDSOwner(new _ParsingToken(tokens[i])));
            else if(tokens[i] == u8"\u2B1A")
                stack.push(IDSOwner(new SearchParam(SPARAM_QUESTIONMARK)));
            else
                stack.push(IDSOwner(new _ParsingToken(tokens[i])));
        }
    }
    if(stack.empty()) {
        SetIDSParseError(parseError, idsU32, idsU32.length(), "empty IDS expression");
        return nullptr;
    }
    // 保留原有后缀兼容行为：部分 YiBai 数据会在完整 IDS 后附加形态说明，
    // 解析器历史上会取栈顶的有效结果，不能因为新增诊断而改变这类数据的导入结果。
    if(stack.top()->GetType() == IDS_PARSINGTOKEN) {
        const std::string token = ((_ParsingToken*)stack.top().get())->GetToken();
        temp.reset(new Ideograph(token));
        stack.pop();
        while(!stack.empty())
            stack.pop();
        temp->SetUniqueSeparator(rootUniqueSeparator);
        return temp;
    }
    temp = std::move(stack.top());
    stack.pop();
    while(!stack.empty())
        stack.pop();
    temp->SetUniqueSeparator(rootUniqueSeparator);
    return temp;
}

IDSOwnerList ParseComponentSearchTerms(std::string input) {
    std::string expandedInput;
    if(!ExpandUnicodeEscapes(input, expandedInput)) return IDSOwnerList();
    input = std::move(expandedInput);

    IDSOwnerList   terms;
    std::u32string inputU32     = utf8::utf8to32(input);
    bool           hasSeparator = false;
    for(const char32_t character: inputU32)
        if(character == U' ' || character == U'\t' || character == U'\n' || character == U'\r') {
            hasSeparator = true;
            break;
        }

    size_t residueCount = 0;

    auto appendStrokeCondition = [&](const std::string& token) {
        IDSOwner condition = ParseIDSOwned(token);
        if(condition == nullptr || condition->GetType() != IDS_SEARCHPARAM) return false;
        const SearchParamType param = static_cast<SearchParam*>(condition.get())->GetParam();
        if(param != SPARAM_STROKE_COUNT && param != SPARAM_RESIDUE_STROKE_COUNT) return false;
        if(param == SPARAM_RESIDUE_STROKE_COUNT && ++residueCount > 1) return false;
        terms.push_back(std::move(condition));
        return true;
    };

    if(hasSeparator) {
        std::u32string token;
        for(size_t index = 0; index <= inputU32.length(); index++) {
            const bool isSeparator = index == inputU32.length() || inputU32[index] == U' ' ||
                inputU32[index] == U'\t' || inputU32[index] == U'\n' || inputU32[index] == U'\r';
            if(!isSeparator) {
                token += inputU32[index];
                continue;
            }
            if(token.empty()) continue;
            const std::string tokenUtf8 = utf8::utf32to8(token);
            if(token[0] == U'<') {
                if(!appendStrokeCondition(tokenUtf8)) return IDSOwnerList();
            } else
                terms.push_back(IDSOwner(new Ideograph(tokenUtf8)));
            token.clear();
        }
        return terms;
    }

    for(size_t index = 0; index < inputU32.length(); index++) {
        if(inputU32[index] == U'<') {
            const size_t tokenEnd = inputU32.find(U'>', index + 1);
            if(tokenEnd == std::u32string::npos ||
                !appendStrokeCondition(utf8::utf32to8(inputU32.substr(index, tokenEnd - index + 1))))
                return IDSOwnerList();
            index = tokenEnd;
        } else
            terms.push_back(IDSOwner(new Ideograph(utf8::utf32to8({inputU32[index]}))));
    }
    return terms;
}
IDS* ParseIDS(std::string ids) {
    return ParseIDSOwned(ids).release();
}
bool IDSequal(IDS* ids1, IDS* ids2) {
    if(ids1 == nullptr || ids2 == nullptr) return false;
    if(ids1->GetUniqueSeparator() != ids2->GetUniqueSeparator()) return false;
    if(ids1->GetType() == IDS_IDEOGRAPH && ids2->GetType() == IDS_IDEOGRAPH)
        return *(Ideograph*)ids1 == *(Ideograph*)ids2;
    if(ids1->GetType() == IDS_STROKE && ids2->GetType() == IDS_STROKE) return *(Stroke*)ids1 == *(Stroke*)ids2;
    if(ids1->GetType() == IDS_SEARCHPARAM && ids2->GetType() == IDS_SEARCHPARAM)
        return *(SearchParam*)ids1 == *(SearchParam*)ids2;
    if(ids1->GetType() == IDS_SEARCH && ids2->GetType() == IDS_SEARCH)
        return *(SearchExpression*)ids1 == *(SearchExpression*)ids2;
    if(ids1->GetType() == IDS_VARIABLE && ids2->GetType() == IDS_VARIABLE)
        return *(IDSVariable*)ids1 == *(IDSVariable*)ids2;
    if(ids1->GetType() == IDS_PATTERN && ids2->GetType() == IDS_PATTERN) return *(Pattern*)ids1 == *(Pattern*)ids2;
    if(ids1->GetType() == IDS_PARSINGTOKEN && ids2->GetType() == IDS_PARSINGTOKEN)
        return *(_ParsingToken*)ids1 == *(_ParsingToken*)ids2;
    else
        return false;
}

Ideograph Ideograph::GetPured(void) const {
    return this->inUnicode() ? Ideograph(this->GetIdeo()) : Ideograph(this->GetAbstractName());
}

const std::vector<HVOriginRange>& Pattern::GetHVOriginRanges() const {
    return _hvOriginRanges;
}

void Pattern::SetHVOriginRanges(std::vector<HVOriginRange> ranges) {
    _hvOriginRanges = std::move(ranges);
}
