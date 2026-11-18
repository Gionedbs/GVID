#pragma once
#include <algorithm>
#include <cstdint>
namespace vs {
// Velocità in sedicesimi: 16 = 1x, -16 = 1x indietro, 0 = fermo.
inline constexpr int kSpeedNormal = 16;

// Posizione a campi. Accumulare in sedicesimi evita derive alle velocità frazionarie.
class FieldCursor {
public:
    FieldCursor(int64_t firstField, int64_t lastField)
        : first_(firstField * kSpeedNormal), last_(lastField * kSpeedNormal), pos_(first_) {}

    void seek(int64_t field) { pos_ = std::clamp(field * kSpeedNormal, first_, last_); }

    // Avanza di un campo in uscita. Restituisce false se ha raggiunto un limite.
    bool advance(int speed) {
        const int64_t next = pos_ + speed;
        pos_ = std::clamp(next, first_, last_);
        return pos_ == next;
    }

    int64_t field() const { return pos_ / kSpeedNormal; }

private:
    int64_t first_, last_, pos_;
};
}  // namespace vs
