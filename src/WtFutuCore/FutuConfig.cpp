/*!
 * \file FutuConfig.cpp
 * \brief 配置工具方法实现
 */
#include "FutuConfig.h"

namespace futu {

double FutuConfig::readDouble(wtp::WTSVariant* cfg, const char* key, double defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asDouble() : defVal;
}

uint32_t FutuConfig::readUInt32(wtp::WTSVariant* cfg, const char* key, uint32_t defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asUInt32() : defVal;
}

bool FutuConfig::readBool(wtp::WTSVariant* cfg, const char* key, bool defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asBoolean() : defVal;
}

std::string FutuConfig::readString(wtp::WTSVariant* cfg, const char* key, const char* defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asString() : defVal;
}

} // namespace futu
