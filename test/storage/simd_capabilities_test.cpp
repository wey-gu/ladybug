// Controlled CPU/OS inputs; never execute a kernel selected from fictional CPU bits.
#define SIMSIMD_DYNAMIC_DISPATCH 0
#define SIMSIMD_NATIVE_F16 0
#define SIMSIMD_NATIVE_BF16 0
#define SIMSIMD_TARGET_NEON 0
#define SIMSIMD_TARGET_NEON_F16 0
#define SIMSIMD_TARGET_NEON_BF16 0
#define SIMSIMD_TARGET_NEON_I8 0
#define SIMSIMD_TARGET_SVE_F16 0
#define SIMSIMD_TARGET_SVE_BF16 0
#define SIMSIMD_TARGET_SVE_I8 0
#define SIMSIMD_TARGET_SVE2P1 0
#define SIMSIMD_TARGET_SVE 0
#define SIMSIMD_TARGET_SVE2 0
#define SIMSIMD_TARGET_GENOA 0
#define SIMSIMD_TARGET_SAPPHIRE 0
#define SIMSIMD_TARGET_TURIN 0
#define SIMSIMD_TARGET_SIERRA 0
#if defined(__x86_64__) || defined(_M_X64)
#define SIMSIMD_TARGET_HASWELL 1
#define SIMSIMD_TARGET_SKYLAKE 1
#define SIMSIMD_TARGET_ICE 1
#endif
#include <simsimd.h>
#include <cstdio>
#include <initializer_list>

static unsigned failures = 0;
static void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); ++failures; }
}
int main() {
    const unsigned all = ~0u;
    const auto caps = [](unsigned ecx, unsigned ebx, simsimd_u64_t xcr0) {
        return _simsimd_capabilities_x86_from_registers(ecx, ebx, ~0u, ~0u, ~0u, ~0u, xcr0);
    };
    for (unsigned bit : {26u, 27u, 28u}) {
        require(!_simsimd_x86_xcr0_available(all & ~(1u << bit)), "Missing prerequisites must forbid XGETBV");
        require(caps(all & ~(1u << bit), all, 0xe6) == simsimd_cap_serial_k,
            "CPU XSAVE, OSXSAVE and AVX prerequisites must all hold");
    }
    require(caps(all, all, 0) == simsimd_cap_serial_k, "Disabled OS state must use serial");
    for (unsigned bit : {1u, 2u}) {
        require(caps(all, all, 0xe6 & ~(1ull << bit)) == simsimd_cap_serial_k,
            "Missing XMM or YMM state must use serial");
    }
    const auto avx2 = static_cast<simsimd_capability_t>(simsimd_cap_serial_k | simsimd_cap_haswell_k);
    require(caps(all, all, 0x6) == avx2, "YMM-only state cannot authorize AVX512");
    for (unsigned bit : {5u, 6u, 7u}) {
        require(caps(all, all, 0xe6 & ~(1ull << bit)) == avx2,
            "Missing any opmask or ZMM state must disable every AVX512 generation");
    }
    for (unsigned bit : {16u, 17u, 28u, 30u, 31u}) {
        require(caps(all, all & ~(1u << bit), 0xe6) == avx2,
            "Missing any Skylake kernel subset must disable all AVX512 descendants");
    }
    for (unsigned bit : {5u, 8u}) {
        const auto admitted = caps(all, all & ~(1u << bit), 0xe6);
        const auto allowed = bit == 5 ? simsimd_cap_serial_k : avx2;
        require(admitted == allowed, "AVX2 and BMI2 CPU subsets must guard descendants");
    }
    for (unsigned bit : {12u, 29u}) {
        require(caps(all & ~(1u << bit), all, 0xe6) == simsimd_cap_serial_k,
            "Missing FMA/F16C must not authorize kernels compiled with those instructions");
    }
    const auto without_popcnt = caps(all & ~(1u << 23), all, 0xe6);
    const auto without_lzcnt = _simsimd_capabilities_x86_from_registers(all, all, all, all, all, all & ~(1u << 5), 0xe6);
    for (auto admitted : {without_popcnt, without_lzcnt}) {
        require(!(admitted & (simsimd_cap_ice_k | simsimd_cap_turin_k)),
            "Sparse kernel POPCNT/LZCNT requirements must be admitted");
        require(admitted & simsimd_cap_skylake_k, "Missing sparse features must preserve safe Skylake kernels");
    }
    require(caps(all, all, 0xe6) != avx2, "Complete CPU/OS capabilities must retain AVX512");

    simsimd_kernel_punned_t kernel = nullptr;
    simsimd_capability_t used = simsimd_cap_any_k;
    simsimd_find_kernel_punned(simsimd_metric_l2sq_k, simsimd_datatype_f32_k,
        caps(all, all, 0), simsimd_cap_any_k, &kernel, &used);
    require(kernel != nullptr && used == simsimd_cap_serial_k,
        "Real selector must choose serial with disabled OS state");
    if (kernel && used == simsimd_cap_serial_k) {
        const float a[] = {1, 2, 3}, b[] = {0, 0, 0};
        simsimd_distance_t distance = 0;
        reinterpret_cast<simsimd_metric_dense_punned_t>(kernel)(a, b, 3, &distance);
        require(distance == 14, "Safe scalar kernel must still produce correct distance");
    }
#if SIMSIMD_TARGET_HASWELL
    simsimd_find_kernel_punned(simsimd_metric_l2sq_k, simsimd_datatype_f32_k,
        caps(all, all, 0x6), simsimd_cap_any_k, &kernel, &used);
    require(kernel != nullptr && used == simsimd_cap_haswell_k,
        "Real selector must prefer admitted AVX2 and exclude AVX512 when OS only saves YMM");
#endif
    // Only execute a kernel admitted by actual hardware and OS state.
    simsimd_find_kernel_punned(simsimd_metric_l2sq_k, simsimd_datatype_f32_k,
        simsimd_capabilities(), simsimd_cap_any_k, &kernel, &used);
    require(kernel != nullptr, "Actual host must retain a usable metric");
    if (kernel) {
        const float a[] = {1, 2, 3}, b[] = {0, 0, 0};
        simsimd_distance_t distance = 0;
        reinterpret_cast<simsimd_metric_dense_punned_t>(kernel)(a, b, 3, &distance);
        require(distance == 14, "Actually admitted kernel must produce correct distance");
    }
    std::printf("SimdCapabilitiesTest: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
