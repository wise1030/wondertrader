/*!
 * \file FutuModuleAssembler.h
 * \brief 5A-3 (v7.5): 业务模块装配器
 *
 * 从 UftFutuMmStrategy 剥离的模块创建/配置/依赖注入逻辑
 * (原 initBusinessModules)。friend 访问策略私有成员。
 */
#pragma once

namespace wtp
{
class IUftStraCtx;
}

namespace futu
{

class UftFutuMmStrategy;

class FutuModuleAssembler
{
public:
    /// 创建并接线全部业务模块 (coordinator/portfolio/quoter/risk/arb/...)
    static void assemble(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx);

    /// 合约信息加载 (session 缓存/收盘时间推导/乘数tick回填, 须在 assemble 前)
    static void loadContractInfos(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx);
};

} // namespace futu
