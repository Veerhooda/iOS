// The G1 checks, the same on the fake GPU (tests/) and on the phone (tools/probe.cpp): every result a line, every
// byte compared with what the CPU computes.
#pragma once

#include <functional>
#include <string>

#include "op2gpu/gpu.hpp"

namespace op2gpu {

// runs the checks; SAY gets one line per check ("ok ..." or "FAIL ..."); returns the number of failures
int runBlitChecks(Gpu& gpu, const std::function<void(const std::string&)>& say);

}  // namespace op2gpu
