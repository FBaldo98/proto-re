// Registry of differential tests, one per reconstructed function.
#pragma once

#include <cstddef>

#include "../host/difftest.h"

namespace tests {

const protore::DiffTest* All(size_t* count);

void TestReport(protore::DiffResult* r);
void TestTraceVector(protore::DiffResult* r);

}  // namespace tests
