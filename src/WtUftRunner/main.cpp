/*!
 * /file main.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */

#include "WtUftRunner.h"

#include "../WTSTools/WTSLogger.h"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "../Share/cppcli.hpp"
//#include <vld.h>

// SIGCHLD 信号处理函数
#ifndef _MSC_VER
static void sigchld_handler(int signo)
{
    // SIGCHLD 通常不需要特殊处理，只需忽略即可
    // 这样可以避免 "app discard signal 17" 错误
    (void)signo;  // 避免未使用参数警告
}
#endif

int main(int argc, char* argv[])
{
#ifdef _MSC_VER
	CMiniDumper::Enable("WtUftRunner.exe", true);
#else
    // 设置 SIGCHLD 信号处理，避免信号被丢弃导致错误日志
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);
#endif

	cppcli::Option opt(argc, argv);

	auto cParam = opt("-c", "--config", "configure filepath, dtcfg.yaml as default", false);
	auto lParam = opt("-l", "--logcfg", "logging configure filepath, logcfgbt.yaml as default", false);

	auto hParam = opt("-h", "--help", "gain help doc", false)->asHelpParam();

	opt.parse();

	if (hParam->exists())
		return 0;

	std::string filename;
	if (lParam->exists())
		filename = lParam->get<std::string>();
	else
		filename = "./logcfg.yaml";

	WtUftRunner runner;
	runner.init(filename);

	if (cParam->exists())
		filename = cParam->get<std::string>();
	else
		filename = "./config.yaml";
	runner.config(filename);

	runner.run(false);
	return 0;
}

