#include "Threat.h"

#include <cstdio>

std::atomic<uint16_t> Threat::createdThreatNumber { 0 };

Threat::Threat(const float azimuth, const float elevation) :
    m_threatID(createdThreatNumber.fetch_add(1, std::memory_order_relaxed)),
    m_azimuth(azimuth),
    m_elevation(elevation) {
};

std::uint16_t Threat::getThreatId() const { return m_threatID; }

float Threat::getAzimuth() const { return m_azimuth; }

void Threat::setAzimuth(const float azimuth) { this->m_azimuth = azimuth; }

float Threat::getElevation() const { return m_elevation; }

void Threat::setElevation(const float elevation) { this->m_elevation = elevation; }


// =================== MISSILE THREAT =======================
MissileThreat::MissileThreat(const float azimuth, const float elevation, const double speed, const double velocity) :
    Threat(azimuth, elevation), m_speed(speed), m_velocity(velocity) {};


int MissileThreat::format(char* buf, const std::size_t cap) const noexcept {
    return std::snprintf(buf, cap,
        "[MISSILE] id=%u az=%s el=%s speed=%s velocity=%s",
        static_cast<unsigned>(m_threatID),
        toText(m_azimuth).data(), toText(m_elevation).data(),
        toText(m_speed).data(), toText(m_velocity).data());
}

double MissileThreat::getSpeed() const { return m_speed; }

double MissileThreat::getVelocity() const { return m_velocity; }

void MissileThreat::setSpeed(const double newSpeed) { m_speed = newSpeed; }

void MissileThreat::setVelocity(const double newVelocity) { m_velocity = newVelocity; }


// =================== RADAR THREAT ==========================
RadarThreat::RadarThreat(const float azimuth, const float elevation, const double frequency, const double signalStrength) :
    Threat(azimuth, elevation), m_frequency(frequency), m_signalStrength(signalStrength) {};

int RadarThreat::format(char* buf, const std::size_t cap) const noexcept {
    return std::snprintf(buf, cap,
        "[RADAR] id=%u az=%s el=%s freq=%s signal=%s",
        static_cast<unsigned>(m_threatID),
        toText(m_azimuth).data(), toText(m_elevation).data(),
        toText(m_frequency).data(), toText(m_signalStrength).data());
}

double RadarThreat::getFrequency() const { return m_frequency; }

double RadarThreat::getSignalStrength() const { return m_signalStrength; }

void RadarThreat::setFrequency(const double newFrequency) { m_frequency = newFrequency; }

void RadarThreat::setSignalStrength(const double newSignalStrength) { m_signalStrength = newSignalStrength; }


// =================== LASER THREAT ==========================
LaserThreat::LaserThreat(const float azimuth, const float elevation, const double wavelengthNm, const double pulseRepetitionHz) :
    Threat(azimuth, elevation), m_wavelengthNm(wavelengthNm), m_pulseRepetitionHz(pulseRepetitionHz) {};

int LaserThreat::format(char* buf, const std::size_t cap) const noexcept {
    return std::snprintf(buf, cap,
        "[LASER] id=%u az=%s el=%s wavelength=%s prf=%s",
        static_cast<unsigned>(m_threatID),
        toText(m_azimuth).data(), toText(m_elevation).data(),
        toText(m_wavelengthNm).data(), toText(m_pulseRepetitionHz).data());
}

double LaserThreat::getWavelength() const { return m_wavelengthNm; }

double LaserThreat::getPulseRepetition() const { return m_pulseRepetitionHz; }

void LaserThreat::setWavelength(const double newWavelengthNm) { m_wavelengthNm = newWavelengthNm; }

void LaserThreat::setPulseRepetition(const double newPulseRepetitionHz) { m_pulseRepetitionHz = newPulseRepetitionHz; }
