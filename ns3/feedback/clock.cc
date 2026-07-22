// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 1 (R-CLOCK). See clock.h for the full
// header, the clean-room provenance statement, and the licensing caveat.

#include "clock.h"

#include "ns3/simulator.h"

namespace syncsim {

Clock::Clock(double driftPpm, ns3::Time epoch, ns3::Time localBase)
    : m_t0(epoch == ns3::Time::Min() ? ns3::Simulator::Now() : epoch),
      m_localBase(localBase),
      m_rate(1.0 + driftPpm / 1e6)
{
}

ns3::Time
Clock::GetLocalTimeAt(ns3::Time global) const
{
    // local = localBase + rate * (global - t0).
    // Computed via GetSeconds() (double) so the ppm rate multiplies cleanly.
    // ns-3's default ns resolution resolves the drift magnitudes of interest
    // here (>= ~1 us over seconds) with many orders of magnitude to spare.
    double elapsed = (global - m_t0).GetSeconds();
    return m_localBase + ns3::Seconds(m_rate * elapsed);
}

ns3::Time
Clock::GetLocalTime() const
{
    return GetLocalTimeAt(ns3::Simulator::Now());
}

double
Clock::GetDriftPpm() const
{
    return (m_rate - 1.0) * 1e6;
}

void
Clock::Rebase()
{
    ns3::Time now = ns3::Simulator::Now();
    m_localBase = GetLocalTimeAt(now);
    m_t0 = now;
}

void
Clock::AdjustRate(double ppmDelta)
{
    double oldPpm = GetDriftPpm();
    // Re-anchor first so the reported local time is C0-continuous across the
    // rate change: only the slope from now on is altered.
    Rebase();
    m_rate += ppmDelta / 1e6;
    m_steerTrace(ns3::Simulator::Now(), oldPpm, GetDriftPpm(), ns3::Time(0));
}

void
Clock::AdjustOffset(ns3::Time offsetDelta)
{
    // A pure phase step: shift the local-time base. Rate/slope unchanged.
    m_localBase += offsetDelta;
    double ppm = GetDriftPpm();
    m_steerTrace(ns3::Simulator::Now(), ppm, ppm, offsetDelta);
}

ns3::Time
Clock::Sample()
{
    ns3::Time now = ns3::Simulator::Now();
    ns3::Time local = GetLocalTimeAt(now);
    m_sampleTrace(now, local, GetDriftPpm());
    return local;
}

void
Clock::ConnectSampleTrace(ns3::Callback<void, ns3::Time, ns3::Time, double> cb)
{
    m_sampleTrace.ConnectWithoutContext(cb);
}

void
Clock::ConnectSteerTrace(ns3::Callback<void, ns3::Time, double, double, ns3::Time> cb)
{
    m_steerTrace.ConnectWithoutContext(cb);
}

} // namespace syncsim
