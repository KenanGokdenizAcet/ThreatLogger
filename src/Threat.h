#ifndef ASELSAN_REHIS_THREAT_H
#define ASELSAN_REHIS_THREAT_H

#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>

class Threat {
public:
    virtual int format(char* buf, std::size_t cap) const noexcept = 0;

    virtual ~Threat() = default;

    [[nodiscard]] std::uint16_t getThreatId() const;

    [[nodiscard]] float getAzimuth() const;
    void setAzimuth(float azimuth);

    [[nodiscard]] float getElevation() const;
    void setElevation(float elevation);

protected:
    Threat(float azimuth, float elevation);

    // A number as "%g" text (6 significant digits), written by std::to_chars instead of snprintf.
    // snprintf's float formatting can allocate from the heap for some values;
    // to_chars never allocates. Longest: "-1.23457e+308" (13 chars) + '\0'.
    [[nodiscard]] static std::array<char, 16> toText(const double value) noexcept {
        std::array<char, 16> text{};
        const auto result = std::to_chars(text.data(), text.data() + text.size() - 1, value,
                                          std::chars_format::general, 6);
        *result.ptr = '\0';
        return text;
    }

    const uint16_t m_threatID;
    float m_azimuth;
    float m_elevation;

private:
    static std::atomic<uint16_t> createdThreatNumber; // for assigning a unique threatID to each threat
};

class MissileThreat : public Threat {
public:
    MissileThreat(float azimuth, float elevation, double speed, double velocity);

    [[nodiscard]] double getSpeed() const;
    void setSpeed(double newSpeed);

    [[nodiscard]] double getVelocity() const;
    void setVelocity(double newVelocity);

    int format(char* buf, std::size_t cap) const noexcept override;

private:
    double m_speed;
    double m_velocity;
};

class RadarThreat: public Threat {
public:
    RadarThreat(float azimuth, float elevation, double frequency, double signalStrength);

    [[nodiscard]] double getFrequency() const;
    void setFrequency(double newFrequency);

    [[nodiscard]] double getSignalStrength() const;
    void setSignalStrength(double newSignalStrength);

    int format(char* buf, std::size_t cap) const noexcept override;
private:
    double m_frequency;
    double m_signalStrength;
};

// Laser warning: a rangefinder / designator / beam-rider illuminating the platform.
class LaserThreat : public Threat {
public:
    LaserThreat(float azimuth, float elevation, double wavelengthNm, double pulseRepetitionHz);

    [[nodiscard]] double getWavelength() const;
    void setWavelength(double newWavelengthNm);

    [[nodiscard]] double getPulseRepetition() const;
    void setPulseRepetition(double newPulseRepetitionHz);

    int format(char* buf, std::size_t cap) const noexcept override;

private:
    double m_wavelengthNm;
    double m_pulseRepetitionHz;
};
#endif //ASELSAN_REHIS_THREAT_H
