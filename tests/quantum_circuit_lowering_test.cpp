#include "oreslang/runtime/QuantumCircuitIR.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

static void check(bool ok) {
    if (!ok) throw std::runtime_error("quantum circuit test failed");
}
template<class Exception, class F> void expect(F fn) {
    try { fn(); } catch (const Exception&) { return; }
    throw std::runtime_error("expected quantum circuit admission failure");
}

int main() {
    using namespace oreslang::quantum;
    auto bell = Builder(2, 2).h(0).cx(0, 1).measure(0, 0).measure(1, 1).build();
    auto probs = simulate_probabilities(bell);
    check(probs.size() == 4);
    check(std::abs(probs[0] - .5) < 1e-12);
    check(probs[1] == 0 && probs[2] == 0);
    check(std::abs(probs[3] - .5) < 1e-12);
    auto qasm = lower_openqasm3(bell);
    check(qasm.find("OPENQASM 3.0;") == 0);
    check(qasm.find("cx q[0], q[1];") != std::string::npos);
    check(qasm.find("c[1] = measure q[1];") != std::string::npos);
    const auto shots = simulate_shots(bell, 1000, 42);
    check(shots[1] == 0 && shots[2] == 0);
    check(std::accumulate(shots.begin(), shots.end(), std::uint64_t{0}) == 1000);
    check(shots == simulate_shots(bell, 1000, 42));
    auto xstate = Builder(1, 1).x(0).measure(0, 0).build();
    check(simulate_probabilities(xstate) == std::vector<double>({0.0, 1.0}));
    expect<std::invalid_argument>([] { (void)Builder(17, 1); });
    expect<std::invalid_argument>([] { (void)Builder(2, 2).cx(0, 0).measure(0, 0).build(); });
    expect<std::invalid_argument>([] { (void)Builder(2, 2).measure(0, 0).measure(0, 1).build(); });
    expect<std::invalid_argument>([] { (void)Builder(2, 2).measure(0, 0).measure(1, 0).build(); });
    expect<std::invalid_argument>([] { (void)Builder(2, 2).measure(0, 0).h(1).build(); });
    expect<std::invalid_argument>([] { (void)Builder(2, 2).h(0).build(); });
    expect<std::invalid_argument>([&] { (void)simulate_shots(bell, 1000001, 1); });
    // A 16-bit distribution used to cost billions of linear-scan probes;
    // it must be sampled with bounded work and remain seed-reproducible.
    Builder broad(16, 16);
    for (int bit = 0; bit < 16; ++bit) broad.h(bit);
    for (int bit = 0; bit < 16; ++bit) broad.measure(bit, bit);
    const auto broad_circuit = broad.build();
    const auto samples = simulate_shots(broad_circuit, 100000, 918273);
    check(std::accumulate(samples.begin(), samples.end(), std::uint64_t{0}) == 100000);
    check(samples == simulate_shots(broad_circuit, 100000, 918273));

    // Prefix sums with zero-weight outcomes must never select impossible bins.
    const auto basis = Builder(2, 2).x(0).measure(0, 0).measure(1, 1).build();
    const auto exact = simulate_shots(basis, 100000, 7);
    check(exact[1] == 100000 && exact[0] == 0 && exact[2] == 0 && exact[3] == 0);

    // Constructor-only checking allowed unbounded intermediate gate growth.
    Builder quota(1, 1);
    for (int i = 0; i < 1024; ++i) quota.x(0);
    expect<std::invalid_argument>([&] { quota.measure(0, 0); });
    std::cout << "native quantum circuit QASM+statevector validation passed\n";
}
