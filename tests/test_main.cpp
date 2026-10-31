#include "test_framework.hpp"

int main()
{
	int failedTests = 0;
	for (const auto &test : testing::registry()) {
		const int before = testing::failures();
		std::printf("[ RUN  ] %s\n", test.name);
		test.fn();
		const bool ok = testing::failures() == before;
		std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", test.name);
		if (!ok)
			failedTests++;
	}
	std::printf("\n%zu tests, %d failed, %d failed checks\n", testing::registry().size(), failedTests,
		    testing::failures());
	return failedTests == 0 ? 0 : 1;
}
