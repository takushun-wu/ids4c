#ifndef _IDS4C_H
#define _IDS4C_H

#include <cstdint>
#include <cstring>
#include <memory>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include "utf8.h"

/**
 * @file ids4c.h
 * @brief IDS 语法树、字形值对象和查询表达式的公共接口。
 *
 * API 分层约定：稳定值类型适合语言绑定；解析接口优先使用 IDSOwner；
 * 暴露裸指针或内部容器的接口仅供高级 C++ 扩展使用。
 */

/// IDS 节点的运行时类型；跨语言绑定可用它进行类型判别。
#define VAILD_GLYPHNAME_CHAR ".0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz"
#define VAILD_ASCII_CHAR                                                                                      \
    " !\"#$%&'()*+,-./" "0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`" "abcdefghijklmnopqrstuvwxyz{|}~"
#define ASCII_UPPERCASE "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define ASCII_LOWERCASE "abcdefghijklmnopqrstuvwxyz"

typedef enum {
    IDS_UNKNOWN,
    IDS_STROKE,
    IDS_IDEOGRAPH,
    IDS_PATTERN,
    IDS_SEARCHPARAM,
    IDS_SEARCH,
    IDS_VARIABLE,
    IDS_PARSINGTOKEN
} IDStype;

/**
 * @brief 所有 IDS 节点的多态基类。
 *
 * 绑定层应使用 IDSOwner 管理解析结果，不建议直接暴露 IDS* 的释放约定。
 */
class IDS {
protected:
    IDStype _type = IDS_UNKNOWN;

public:
    IDS()                                 = default;
    virtual ~IDS()                        = default;
    virtual IDS*         DeepCopy() const = 0;
    virtual std::string  toString() const = 0;
    std::unique_ptr<IDS> Clone() const;

    /// 返回节点类型；不转移任何对象所有权。
    IDStype GetType();
};

/// IDS 节点的拥有型指针；解析函数和返回 IDS 树的 API 优先使用此类型。
using IDSOwner     = std::unique_ptr<IDS>;
using IDSOwnerList = std::vector<IDSOwner>;

/**
 * @brief 一个 Unicode 字形、变体选择器或抽象字形名。
 *
 * 这是最适合跨语言绑定的核心值类型，对象按值复制不会共享内部状态。
 */
class Ideograph: public IDS {
protected:
    uint32_t    _ideo         = 0;
    uint32_t    _vs           = 0;
    std::string _suffix       = "";
    std::string _abstractName = "";

public:
    Ideograph(uint32_t code, uint32_t vs = 0, std::string suffix = "");
    Ideograph(std::string glyphName);
    ~Ideograph() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    uint32_t    GetIdeo() const;
    uint32_t    GetVS() const;
    std::string GetSuffix() const;
    std::string GetAbstractName() const;
    Ideograph   GetPured(void) const;

    bool operator==(const Ideograph& ideo) const;
    bool inUnicode() const;
    bool inPUA() const;

    friend struct Ideograph_Hash;
    friend bool IdeographCmp(Ideograph x, Ideograph y);
};

bool IdeographCmp(Ideograph x, Ideograph y);

struct Ideograph_Hash {
    size_t operator()(const Ideograph& ideo) const {
        size_t h1 = std::hash<uint32_t>()(ideo._ideo);
        size_t h2 = std::hash<uint32_t>()(ideo._vs);
        size_t h3 = std::hash<std::string>()(ideo._suffix);
        size_t h4 = std::hash<std::string>()(ideo._abstractName);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

typedef struct _Stroke_Data {
    bool        neg;
    std::string stroke;

    bool operator==(const struct _Stroke_Data& other) const { return (neg == other.neg && stroke == other.stroke); }
} Stroke_Data;

typedef struct _Stroke_CrossData {
    size_t pos;
    size_t cross;

    bool operator==(const struct _Stroke_CrossData& other) const { return (pos == other.pos && cross == other.cross); }
} Stroke_CrossData;

/**
 * @brief 笔画序列节点。
 *
 * 可作为跨语言的只读结果对象使用；GetStrokeRef() 属于高级 C++ API。
 */
class Stroke: public IDS {
protected:
    std::vector<Stroke_Data>      _stroke    = {};
    std::vector<size_t>           _breakPos  = {};
    std::vector<Stroke_CrossData> _crossData = {};
    bool                          _enclosed  = false;

public:
    Stroke(std::vector<Stroke_Data> stroke, std::vector<size_t> breakPos = {}, std::vector<Stroke_CrossData> cross = {},
        bool enclosed = false);
    ~Stroke() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    std::vector<Stroke_Data>      GetStroke();
    std::vector<Stroke_Data>&     GetStrokeRef();
    std::vector<size_t>           GetBreakPos();
    std::vector<Stroke_CrossData> GetCrossData();
    bool                          isEnclosed();

    bool operator==(const Stroke& stroke);
    bool isBasicStroke();
};

typedef enum { SPARAM_UNKNOWN, SPARAM_QUESTIONMARK, SPARAM_STROKE_COUNT, SPARAM_RESIDUE_STROKE_COUNT } SearchParamType;

/// 查询参数节点；适合由绑定层作为只读值对象暴露。
class SearchParam: public IDS {
protected:
    SearchParamType _param         = SPARAM_UNKNOWN;
    uint32_t        _strokeMinimum = 0;
    uint32_t        _strokeMaximum = 0;

public:
    SearchParam(SearchParamType param);
    SearchParam(uint32_t strokeMinimum, uint32_t strokeMaximum);
    SearchParam(SearchParamType param, uint32_t strokeMinimum, uint32_t strokeMaximum);
    ~SearchParam() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    SearchParamType GetParam();
    uint32_t        GetStrokeMinimum() const;
    uint32_t        GetStrokeMaximum() const;

    bool operator==(const SearchParam& sparam);
    bool operator==(const SearchParamType& param);
};

class IDSVariable: public IDS {
protected:
    std::string _name;

public:
    explicit IDSVariable(std::string name);
    ~IDSVariable() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    const std::string& GetName() const;
    bool               operator==(const IDSVariable& variable) const;
};
typedef enum { SEARCH_EXPRESSION_ALL, SEARCH_EXPRESSION_ANY, SEARCH_EXPRESSION_EXCEPT } SearchExpressionMode;

/**
 * @brief <search=...>、<any=...> 和 <except=...> 表达式。
 *
 * terms 和 exceptTerms 的所有权由对象接管；绑定层应优先通过 ParseIDSOwned() 获得对象。
 */
class SearchExpression: public IDS {
protected:
    IDSOwnerList         _terms       = {};
    IDSOwnerList         _exceptTerms = {};
    SearchExpressionMode _mode        = SEARCH_EXPRESSION_ALL;

public:
    SearchExpression(
        IDSOwnerList terms, SearchExpressionMode mode = SEARCH_EXPRESSION_ALL, IDSOwnerList exceptTerms = {});
    ~SearchExpression() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    const IDSOwnerList&  GetTerms() const;
    const IDSOwnerList&  GetExceptTerms() const;
    SearchExpressionMode GetMode() const;
    bool                 operator==(const SearchExpression& search) const;
};

/// IDS 结构符号类型，包括 U+303E 〾 变体/近似标记。
typedef enum {
    IDC_UNKNOWN,
    IDC_LEFT_RIGHT,
    IDC_ABOVE_BELOW,
    IDC_LEFT_MIDDLE_RIGHT,
    IDC_ABOVE_MIDDLE_BELOW,
    IDC_SURROUND_FULL,
    IDC_SURROUND_ABOVE,
    IDC_SURROUND_BELOW,
    IDC_SURROUND_LEFT,
    IDC_SURROUND_RIGHT,
    IDC_SURROUND_UPPERLEFT,
    IDC_SURROUND_UPPERRIGHT,
    IDC_SURROUND_LOWERLEFT,
    IDC_SURROUND_LOWERRIGHT,
    IDC_OVERLAY,
    IDC_HORIZONAL_REFLECT,
    IDC_ROTATE,
    IDC_SUBTRACT,
    IDC_VARIANT,
    IDC_HORIZONAL_ARRANGE,
    IDC_VERTICAL_ARRANGE,
    IDC_REPLACE
} IDCtype;

/**
 * @brief IDS 结构节点。
 *
 * GetpIDSRef() 暴露内部所有权容器，属于高级 C++ API；普通调用者应使用 GetpIDS() 或 toString()。
 */
class Pattern: public IDS {
protected:
    IDCtype                  _idc              = IDC_UNKNOWN;
    IDSOwnerList             _pids             = {};
    size_t                   _preferSplitPoint = -1;
    int                      _overlayRange[2]  = {0, 0};
    std::vector<std::string> _overlayType      = {};
    int                      _optionalInt      = 0;

public:
    Pattern(IDCtype idc, std::vector<IDS*> pids, size_t preferSplitPoint = -1, int* overlayRange = nullptr,
        std::vector<std::string> overlayType = {}, int optionalInt = 0);
    Pattern(const Pattern& pattern);
    Pattern(Pattern&& pattern) noexcept;
    ~Pattern();
    Pattern&    operator=(const Pattern& pattern);
    Pattern&    operator=(Pattern&& pattern) noexcept;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    IDCtype                  GetIDC() const;
    std::vector<IDS*>        GetpIDS() const;
    IDSOwnerList&            GetpIDSRef();
    const IDSOwnerList&      GetpIDSRef() const;
    size_t                   GetPreferSplitPoint() const;
    void                     GetOverlayRange(int* output) const;
    std::vector<std::string> GetOverlayType() const;
    int                      GetOptionalInt() const;

    bool operator==(const Pattern& pattern);
};

/**
 * @brief IDS 解析错误。
 *
 * position 为 Unicode code-point 位置而不是 UTF-8 字节偏移；characterIndex 从 1 开始。
 */
typedef struct {
    bool        hasPosition = false;
    size_t      position    = 0; // Unicode code-point position, zero-based.
    char32_t    character   = 0;
    std::string message;
} IDSParseError;

/// 稳定解析接口：解析 IDS，并以拥有型树返回结果；失败时返回 nullptr。
IDSOwner     ParseIDSOwned(std::string ids);
/// 带详细错误信息的 IDS 解析接口；error 可以为 nullptr。
IDSOwner     ParseIDSOwned(std::string ids, IDSParseError* error);
/// 兼容旧代码的解析接口；不建议直接用于新的跨语言绑定。
IDS*         ParseIDS(std::string ids);
/// 解析逗号分隔的部件搜索项；返回列表拥有其全部节点。
IDSOwnerList ParseComponentSearchTerms(std::string input);

/// 内部解析 token，不属于稳定的跨语言 API。
class _ParsingToken: public IDS {
protected:
    std::string _token = "";

public:
    _ParsingToken(std::string token);
    ~_ParsingToken() = default;
    IDS*        DeepCopy() const override;
    std::string toString() const override;

    std::string GetToken();

    bool operator==(const _ParsingToken& parseToken);
};

/// 将一个 IDS 结构符号转换为 IDCtype；未知符号返回 IDC_UNKNOWN。
IDCtype                  IDCchar2type(std::string idc);
/// 将 IDCtype 转换为结构符号字符串；未知类型返回 <invalidIDC>。
const char*              IDCtype2char(IDCtype idc);
std::vector<std::string> StringSplit(std::string str, char32_t split);
IDSOwner                 ParseIDSOwned(std::string ids);
IDSOwner                 ParseIDSOwned(std::string ids, IDSParseError* error);
IDS*                     ParseIDS(std::string ids);
/// 比较两棵 IDS 树的结构和值是否完全相等。
bool                     IDSequal(IDS* ids1, IDS* ids2);

#endif
