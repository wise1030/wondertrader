/*!
 * \file FutuDataDefs.h
 * \brief Memory-mapped data block definitions for Futures Market Making
 * 
 * This file provides namespace aliases to reuse WtUftCore/UftDataDefs.h
 * Avoiding duplicate definitions for better maintainability.
 */
#pragma once

// 复用 UFT 框架的数据定义，避免重复
#include "../WtUftCore/UftDataDefs.h"

namespace futu {

// 命名空间别名：直接复用 uft 命名空间的类型定义
// 这样既保持向后兼容，又避免了重复定义

using BlockHeader = uft::BlockHeader;
using DetailStruct = uft::DetailStruct;
using PositionBlock = uft::PositionBlock;
using OrderStruct = uft::OrderStruct;
using OrderBlock = uft::OrderBlock;
using TradeStruct = uft::TradeStruct;
using TradeBlock = uft::TradeBlock;
using RoundStruct = uft::RoundStruct;
using RoundBlock = uft::RoundBlock;

// 常量也复用
using uft::BLK_FLAG;
using uft::FLAG_SIZE;

} // namespace futu