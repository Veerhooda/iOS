// op2gpu-probe: the G1 checks on the phone, through OP2GPU. Runs as a launchd boot task (it replaces
// /usr/libexec/tzinit, see tools/deploy-probe.sh); every line goes to the kernel log, which ramoops keeps.
#include <cstdio>
#include <string>
#include <unistd.h>

#include "op2gpu/checks.hpp"
#include "op2gpu/gpu.hpp"

int main() {
    std::string why;
    std::unique_ptr<op2gpu::Transport> t = op2gpu::openKernelGpu(&why);
    if (!t) {
        fprintf(stderr, "op2gpu-probe: %s\n", why.c_str());
        return 2;
    }
    op2gpu::Gpu gpu(std::move(t));
    gpu.log("probe: G1 start (pid " + std::to_string(getpid()) + ")");
    int f = op2gpu::runBlitChecks(gpu, [&](const std::string& l) {
        gpu.log("probe: " + l);
        printf("%s\n", l.c_str());
    });
    gpu.log(f == 0 ? "probe: G1 PASSED" : "probe: G1 FAILED (" + std::to_string(f) + " checks)");
    return f == 0 ? 0 : 1;
}
