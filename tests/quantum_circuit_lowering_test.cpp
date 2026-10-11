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
    std::cout << "native quantum circuit QASM+statevector validation passed\n";
}
