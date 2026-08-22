/*!
 * \file FutuConfig.cpp
 * \brief 配置工具方法实现
 */
#include "FutuConfig.h"
#include <cstring>

namespace futu
{

namespace
{
// V8-R5: "键存在但值无效" 判定 — 旧实现 node->asDouble() 对 VT_Null/空串/
// 类型错(Object/Array) 一律返回 0.0 而非默认值 (protectTicks: 空值 → 价格保护
// 静默关闭)。以下情况一律回落默认值: 节点为 Null、Object/Array 容器、空字符串。
inline bool isInvalidScalar(const wtp::WTSVariant* node)
{
    if (!node)
        return true;
    switch (node->type())
    {
    case wtp::WTSVariant::VT_Null:
    case wtp::WTSVariant::VT_Object:
    case wtp::WTSVariant::VT_Array:
        return true;
    case wtp::WTSVariant::VT_String:
        return node->asCString()[0] == '\0';
    default:
        return false;
    }
}
} // namespace

double FutuConfig::readDouble(wtp::WTSVariant* cfg, const char* key, double defVal)
{
    if (!cfg)
        return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return isInvalidScalar(node) ? defVal : node->asDouble();
}

uint32_t FutuConfig::readUInt32(wtp::WTSVariant* cfg, const char* key, uint32_t defVal)
{
    if (!cfg)
        return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return isInvalidScalar(node) ? defVal : node->asUInt32();
}

bool FutuConfig::readBool(wtp::WTSVariant* cfg, const char* key, bool defVal)
{
    if (!cfg)
        return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    if (isInvalidScalar(node))
        return defVal;
    // V8-R5: asBoolean 只认 "true"/"yes" 字符串, YAML 数值 1 会被读成 false。
    // 数值型按 !=0 判定, 字符串/布尔型走原语义 (true/yes, 兼容 on/1)。
    switch (node->type())
    {
    case wtp::WTSVariant::VT_Int32:
    case wtp::WTSVariant::VT_Uint32:
    case wtp::WTSVariant::VT_Int64:
    case wtp::WTSVariant::VT_Uint64:
    case wtp::WTSVariant::VT_Real:
        return node->asDouble() != 0.0;
    case wtp::WTSVariant::VT_String: {
        const char* s = node->asCString();
        return wt_stricmp(s, "true") == 0 || wt_stricmp(s, "yes") == 0 || wt_stricmp(s, "on") == 0 ||
               strcmp(s, "1") == 0;
    }
    default:
        return node->asBoolean();
    }
}

std::string FutuConfig::readString(wtp::WTSVariant* cfg, const char* key, const char* defVal)
{
    if (!cfg)
        return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    // VT_Null/容器 → 默认值; 空字符串保留原语义 (调用方可能合法地配空串)
    if (!node || node->type() == wtp::WTSVariant::VT_Null || node->type() == wtp::WTSVariant::VT_Object ||
        node->type() == wtp::WTSVariant::VT_Array)
        return defVal;
    return node->asString();
}

} // namespace futu
