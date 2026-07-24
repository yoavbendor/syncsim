// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 1 (R-CLOCK).
//
// Clean-room, permissively-licensed per-node steerable drift clock for ns-3.
// This is INET's ConstantDriftOscillator + ClockBase semantics reimplemented
// from first principles for ns-3: a node-local time that runs as a linear
// (constant-rate-error) function of ns-3's single global Simulator::Now().
//
// Written from scratch. The three community ns-3 clock efforts named in
// NS3_MIGRATION_SURVEY.md (the 2025 "Clock Skew Models for ns-3" conf code,
// Lagwankar's bounded-clock-skew-with-probability branch, LCA2-EPFL's
// TSN-ATS-Clocks local-time module) were NOT read or copied while writing this
// -- only their existence and general approach (as summarised in the survey)
// were known. That keeps the copyright on this file entirely ours, so it can
// carry a permissive Apache-2.0 header, distinct from ns-3 core's GPL-2.0-only
// headers on the unmodified ns-3 files this links against.
//
// Honest licensing caveat (see NS3_MIGRATION_POC_PLAN.md): the Apache-2.0
// header applies to THIS source file's copyright only. Because this class links
// against ns-3 core (GPL-2.0-only), the *combined, distributed* binary is still
// governed by GPLv2. Clean-room buys copyright ownership + freedom from any one
// fork's terms/version-lock; it does not make the ns-3 build itself permissive.
//
// Model (mirrors INET's ConstantDriftOscillator, driftRate == ppm/1e6):
//
//     local(t) = localBase + (1 + ppm/1e6) * (t - t0)
//
// where t = Simulator::Now(). A gPTP servo (Phase 2) steers a slave by calling
// AdjustRate (frequency correction) and AdjustOffset (phase/offset correction);
// both are proven here in clock-spike.cc but no servo is built yet.

#ifndef SYNCSIM_CLOCK_H
#define SYNCSIM_CLOCK_H

#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

namespace syncsim {

// A per-node local clock. Deliberately a plain C++ class driven by
// Simulator::Now() (not an ns3::Object / NetDevice / Application) -- the
// smoke-test header documents why staying close to ns-3 core and out of the
// Application/Socket layers was the only approach that behaved on this ns-3.45
// build. The clock is passive: local time is a pure function of global time,
// so there is nothing to schedule for the clock itself; the owner samples it.
class Clock
{
  public:
    // driftPpm: constant frequency error in parts-per-million (may be negative).
    // epoch:    the global time this clock is "born" at (t0). Local time equals
    //           localBase at t == epoch. Defaults to the current Simulator::Now()
    //           (0 when constructed before Simulator::Run(), the usual case).
    // localBase: local time at t == epoch (defaults to 0, i.e. local starts
    //           aligned with global and then drifts).
    explicit Clock(double driftPpm, ns3::Time epoch = ns3::Time::Min(), ns3::Time localBase = ns3::Time(0));

    // Local time now (at Simulator::Now()).
    ns3::Time GetLocalTime() const;
    // Local time this clock reports for an arbitrary global time.
    ns3::Time GetLocalTimeAt(ns3::Time global) const;

    // Current drift in ppm (i.e. (rate - 1) * 1e6). Changes as AdjustRate is called.
    double GetDriftPpm() const;

    // --- Steering (what a gPTP servo will call in Phase 2) ---------------
    // Frequency correction: change the drift rate by ppmDelta, continuously
    // (local time does NOT jump; only its future slope changes). A slave at
    // -350 ppm calling AdjustRate(+350) becomes a 0-ppm clock going forward.
    void AdjustRate(double ppmDelta);
    // Phase correction: step local time by offsetDelta immediately (a jump).
    void AdjustOffset(ns3::Time offsetDelta);

    // Read local time at Now and fire the sample trace source with
    // (global, local, ppm). The owner schedules this via Simulator::Schedule
    // to log the local-vs-global trajectory over a run.
    ns3::Time Sample();

    // Trace source hookup (this is what makes the clock "observable" the way
    // Phase 2 / analysis tooling needs). ConnectWithoutContext-style.
    void ConnectSampleTrace(ns3::Callback<void, ns3::Time, ns3::Time, double> cb);
    // Fired on every steering action: (when, oldPpm, newPpm, offsetStepApplied).
    void ConnectSteerTrace(ns3::Callback<void, ns3::Time, double, double, ns3::Time> cb);

  private:
    // Re-anchor t0/localBase to "now" without changing the reported local time,
    // so a subsequent rate change only affects the future slope (C0-continuous).
    void Rebase();

    ns3::Time m_t0;        // reference global time
    ns3::Time m_localBase; // local time at m_t0
    double m_rate;         // 1 + ppm/1e6

    ns3::TracedCallback<ns3::Time, ns3::Time, double> m_sampleTrace;
    ns3::TracedCallback<ns3::Time, double, double, ns3::Time> m_steerTrace;
};

} // namespace syncsim

#endif // SYNCSIM_CLOCK_H
