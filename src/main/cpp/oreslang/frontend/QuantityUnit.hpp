#pragma once

#include "llvm/ADT/APInt.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang {

// Unit scalars are reduced to 256 bits per component. Intermediate products
// in an exact conversion can be 576 bits (i64 magnitude and two components).
class QuantityUnit {
    static constexpr unsigned WorkingBits = 640;
    std::map<std::string, int> powers_;
    llvm::APInt numerator_{WorkingBits, 1};
    llvm::APInt denominator_{WorkingBits, 1};

    QuantityUnit(std::map<std::string, int> powers,
                 llvm::APInt numerator, llvm::APInt denominator)
        : powers_(std::move(powers)), numerator_(std::move(numerator)),
          denominator_(std::move(denominator)) {
        if (numerator_.isZero() || denominator_.isZero())
            throw std::runtime_error("E_UNIT_SCALE: unit scales must be positive");
        auto a = numerator_;
        auto b = denominator_;
        while (!b.isZero()) {
            auto next = a.urem(b);
            a = std::move(b);
            b = std::move(next);
        }
        numerator_ = numerator_.udiv(a);
        denominator_ = denominator_.udiv(a);
        if (powers_.size() > 32 || numerator_.getActiveBits() > 256
                || denominator_.getActiveBits() > 256)
            throw std::runtime_error("E_UNIT_LIMIT: normalized quantity exceeds supported limits");
    }

public:
    QuantityUnit() = default;

    static QuantityUnit base(const std::string& name) {
        return QuantityUnit({{name, 1}}, llvm::APInt(WorkingBits, 1),
                            llvm::APInt(WorkingBits, 1));
    }

    bool scalar() const {
        return powers_.empty() && numerator_.isOne() && denominator_.isOne();
    }

    bool same_dimension(const QuantityUnit& other) const {
        return powers_ == other.powers_;
    }

    void require_compatible(const QuantityUnit& other) const {
        if (!same_dimension(other))
            throw std::runtime_error("E_UNIT_DIMENSION: incompatible quantity dimensions");
        if (numerator_ != other.numerator_ || denominator_ != other.denominator_)
            throw std::runtime_error("E_UNIT_SCALE: quantities require an explicit scale conversion");
    }

    QuantityUnit scale(std::int64_t factor, bool divide) const {
        if (factor <= 0) throw std::runtime_error("E_UNIT_SCALE: unit scales must be positive");
        const llvm::APInt magnitude(WorkingBits, static_cast<std::uint64_t>(factor));
        return divide ? QuantityUnit(powers_, numerator_, denominator_ * magnitude)
                      : QuantityUnit(powers_, numerator_ * magnitude, denominator_);
    }

    QuantityUnit combine(const QuantityUnit& other, bool divide) const {
        auto powers = powers_;
        for (const auto& entry : other.powers_) {
            auto& exponent = powers[entry.first];
            exponent += divide ? -entry.second : entry.second;
            if (exponent < -64 || exponent > 64)
                throw std::runtime_error("E_UNIT_LIMIT: dimension exponent exceeds 64");
            if (exponent == 0) powers.erase(entry.first);
        }
        return divide
                ? QuantityUnit(std::move(powers), numerator_ * other.denominator_, denominator_ * other.numerator_)
                : QuantityUnit(std::move(powers), numerator_ * other.numerator_, denominator_ * other.denominator_);
    }

    std::int64_t convert_exact(std::int64_t value, const QuantityUnit& target) const {
        if (!same_dimension(target))
            throw std::runtime_error("E_UNIT_DIMENSION: conversion requires the same dimension");
        const llvm::APInt signed_value(WorkingBits, static_cast<std::uint64_t>(value), true);
        const auto scaled = signed_value * numerator_ * target.denominator_;
        const auto divisor = denominator_ * target.numerator_;
        if (!scaled.srem(divisor).isZero())
            throw std::runtime_error("E_UNIT_INEXACT: conversion requires an integral result");
        const auto result = scaled.sdiv(divisor);
        if (!result.isSignedIntN(64))
            throw std::runtime_error("E_UNIT_RANGE: conversion result is outside signed i64");
        return result.getSExtValue();
    }
};

} // namespace oreslang
