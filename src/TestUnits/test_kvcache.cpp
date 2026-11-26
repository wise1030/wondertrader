#include "../Share/WtKVCache.hpp"
#include "../Share/IniHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/fmtlib.h"
#include "gtest/gtest/gtest.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

USING_NS_WTP;

TEST(test_kvcache, test_usage)
{
	{
		WtKVCache cache;
		EXPECT_TRUE(cache.init("./simplecache.dat", 20220325, [](const char* msg) {
			printf(msg);
			printf("\n");
		}));
		cache.clear();
	
		EXPECT_STREQ(cache.get("test_null"), "");
	
		cache.put("test", "this is a test string", 0, [](const char* msg) {
			printf(msg);
			printf("\n");
		});

		EXPECT_STREQ(cache.get("test"), "this is a test string");
	
		EXPECT_EQ(cache.size(), 1);
	}

	{
		WtKVCache cache;
		EXPECT_TRUE(cache.init("./simplecache.dat", 20220326, [](const char* msg) {
			printf(msg);
			printf("\n");
		}));
		EXPECT_EQ(cache.size(), 0);
	}
}

TEST(test_kvcache, test_perform)
{
	WtKVCache cache;
	cache.init("./scperform.dat", 20220325, [](const char* msg) {
		printf(msg);
		printf("\n");
	});
	cache.clear();

	IniHelper ini;
	ini.load("./scperform.ini");
	ini.removeSection("test");
	ini.save();

	uint32_t times = 10000;

	TimeUtils::Ticker ticker;
	char buffer[16] = { 0 };

	//œ»≤‚ ‘simplecache
	for(uint32_t i = 0; i < times; i++)
	{
		char* s = fmt::format_to(buffer, "{}", i);
		s[0] = '\0';
		cache.put(buffer, buffer);
	}

	for (uint32_t i = 0; i < times; i++)
	{
		char* s = fmt::format_to(buffer, "{}", i);
		s[0] = '\0';
		cache.get(buffer);
	}
	uint64_t a = ticker.nano_seconds();

	//‘Ÿ≤‚ ‘ini
	ticker.reset();
	for (uint32_t i = 0; i < times; i++)
	{
		char* s = fmt::format_to(buffer, "{}", i);
		s[0] = '\0';
		ini.writeString("test", buffer, buffer);
		//ini.save();
	}

	for (uint32_t i = 0; i < times; i++)
	{
		char* s = fmt::format_to(buffer, "{}", i);
		s[0] = '\0';
		ini.readString("test", buffer);
	}
	uint64_t b = ticker.nano_seconds();

	fmt::print("cache: {} - ini: {}\n", a, b);
}

TEST(test_kvcache, clear_without_init_does_not_deadlock)
{
	WtKVCache cache;
	cache.clear();
	EXPECT_EQ(cache.size(), 0u);
	EXPECT_EQ(cache.capacity(), 0u);
	EXPECT_FALSE(cache.has("missing"));
	EXPECT_STREQ(cache.get("missing"), "");
}

TEST(test_kvcache, returned_value_survives_resize)
{
	const char* path = "./kvcache_resize_test.dat";
	std::remove(path);
	WtKVCache cache;
	ASSERT_TRUE(cache.init(path, 20261126));
	cache.clear();
	for (uint32_t i = 0; i < SIZE_STEP; ++i)
	{
		const std::string key = fmt::format("key{}", i);
		const std::string val = fmt::format("value{}", i);
		cache.put(key.c_str(), val.c_str());
	}
	ASSERT_EQ(cache.capacity(), static_cast<uint32_t>(SIZE_STEP));
	const char* snapshot = cache.get("key0");
	for (uint32_t i = SIZE_STEP; i < 3 * SIZE_STEP; ++i)
	{
		const std::string key = fmt::format("key{}", i);
		cache.put(key.c_str(), key.c_str());
	}
	EXPECT_GE(cache.capacity(), static_cast<uint32_t>(2 * SIZE_STEP));
	EXPECT_STREQ(snapshot, "value0");
	for (uint32_t i = 0; i < 3 * SIZE_STEP; ++i)
	{
		const std::string key = fmt::format("key{}", i);
		EXPECT_TRUE(cache.has(key.c_str()));
	}
}

TEST(test_kvcache, concurrent_read_resize_and_overwrite)
{
	const char* path = "./kvcache_concurrent_test.dat";
	std::remove(path);
	WtKVCache cache;
	ASSERT_TRUE(cache.init(path, 20261126));
	cache.clear();
	cache.put("key0", "value0");

	std::atomic<bool> done{false};
	std::atomic<bool> valid{true};
	std::thread reader([&]() {
		while (!done.load(std::memory_order_acquire))
		{
			const char* value = cache.get("key0");
			if (std::strcmp(value, "value0") != 0
				&& std::strcmp(value, "updated0") != 0)
				valid.store(false, std::memory_order_release);
			(void)cache.get("missing");
			(void)cache.size();
		}
	});
	for (uint32_t i = 1; i < 2000; ++i)
	{
		const std::string key = fmt::format("key{}", i);
		cache.put(key.c_str(), key.c_str());
	}
	cache.put("key0", "updated0");
	done.store(true, std::memory_order_release);
	reader.join();

	EXPECT_TRUE(valid.load());
	EXPECT_EQ(cache.size(), 2000u);
	EXPECT_STREQ(cache.get("key0"), "updated0");
	EXPECT_GE(cache.capacity(), 2000u);
}