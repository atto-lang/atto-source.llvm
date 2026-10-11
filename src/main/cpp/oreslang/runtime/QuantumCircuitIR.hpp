#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::quantum {

// Native typed circuit IR, executable state-vector simulator and OpenQASM 3
// lowering. Quantum hardware dispatch is NOT implemented or implicitly simulated.
enum class Op { X, H, CX, Measure };
struct Gate {
    Op op;
    int target;
    int control = -1;
    int classical_bit = -1;
};

class Circuit {
public:
    Circuit(int qubits, int classical_bits, std::vector<Gate> gates)
        : qubits_(qubits), classical_bits_(classical_bits), gates_(std::move(gates)) {
        if (qubits < 1 || qubits > 16 || classical_bits < 1 || classical_bits > 16)
            throw std::invalid_argument("circuit exceeds bounded simulator profile");
        if (gates_.size() > 1024)
            throw std::invalid_argument("circuit exceeds gate quota");
        std::vector<bool> measured_qubit(static_cast<std::size_t>(qubits), false);
        std::vector<bool> filled_classical(static_cast<std::size_t>(classical_bits), false);
        bool terminal = false;
        for (const auto& gate : gates_) {
            if (gate.target < 0 || gate.target >= qubits)
                throw std::invalid_argument("invalid qubit index");
            if (gate.op == Op::Measure) {
                terminal = true;
                if (gate.control != -1 || gate.classical_bit < 0
                    || gate.classical_bit >= classical_bits
                    || measured_qubit[static_cast<std::size_t>(gate.target)]
                    || filled_classical[static_cast<std::size_t>(gate.classical_bit)])
                    throw std::invalid_argument("invalid duplicate measurement or aliased classical bit");
                measured_qubit[static_cast<std::size_t>(gate.target)] = true;
                filled_classical[static_cast<std::size_t>(gate.classical_bit)] = true;
            } else {
                if (terminal) throw std::invalid_argument("gate after terminal measurement");
                if (gate.classical_bit != -1
                        || (gate.op != Op::CX && gate.control != -1)
                        || (gate.op == Op::CX && (gate.control < 0
                            || gate.control >= qubits || gate.control == gate.target)))
                    throw std::invalid_argument("malformed gate operand");
            }
        }
        if (!terminal) throw std::invalid_argument("at least one terminal measurement required");
    }
    int qubits() const noexcept { return qubits_; }
    int classical_bits() const noexcept { return classical_bits_; }
    const std::vector<Gate>& gates() const noexcept { return gates_; }

private:
    int qubits_;
    int classical_bits_;
    std::vector<Gate> gates_;
};

class Builder {
public:
    Builder(int qubits, int classical_bits)
        : qubits_(qubits), classical_bits_(classical_bits) {
        if (qubits < 1 || qubits > 16 || classical_bits < 1 || classical_bits > 16)
            throw std::invalid_argument("circuit dimensions exceed simulator profile");
    }
    Builder& x(int target) { gates_.push_back({Op::X, target, -1, -1}); return *this; }
    Builder& h(int target) { gates_.push_back({Op::H, target, -1, -1}); return *this; }
    Builder& cx(int control, int target) {
        gates_.push_back({Op::CX, target, control, -1}); return *this;
    }
    Builder& measure(int qubit, int classical_bit) {
        gates_.push_back({Op::Measure, qubit, -1, classical_bit}); return *this;
    }
    Circuit build() const { return Circuit(qubits_, classical_bits_, gates_); }
private:
    int qubits_, classical_bits_;
    std::vector<Gate> gates_;
};

inline std::string lower_openqasm3(const Circuit& circuit) {
    std::string output = "OPENQASM 3.0;\ninclude \"stdgates.inc\";\n"
        "qubit[" + std::to_string(circuit.qubits()) + "] q;\n"
        "bit[" + std::to_string(circuit.classical_bits()) + "] c;\n";
    for (const auto& g : circuit.gates()) {
        switch (g.op) {
        case Op::X: output += "x q[" + std::to_string(g.target) + "];\n"; break;
        case Op::H: output += "h q[" + std::to_string(g.target) + "];\n"; break;
        case Op::CX:
            output += "cx q[" + std::to_string(g.control) + "], q["
                    + std::to_string(g.target) + "];\n"; break;
        case Op::Measure:
            output += "c[" + std::to_string(g.classical_bit) + "] = measure q["
                    + std::to_string(g.target) + "];\n"; break;
        }
    }
    return output;
}

inline std::vector<double> simulate_probabilities(const Circuit& circuit) {
    const std::size_t states = static_cast<std::size_t>(1) << circuit.qubits();
    std::vector<double> amplitude(states, 0.0);
    amplitude[0] = 1.0;
    const double inv_sqrt_two = 1.0 / std::sqrt(2.0);
    for (const auto& g : circuit.gates()) {
        const std::size_t target = static_cast<std::size_t>(1) << g.target;
        switch (g.op) {
        case Op::X:
            for (std::size_t i = 0; i < states; ++i) {
                if (i & target) continue;
                std::swap(amplitude[i], amplitude[i | target]);
            }
            break;
        case Op::H:
            for (std::size_t i = 0; i < states; ++i) {
                if (i & target) continue;
                const auto lo = amplitude[i], hi = amplitude[i | target];
                amplitude[i] = (lo + hi) * inv_sqrt_two;
                amplitude[i | target] = (lo - hi) * inv_sqrt_two;
            }
            break;
        case Op::CX: {
            const std::size_t control = static_cast<std::size_t>(1) << g.control;
            for (std::size_t i = 0; i < states; ++i) {
                if ((i & target) || !(i & control)) continue;
                std::swap(amplitude[i], amplitude[i | target]);
            }
            break;
        }
        case Op::Measure: break;
        }
    }
    std::vector<double> probabilities(static_cast<std::size_t>(1) << circuit.classical_bits(), 0.0);
    for (std::size_t state = 0; state < states; ++state) {
        std::size_t outcome = 0;
        for (const auto& g : circuit.gates()) {
            if (g.op == Op::Measure && (state & (static_cast<std::size_t>(1) << g.target)))
                outcome |= static_cast<std::size_t>(1) << g.classical_bit;
        }
        probabilities[outcome] += amplitude[state] * amplitude[state];
    }
    return probabilities;
}

inline std::vector<std::uint64_t> simulate_shots(const Circuit& circuit,
                                                   std::uint32_t shots,
                                                   std::uint64_t seed) {
    if (shots > 1000000) throw std::invalid_argument("shot quota exceeded");
    const auto probabilities = simulate_probabilities(circuit);
    std::vector<std::uint64_t> counts(probabilities.size(), 0);
    std::mt19937_64 engine(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (std::uint32_t sample = 0; sample < shots; ++sample) {
        const double draw = uniform(engine);
        double cumulative = 0;
        std::size_t outcome = probabilities.size() - 1;
        for (std::size_t j = 0; j < probabilities.size(); ++j) {
            cumulative += probabilities[j];
            if (draw < cumulative) { outcome = j; break; }
        }
        ++counts[outcome];
    }
    return counts;
}

} // namespace oreslang::quantum
