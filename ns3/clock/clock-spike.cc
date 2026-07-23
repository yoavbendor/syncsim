// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 1 (R-CLOCK) proof scenario / Gate 1.
//
// Proves the make-or-break precondition for the whole ns-3 migration: that a
// per-node, independently-drifting, *steerable* local clock can be built on
// ns-3 (which natively has only one global Simulator::Now() and no node-local
// time -- the single biggest gap flagged in NS3_MIGRATION_SURVEY.md).
//
// What is proven here, numerically (not just "PASS"):
//   (1) Two syncsim::Clock instances at the project's M1 baseline drift rates
//       -- client1 = +200 ppm, client2 = -350 ppm -- run as distinct local
//       clocks that diverge from each other AND from Simulator::Now() at
//       exactly their configured rate over an 8 s run. The divergence slope is
//       measured back out of the sampled trajectory and checked against the
//       configured ppm (this is the "real run, real numbers" bar the OMNeT++
//       M1-M5 write-ups hold themselves to).
//   (2) Steerability: mid-run (t = 4 s) a simulated servo action steers the
//       -350 ppm clock the way a gPTP slave servo will in Phase 2 --
//       AdjustOffset() nulls its accumulated phase offset (a step) and
//       AdjustRate(+350) nulls its frequency error (a slope change). The
//       sampled trajectory demonstrably bends: its offset-from-global goes from
//       tracking -350 ppm to holding ~flat at ~0. The +200 ppm clock is left
//       untouched as a control and keeps drifting.
//
// Design notes / dead ends (following smoke-topology.cc's convention of writing
// down what was tried):
//   - The Clock is a plain C++ class driven purely by Simulator::Now(), NOT an
//     ns3::Object/NetDevice/Application. That is deliberate and matches the
//     Phase-0 finding that staying close to ns-3 core and out of the
//     Application/Socket layers was the only thing that behaved on this ns-3.45
//     build. A passive clock (local time = a pure function of global time) has
//     nothing to schedule for itself; the scenario samples it on a timer.
//   - Sampling is routed THROUGH the clock's TracedCallback sample source
//     (Clock::Sample() fires it; a bound sink records it), proving the clock is
//     trace-source-observable the way Phase 2 and the analysis tooling will
//     need -- rather than just reading GetLocalTime() directly in the loop.
//   - No RNG is touched anywhere in this spike, so the run is deterministic by
//     construction (confirmed by running the binary twice: byte-identical
//     stdout). The RngSeedManager seed/run are still pinned for good hygiene
//     and parity with smoke-topology.cc.

#include "clock.h"

#include "ns3/core-module.h"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SyncsimClockSpike");

namespace
{

struct Sample
{
    int id;         // 0 = clock A (+200 ppm), 1 = clock B (-350 ppm)
    double global;  // seconds
    double local;   // seconds
    double ppm;     // reported drift at sample time
};

std::vector<Sample> g_samples;

// Bound sink connected to Clock's sample trace source. `id` is bound per clock
// so the two clocks' samples land in one time-ordered log distinguishably.
void
SampleSink(int id, Time global, Time local, double ppm)
{
    g_samples.push_back({id, global.GetSeconds(), local.GetSeconds(), ppm});
}

// Steering event log (fired by Clock on AdjustRate/AdjustOffset).
void
SteerSink(Time when, double oldPpm, double newPpm, Time offsetStep)
{
    std::cout << "[steer] t=" << std::fixed << std::setprecision(3) << when.GetSeconds()
              << "s  ppm " << std::setprecision(1) << oldPpm << " -> " << newPpm
              << "  offsetStep=" << std::setprecision(3) << (offsetStep.GetSeconds() * 1e6) << " us"
              << std::endl;
}

// Measured drift (ppm) of a clock over [samples[a], samples[b]] for a given id,
// derived purely from the sampled trajectory: ppm = d(local-global)/d(global).
// This is exactly the "offset = clock_time - sim_time" trick analyze.py uses.
double
MeasuredPpm(int id, double gStart, double gEnd)
{
    double o0 = NAN;
    double o1 = NAN;
    double t0 = 0;
    double t1 = 0;
    for (const auto& s : g_samples)
    {
        if (s.id != id)
        {
            continue;
        }
        if (std::isnan(o0) && s.global >= gStart)
        {
            o0 = s.local - s.global;
            t0 = s.global;
        }
        if (s.global <= gEnd + 1e-9)
        {
            o1 = s.local - s.global;
            t1 = s.global;
        }
    }
    return (o1 - o0) / (t1 - t0) * 1e6;
}

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 8.0;
    double sampleInterval = 0.5;
    double steerTime = 4.0;
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("sampleInterval", "Clock sampling interval (s)", sampleInterval);
    cmd.AddValue("steerTime", "When the servo steers clock B (s)", steerTime);
    cmd.Parse(argc, argv);

    // Pinned for hygiene/parity; this spike touches no RNG (see header).
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    // Two independent local clocks at syncsim's M1 baseline drift rates.
    // Constructed at Now() == 0, both starting aligned with global time.
    syncsim::Clock clockA(+200.0); // client1
    syncsim::Clock clockB(-350.0); // client2

    clockA.ConnectSampleTrace(MakeBoundCallback(&SampleSink, 0));
    clockB.ConnectSampleTrace(MakeBoundCallback(&SampleSink, 1));
    clockA.ConnectSteerTrace(MakeCallback(&SteerSink));
    clockB.ConnectSteerTrace(MakeCallback(&SteerSink));

    // Sample both clocks through their trace source on a fixed timer.
    for (double t = 0.0; t <= simTime + 1e-9; t += sampleInterval)
    {
        Simulator::Schedule(Seconds(t), &syncsim::Clock::Sample, &clockA);
        Simulator::Schedule(Seconds(t), &syncsim::Clock::Sample, &clockB);
    }

    // Mid-run servo action on clock B ONLY (clock A is the untouched control).
    // A gPTP slave servo does exactly these two corrections: phase then freq.
    Simulator::Schedule(Seconds(steerTime), [&clockB, steerTime]() {
        // Phase: null the offset accumulated so far. At t=steerTime the
        // -350 ppm clock is behind global by 350e-6 * steerTime seconds; step
        // local FORWARD by that amount to bring offset-from-global back to ~0.
        Time accumulated = Seconds(350e-6 * steerTime);
        clockB.AdjustOffset(accumulated);
        // Frequency: cancel the -350 ppm error so it holds flat from here.
        clockB.AdjustRate(+350.0);
    });

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // ---- Report: local-vs-global trajectory, then the gate checks ----------
    std::cout << "\n[clock-spike] local time vs global time (offset = local - global, in us)\n";
    std::cout << std::string(72, '-') << "\n";
    std::cout << std::setw(9) << "global(s)" << " | " << std::setw(14) << "A off us(+200)"
              << " | " << std::setw(14) << "B off us(-350)" << " | " << std::setw(12) << "A-B us"
              << "\n";
    std::cout << std::string(72, '-') << "\n";
    // Walk the per-time pairs (samples are pushed A then B at each timestamp).
    for (size_t i = 0; i + 1 < g_samples.size(); i += 2)
    {
        const Sample& a = g_samples[i];
        const Sample& b = g_samples[i + 1];
        double aOff = (a.local - a.global) * 1e6;
        double bOff = (b.local - b.global) * 1e6;
        std::cout << std::fixed << std::setprecision(2) << std::setw(9) << a.global << " | "
                  << std::setprecision(3) << std::setw(14) << aOff << " | " << std::setw(14) << bOff
                  << " | " << std::setw(12) << (aOff - bOff) << "\n";
    }

    // Measured slopes: before steering (both drift freely) and after (B nulled).
    double aBefore = MeasuredPpm(0, 0.0, steerTime);
    double bBefore = MeasuredPpm(1, 0.0, steerTime);
    double aAfter = MeasuredPpm(0, steerTime + 1e-9, simTime); // strictly after the step
    double bAfter = MeasuredPpm(1, steerTime + 1e-9, simTime);

    // Final offset-from-global of B (should be ~0 after phase+freq correction).
    double bFinalOff = NAN;
    for (const auto& s : g_samples)
    {
        if (s.id == 1)
        {
            bFinalOff = (s.local - s.global) * 1e6;
        }
    }

    std::cout << "\n[clock-spike] measured drift (ppm), recovered from the sampled trajectory:\n";
    std::cout << std::setprecision(3);
    std::cout << "  clock A  [0," << steerTime << "]s : " << aBefore << " ppm (configured +200)\n";
    std::cout << "  clock B  [0," << steerTime << "]s : " << bBefore << " ppm (configured -350)\n";
    std::cout << "  clock A  [" << steerTime << "," << simTime << "]s : " << aAfter
              << " ppm (still +200, untouched control)\n";
    std::cout << "  clock B  [" << steerTime << "," << simTime << "]s : " << bAfter
              << " ppm (steered to ~0 by AdjustRate(+350))\n";
    std::cout << "  clock B final offset-from-global : " << bFinalOff
              << " us (steered to ~0 by AdjustOffset)\n";

    // ---- Gate 1 checks -----------------------------------------------------
    const double ppmTol = 0.5;   // ppm: sampling-quantisation slack
    const double offTolUs = 1.0; // us: residual offset slack after steering
    bool cfgA = std::fabs(aBefore - 200.0) < ppmTol;
    bool cfgB = std::fabs(bBefore - (-350.0)) < ppmTol;
    bool diverge = false;
    // A-B separation must grow monotonically over the free-drift phase.
    {
        double prev = -1e18;
        diverge = true;
        for (size_t i = 0; i + 1 < g_samples.size(); i += 2)
        {
            if (g_samples[i].global > steerTime)
            {
                break;
            }
            double sep = (g_samples[i].local) - (g_samples[i + 1].local); // A - B
            if (sep < prev - 1e-12)
            {
                diverge = false;
                break;
            }
            prev = sep;
        }
    }
    bool ctrlA = std::fabs(aAfter - 200.0) < ppmTol;      // control kept drifting
    bool steerRate = std::fabs(bAfter - 0.0) < ppmTol;    // freq correction took
    bool steerPhase = std::fabs(bFinalOff) < offTolUs;    // phase correction took
    bool steered = steerRate && steerPhase && ctrlA;

    bool pass = cfgA && cfgB && diverge && steered;

    std::cout << "\n[clock-spike] Gate 1 checks:\n";
    std::cout << "  [" << (cfgA ? "PASS" : "FAIL") << "] clock A drifts at configured +200 ppm\n";
    std::cout << "  [" << (cfgB ? "PASS" : "FAIL") << "] clock B drifts at configured -350 ppm\n";
    std::cout << "  [" << (diverge ? "PASS" : "FAIL")
              << "] clocks diverge from each other and from global time\n";
    std::cout << "  [" << (steerRate ? "PASS" : "FAIL")
              << "] AdjustRate steered clock B's frequency to ~0 ppm\n";
    std::cout << "  [" << (steerPhase ? "PASS" : "FAIL")
              << "] AdjustOffset steered clock B's offset to ~0 us\n";
    std::cout << "  [" << (ctrlA ? "PASS" : "FAIL")
              << "] untouched control clock A kept drifting (steering was local)\n";
    std::cout << "\n[clock-spike] "
              << (pass ? "GATE 1 PASS: per-node steerable drift clock works on ns-3.45"
                       : "GATE 1 FAIL")
              << std::endl;

    return pass ? 0 : 1;
}
