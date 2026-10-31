// Minimal dependency-free test harness.
#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace testing {

struct TestCase {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<TestCase> &registry()
{
	static std::vector<TestCase> tests;
	return tests;
}

inline int &failures()
{
	static int count = 0;
	return count;
}

struct Registrar {
	Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

// Polls `cond` until true or the timeout elapses.
inline bool eventually(const std::function<bool()> &cond, int timeoutMs = 3000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (cond())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return cond();
}

} // namespace testing

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                        \
	static void name();                                                               \
	static testing::Registrar TEST_CONCAT(registrar_, name)(#name, name);             \
	static void name()

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			testing::failures()++;                                             \
		}                                                                          \
	} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
