#include "tests.h"

namespace tests {

namespace {

const protore::DiffTest g_tests[] = {
    {"core::Report", &TestReport},
    {"math::TraceVector", &TestTraceVector},
    {"core::HashString/HashFileName", &TestNameHash},
    {"core::CementLibrary lookup", &TestCementLookup},
    {"core::CementFileRangeInit", &TestCementFileRange},
};

}  // namespace

const protore::DiffTest* All(size_t* count) {
    *count = sizeof(g_tests) / sizeof(g_tests[0]);
    return g_tests;
}

}  // namespace tests
