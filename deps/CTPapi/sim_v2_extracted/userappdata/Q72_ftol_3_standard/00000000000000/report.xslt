<?xml version="1.0" encoding="utf-8"?><!-- DWXMLSource="file:///D|/projects/trader/Report/report.xml" -->
<!DOCTYPE xsl:stylesheet  [
	<!ENTITY nbsp   "&#160;">
	<!ENTITY copy   "&#169;">
	<!ENTITY reg    "&#174;">
	<!ENTITY trade  "&#8482;">
	<!ENTITY mdash  "&#8212;">
	<!ENTITY ldquo  "&#8220;">
	<!ENTITY rdquo  "&#8221;"> 
	<!ENTITY pound  "&#163;">
	<!ENTITY yen    "&#165;">
	<!ENTITY euro   "&#8364;">
]>
<xsl:stylesheet version="2.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:fo="http://www.w3.org/1999/XSL/Format" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:fn="http://www.w3.org/2005/xpath-functions" xmlns:v="urn:schemas-microsoft-com:vml" >

    <xsl:output method="html" encoding="utf-8" doctype-public="-//W3C//DTD XHTML 1.0 Strict//EN" doctype-system="http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"/>
<xsl:template match="/Report">
        <html xmlns="http://www.w3.org/1999/xhtml">
        <head>
		<meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
        <title>快期交易报告-UserName-ReportPeriod</title>
  <style type="text/css" media="all" >
v\:*
{
	behavior:url(#default#VML);
}
body{ 
	width:600px;
	font-size:10px;
	font-family:Arial, Helvetica, sans-serif;
}
.Line2 {
	border-bottom-width: 1px;
	border-bottom-style: solid;
	border-bottom-color: #AAAAAA;
}
.LineHead {
	font-weight:bold;
	color:777777;
	font-size:12px;
	border-bottom:solid 1px #AAAAAA;
/*	background-color: #EEEEEE;*/
	
}

 .SectorHeader {
/*	border-bottom:solid 1px black;*/
	margin-top:30px;
	margin-bottom:10px;
	text-align:right;
	background-color: #777777;
	color:white;
	font-size:16px;
	font-weight:bold;
	line-height:24px;
}
.indicBlock{
	border:dotted 1px #777777;
}
.indicBlock1{
	font-size:12px;
	font-weight:bold;
	color:#000000;
}	
.indicBlock2{
	font-size:12px;
	color:#000000;
	border-bottom:solid 2px #777777;
}
.indicBlock3{
	font-size:10px;
	font-family:宋体
	color:#000000;
	margin-top:100px;
}
.Summary{
	font-size:14px;
	font-weight:bold;
	border-top:double 3px black;
}
.Summary1{
	font-size:14px;
	font-weight:bold;
	padding-top:10px;
	border-bottom:solid 1px black;
}
.Summary2{
	font-size:12px;
	font-weight:bold;
	}

  </style>
        </head>
            <body>

<div style="width:600; border-bottom:solid 1px black; vertical-align:bottom;height:60px;">
<div style="float:left;margin-top:15px;font-size:40px;font-weight:bold;">交易及账户报告</div>
<table width="300" border="0" style="float:right;clear:none;width:200px;height:60px;">
  <tr>
    <td width="245"><div align="left"><span class="MsoNormal">账号</span></div></td>
    <td width="237"><div align="right"><span class="MsoNormal"><xsl:value-of select="Summary/Investor/@id"/></span></div></td>
    </tr>
  <tr>
    <td><div align="left">姓名</div></td>
    <td><div align="right"><xsl:value-of select="Summary/Investor/@name"/></div></td>
    </tr>
  <tr>
    <td><div align="left">报告期间</div></td>
    <td><div align="right"><xsl:value-of select="Summary/Reportperiod"/></div></td>
    </tr>
</table>
</div>            

<xsl:for-each select="Account">
<div class="SectorHeader">账户概要</div>
<table border="0" cellspacing="0" cellpadding="0" id="table2">
	<tr height="20">
		<td width="150" class="indicBlock3">期初账户净值</td>
		<td width="80" align="right"><xsl:value-of select="StartBalance"/></td>
	    <td width="20" rowspan="12">&nbsp;</td>
	    <td width="400" rowspan="12" style="valign:top;">
	    <div style="width:390px; height:240px;">
		<xsl:for-each select="AccountHis">
			<v:group style="position:relative;WIDTH:395px;HEIGHT:230px;" coordsize="110,110">
				<v:rect style="WIDTH:110px;HEIGHT:110px" fillcolor="white" strokecolor="black">
				</v:rect>
				<v:line from="10,100" to="110,100" strokeweight="1px">
					<v:stroke EndArrow="classic"/>
				</v:line>
				<v:line from="10,100" to="10,0" strokeweight="1px">
					<v:stroke EndArrow="classic"/>
				</v:line>
				<v:shape style="left: 1px; top:10px;">
					<v:textbox inset="0px,0px,0px,0px" style="font-size:9px;"><xsl:value-of select="@maxy"/></v:textbox>
				</v:shape>
				<v:shape style="left: 1px; top:90px;">
					<v:textbox inset="0px,0px,0px,0px" style="font-size:9px;"><xsl:value-of select="@miny"/></v:textbox>
				</v:shape>
				<v:shape style="left: 10px; top:100px;">
					<v:textbox inset="0px,0px,0px,0px" style="font-size:10.2px;"><xsl:value-of select="@minx"/></v:textbox>
				</v:shape>
				<v:shape style="left: 90px; top:100px;">
					<v:textbox inset="0px,0px,0px,0px" style="font-size:10.2px;"><xsl:value-of select="@maxx"/></v:textbox>
				</v:shape>
				<v:PolyLine filled="false" Points="{.}"  strokeweight="3px"/>
			 </v:group>
		</xsl:for-each>
	    </div>
	    </td>
	</tr>
	<tr height="20">
		<td>　＋出入金</td>
		<td><div align="right"><xsl:value-of select="CashInOut"/></div></td>
      </tr>
	<tr height="20">
		<td>　＋交易盈亏</td>
		<td><div align="right"><xsl:value-of select="TotalProfit"/></div></td>
      </tr>
	<tr height="20">
		<td class="indicBlock3">　－手续费</td>
		<td align="right"><xsl:value-of select="Commission"/></td>
      </tr>
	<tr height="20">
		<td>　＝期末账户净值</td>
		<td><div align="right"><xsl:value-of select="EndBalance"/></div></td>
      </tr>
	<tr height="20">
		<td>　－持仓占用保证金</td>
		<td><div align="right"><xsl:value-of select="UseMargin"/></div></td>
      </tr>
	<tr height="20">
		<td>　－挂单冻结</td>
		<td><div align="right"><xsl:value-of select="Frozen"/></div></td>
      </tr>
	<tr height="20">
		<td>　＝可用资金</td>
		<td><div align="right"><xsl:value-of select="Available"/></div></td>
      </tr>
</table>
</xsl:for-each>

<xsl:for-each select="TradeStat">
<div class="SectorHeader">交易统计</div>
<table border="0" cellpadding="0" cellspacing="0" width="100%" id="table5">
	<tr>
		<td class="LineHead" align="left"><xsl:value-of select="Stat/@field"/></td>
		<td class="LineHead" align="left"><xsl:value-of select="Stat/Stat/@field"/></td>
		<td class="LineHead" align="right"><xsl:value-of select="Stat/Stat/Stat/@field"/></td>
		<td class="LineHead" align="right">交易次数</td>
		<td class="LineHead" align="right">手数</td>
		<td class="LineHead" align="right">净利润</td>
		<td class="LineHead" align="right">平均单次利润</td>
		<td class="LineHead" align="right">平均每手利润</td>
	  </tr>
<xsl:for-each select="Stat">
  <tr>
	<td class="Summary1"><xsl:value-of select="@name"/></td>
		<td class="Summary1">&nbsp;</td>
		<td class="Summary1">&nbsp;</td>
                <td class="Summary1" align="right"><xsl:value-of select="TradeCount"/></td>
              <td class="Summary1" align="right"><xsl:value-of select="Volume"/></td>
              <td class="Summary1" align="right"><xsl:value-of select="NetProfit"/></td>
              <td class="Summary1" align="right"><xsl:value-of select="AvgProfitPerTrade"/></td>
              <td class="Summary1" align="right"><xsl:value-of select="AvgProfitPerHand"/></td>
		</tr>
          <xsl:for-each select="Stat">
          <tr>
		<td class="Summary2">&nbsp;</td>
                <td class="Summary2" colspan="2"><xsl:value-of select="@name"/></td>
                <td class="Summary2" align="right"><xsl:value-of select="TradeCount"/></td>
              <td class="Summary2" align="right"><xsl:value-of select="Volume"/></td>
              <td class="Summary2" align="right"><xsl:value-of select="NetProfit"/></td>
              <td class="Summary2" align="right"><xsl:value-of select="AvgProfitPerTrade"/></td>
              <td class="Summary2" align="right"><xsl:value-of select="AvgProfitPerHand"/></td>
		</tr>
          <xsl:for-each select="Stat">
          <tr>
		<td>&nbsp;</td>
		<td>&nbsp;</td>
                <td align="right"><xsl:value-of select="@name"/></td>
                <td align="right"><xsl:value-of select="TradeCount"/></td>
              <td align="right"><xsl:value-of select="Volume"/></td>
              <td align="right"><xsl:value-of select="NetProfit"/></td>
              <td align="right"><xsl:value-of select="AvgProfitPerTrade"/></td>
              <td align="right"><xsl:value-of select="AvgProfitPerHand"/></td>
              </tr>
          </xsl:for-each>	
          </xsl:for-each>	
  </xsl:for-each>	
	<tr>
		<td class="Summary" colspan="3">总计</td>
		<td class="Summary" align="right"><xsl:value-of select="TradeCount"/></td>
		<td class="Summary" align="right"><xsl:value-of select="Volume"/></td>
		<td class="Summary" align="right"><xsl:value-of select="NetProfit"/></td>
		<td class="Summary" align="right"><xsl:value-of select="AvgProfitPerTrade"/></td>
		<td class="Summary" align="right"><xsl:value-of select="AvgProfitPerHand"/></td>
	  </tr>
</table>
</xsl:for-each>	


<xsl:for-each select="Indicators">
<div class="SectorHeader">指标分析</div>
<table width="100%" border="0" style="vertical-align:top;">
  <tr valign="top">
    <td class="indicBlock1"><span class="MsoNormal">风控指标</span></td>
    <td>&nbsp;</td>
    <td class="indicBlock1"><span class="MsoNormal">收益指标</span></td>
  </tr>
  <tr valign="top">
    <td class="indicBlock"><table width="100%" border="0">
      
      <tr>
        <td class="indicBlock2">指标名称</td>
        <td class="indicBlock2">指标值</td>
        <td class="indicBlock2">参考值</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大逆向波动幅度</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大单次亏损</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大日亏损</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大周亏损</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大月亏损</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大连续亏损金额</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大连续亏损百分比</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">最大连续亏损日数</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
    </table></td>
    <td>&nbsp;</td>
    <td class="indicBlock"><table width="100%" border="0">
      
      <tr>
        <td class="indicBlock2">指标名称</td>
        <td class="indicBlock2">指标值</td>
        <td class="indicBlock2">参考值</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">期间收益￥</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">期间收益%</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均单次交易￥</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均单次收益%</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均周收益%</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
    </table></td>
  </tr>
  <tr>
    <td class="indicBlock1"><span class="MsoNormal">交易与持仓</span></td>
    <td>&nbsp;</td>
    <td class="indicBlock1"><span class="MsoNormal">收益结构</span></td>
  </tr>
  <tr valign="top">
    <td class="indicBlock"><table width="100%" border="0">
      
      <tr class="indicBlock2">
        <td class="indicBlock2">指标名称</td>
        <td class="indicBlock2">指标值</td>
        <td class="indicBlock2">参考值</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均周交易次数</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均周交易额</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">次均仓位比重</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均周换手率</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均持仓时间</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">持仓时间占比</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
    </table></td>
    <td>&nbsp;</td>
    <td class="indicBlock"><table width="100%" border="0">
      
      <tr class="indicBlock2">
        <td class="indicBlock2">指标名称</td>
        <td class="indicBlock2">指标值</td>
        <td class="indicBlock2">参考值</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">盈亏次数比</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">平均盈亏额度比</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">大额盈利比率</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td><span class="MsoNormal">大额亏损比率</span></td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
      <tr>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
        <td>&nbsp;</td>
      </tr>
    </table></td>
  </tr>
  <tr>
    <td>&nbsp;</td>
    <td>&nbsp;</td>
    <td>&nbsp;</td>
  </tr>
</table>
</xsl:for-each>


<xsl:for-each select="ActivePositions">
<div class="SectorHeader">期末持仓</div>
<table border="0" cellpadding="0" cellspacing="0" width="100%" id="table4">
	<tr>
		<td width="64" class="LineHead" align="center">&nbsp;交易所</td>
		<td class="LineHead" align="center">合约</td>
		<td class="LineHead" align="right">手数</td>
		<td class="LineHead" align="right">日期</td>
		<td class="LineHead" align="right">买入</td>
		<td class="LineHead" align="right">卖出</td>
		<td class="LineHead" align="right">浮动盈亏</td>
		<td class="LineHead" align="center">投保</td>
	</tr>
  <xsl:for-each select="Position">
	<tr>
		<td width="64" align="center"><xsl:value-of select="Exchange"/></td>
		<td align="center"><xsl:value-of select="InstrumentID"/></td>
		<td align="right"><xsl:value-of select="Volume"/></td>
		<td align="right"><xsl:value-of select="OpenDateTime"/></td>
		<td align="right">　
      <xsl:if test="OpenBuyPrice &gt; 0">
<xsl:value-of select="OpenBuyPrice"  disable-output-escaping="yes"/>      </xsl:if>
		</td>
		<td align="right">　
      <xsl:if test="OpenSellPrice &gt; 0">
		<xsl:value-of select="OpenSellPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
		<td><p>&nbsp;</p></td>
		<td><p>&nbsp;</p></td>
		<td align="center">　</td>
	</tr>
	<tr>
		<td width="64" class="Line2">　</td>
		<td class="Line2">　</td>
		<td class="Line2">　</td>
		<td class="Line2" align="right">期末结算价</td>
		<td class="Line2" align="right">　
      <xsl:if test="CloseBuyPrice &gt; 0">
		<xsl:value-of select="CloseBuyPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
		<td class="Line2" align="right">　
      <xsl:if test="CloseSellPrice &gt; 0">
		<xsl:value-of select="CloseSellPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
		<td class="Line2" align="right"><xsl:value-of select="FloatProfit"/></td>
		<td align="center" class="Line2"><xsl:value-of select="Hedge"/></td>
	</tr>
  </xsl:for-each>
	<tr>
		<td class="Summary">合计</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary" align="right"><xsl:value-of select="FloatProfit"/></td>
		<td class="Summary">&nbsp;</td>
	</tr>
</table>
  </xsl:for-each>


<xsl:for-each select="ClosedPositions">

<div class="SectorHeader">平仓记录</div>
<table border="0" cellpadding="0" cellspacing="0" width="100%" id="table3">
<thead style="display:table-header-group;">
	<tr>
		<td class="LineHead" align="center" width="64" >交易所</td>
		<td class="LineHead" align="center">合约</td>
		<td class="LineHead" align="right">手数</td>
		<td class="LineHead" align="right">日期</td>
		<td class="LineHead" align="right">买入</td>
		<td class="LineHead" align="right">卖出</td>
		<td class="LineHead" align="right">平仓盈亏</td>
	</tr>
</thead>
  <xsl:for-each select="Position">
	<tr>
	  <td align="center"><xsl:value-of select="Exchange"/></td>
	  <td align="center"><xsl:value-of select="InstrumentID"/></td>
	  <td align="right"><xsl:value-of select="Volume"/></td>
	  <td align="right"><xsl:value-of select="OpenDateTime" disable-output-escaping="yes"/></td>
		<td align="right">　
      <xsl:if test="OpenBuyPrice &gt; 0">
<xsl:value-of select="OpenBuyPrice"  disable-output-escaping="yes"/>      </xsl:if>
		</td>
		<td align="right">　
      <xsl:if test="OpenSellPrice &gt; 0">
		<xsl:value-of select="OpenSellPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
	  <td  align="center"><xsl:value-of select="OpenOrderStyle"/></td>
	  </tr>
	<tr>
		<td width="64" class="Line2">&nbsp;</td>
		<td class="Line2">&nbsp;</td>
		<td class="Line2" align="right">&nbsp;</td>
		<td class="Line2" align="right"><xsl:value-of select="CloseDateTime" disable-output-escaping="yes"/></td>
		<td class="Line2" align="right">　
      <xsl:if test="CloseBuyPrice &gt; 0">
		<xsl:value-of select="CloseBuyPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
		<td class="Line2" align="right">　
      <xsl:if test="CloseSellPrice &gt; 0">
		<xsl:value-of select="CloseSellPrice"  disable-output-escaping="yes"/>
</xsl:if>
		</td>
				<td class="Line2"  align="right">　<xsl:value-of select="CloseProfit"/></td>
	</tr>
  </xsl:for-each>
	<tr>
		<td class="Summary">合计</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary">&nbsp;</td>
		<td class="Summary" align="right"><xsl:value-of select="CloseProfit"/></td>
	</tr>
</table>
 </xsl:for-each>

<ol style="border-top:solid 1px black; margin-top: 40px; padding-top: 10px;">
报告说明：
	<li>本报告提供的数据仅供参考，不具法律效应，交易所、期货公司及软件公司均不对此报告的准确性负责。若此报告的数据与结算单不符，应以结算单为准。</li>
	<li>【平仓记录】及【交易统计】中的平仓盈亏与【账户概要】中的值不相等，系算法不同而产生的差异。在【平仓记录】及【交易统计】中，平仓盈亏指一个持仓从开仓时刻至平仓时刻之间产生的盈亏。而在【账户概要】中，平仓盈亏只计算选定报告范围内的部分。 </li>
</ol>
        </body>
        </html>
    </xsl:template>
</xsl:stylesheet>