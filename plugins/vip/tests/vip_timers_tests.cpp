// vip_timers_tests — the timer-safety regression test of the vip core, after the port off
// pisexcompat (docs/PLUGIN_STANDARD.md §7, handoff/weaponskins/PITFALLS.md C9/B4).
//
// Two things are checked here, without an engine:
//
//   1. the historical hazard that made this test exist. The VIP family once ported the original's
//      repeating timers as pisex::CreateTimer(0.0f, ...): s2sdk's TimerSystem::RunFrame() keeps
//      firing every timer that is due, and a repeat timer's next execution is recomputed from its
//      delay, so a 0 delay is immediately due again - the main thread never returns from one
//      frame, the engine watchdog fires and the server exits. The simulation below shows the
//      runaway and that every positive delay terminates.
//
//   2. the port's own replacement: there is no s2sdk timer at all any more. The once-per-second
//      reconcile/expiry loop and the plgdb Poll() pump are vip::TickAccumulator instances fed by
//      OnPluginUpdate(dt) (src/tick_accumulator.hpp), so a 0 ms update, a long stall or a
//      changelevel can never re-enter a callback or replay a backlog.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "tick_accumulator.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                            \
		}                                                                                          \
	} while (false)

using namespace std::chrono_literals;

// --- 1. the documented s2sdk timer hazard ------------------------------------------------------

// s2sdk's engine tick interval (TimerSystem::engineFixedTickInterval).
constexpr double kTick = 0.015625;

struct Timer {
	double executeTime = 0.0;
	double delay = 0.0;
	bool repeat = true;
	int id = 0;
};

// The body of TimerSystem::RunFrame for one game frame: |timers| is the system's set (updated in
// place, exactly like the real m_timers). Returns how many times the callbacks fired; a value
// above |budget| means the loop did not terminate (the watchdog case).
std::size_t SimulateRunFrame(double universalTime, std::vector<Timer>& timers, std::size_t budget = 100000) {
	std::size_t fires = 0;
	while (!timers.empty()) {
		const auto it = std::min_element(timers.begin(), timers.end(), [](const Timer& a, const Timer& b) {
			return a.executeTime < b.executeTime || (a.executeTime == b.executeTime && a.id < b.id);
		});
		if (universalTime < it->executeTime) {
			break;
		}
		++fires;
		if (fires > budget) {
			return fires; // runaway
		}
		if (it->repeat) {
			// The reschedule inside the loop uses the timer's stored delay (the clamp exists only
			// in CreateTimer).
			it->executeTime = universalTime + it->delay;
			continue;
		}
		timers.erase(it);
	}
	return fires;
}

// One created timer as s2sdk stores it: first execution clamped to one tick, delay as passed.
Timer MakeTimer(double delay, int id = 0) {
	Timer timer;
	timer.delay = delay;
	timer.executeTime = 0.0 + std::max(delay, kTick);
	timer.id = id;
	return timer;
}

std::size_t RunFrames(double delay, int frames, std::size_t budget = 100000) {
	std::vector<Timer> timers{MakeTimer(delay)};
	std::size_t total = 0;
	double time = 0.0;
	for (int frame = 0; frame < frames; ++frame) {
		time += kTick;
		total += SimulateRunFrame(time, timers, budget);
		if (total > budget) {
			return total; // runaway, stop early
		}
	}
	return total;
}

void TestZeroDelayHangs() {
	// The documented hazard: delay 0 never terminates (unbounded number of fires in ONE frame).
	std::vector<Timer> timers{MakeTimer(0.0)};
	CHECK(SimulateRunFrame(kTick, timers) > 100000);
	CHECK(RunFrames(0.0, 4) > 100000);
}

void TestPositiveDelaysTerminate() {
	std::vector<Timer> frameTimer{MakeTimer(0.001)};
	CHECK(SimulateRunFrame(kTick, frameTimer) == 1);

	// 1 ms: one fire per tick (60 ticks -> 60 fires).
	CHECK(RunFrames(0.001, 60) == 60);
	// 0.1 s: fires every 7 ticks (0.109 s) -> 8 fires in the first 60 ticks (0.9375 s).
	CHECK(RunFrames(0.1, 60) == 8);
	// 1 s (the reconcile/expiry interval the VIP core needed): 1 fire within 70 ticks.
	CHECK(RunFrames(1.0, 70) == 1);
}

// --- 2. the port's replacement: the update-driven accumulator ----------------------------------

void TestAccumulatorCadence() {
	vip::TickAccumulator second{vip::kSecondTickInterval};
	if (vip::kSecondTickInterval != 1000ms) {
		CHECK(false);
	}
	// 62 updates of 16 ms (992 ms) do not reach the step; the 63rd (1008 ms) does.
	int steps = 0;
	for (int i = 0; i < 62; ++i) {
		steps += second.Feed(16ms);
	}
	CHECK(steps == 0);
	CHECK(second.Feed(16ms) == 1);
	// The leftover is 8 ms, not a full step.
	CHECK(second.Pending() == 8ms);

	// A producer that pumps 100 ms updates gets exactly one second tick every ten updates.
	vip::TickAccumulator slow{vip::kSecondTickInterval};
	int slowSteps = 0;
	for (int i = 0; i < 100; ++i) {
		slowSteps += slow.Feed(100ms);
	}
	CHECK(slowSteps == 10);
}

void TestAccumulatorNeverSpins() {
	// The 0-delay hazard cannot exist here: a 0 ms update reports no step, ever.
	vip::TickAccumulator second{vip::kSecondTickInterval};
	int steps = 0;
	for (int i = 0; i < 1000000; ++i) {
		steps += second.Feed(0ms);
	}
	CHECK(steps == 0);

	// Even a huge single jump reports at most the requested number of steps, and the backlog is
	// dropped instead of being replayed (the missed work is done once on the next frame).
	vip::TickAccumulator stalled{vip::kSecondTickInterval};
	CHECK(stalled.Feed(3600s, /*maxSteps=*/1) == 1);
	CHECK(stalled.Pending() == 0ms); // the backlog was dropped, not kept for the next frame
	CHECK(stalled.Feed(1ms) == 0);
	CHECK(stalled.Feed(1000ms) == 1);

	// maxSteps > 1 is honoured when a caller really wants catch-up.
	vip::TickAccumulator catchUp{vip::kSecondTickInterval};
	CHECK(catchUp.Feed(2500ms, /*maxSteps=*/2) == 2);
}

void TestAccumulatorDueNowAndReset() {
	// The plgdb pump is due immediately after startup / a CREATE TABLE result: DueNow() makes the
	// next update poll even when its delta is 0.
	vip::TickAccumulator database{vip::kDatabasePollInterval};
	if (vip::kDatabasePollInterval != 10ms) {
		CHECK(false);
	}
	CHECK(database.Feed(0ms) == 0);
	database.DueNow();
	CHECK(database.Feed(0ms) == 1);
	CHECK(database.Pending() == 0ms);

	// ~10 ms updates poll on every update (the plugin's OnPluginUpdate forwards the real delta).
	vip::TickAccumulator pump{vip::kDatabasePollInterval};
	int polls = 0;
	for (int i = 0; i < 20; ++i) {
		polls += pump.Feed(16ms) > 0 ? 1 : 0;
	}
	CHECK(polls == 20);

	// Reset() forgets a pending step (OnPluginEnd).
	pump.DueNow();
	pump.Reset();
	CHECK(pump.Feed(0ms) == 0);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("vip_timers_tests\n");
	TestZeroDelayHangs();
	TestPositiveDelaysTerminate();
	TestAccumulatorCadence();
	TestAccumulatorNeverSpins();
	TestAccumulatorDueNowAndReset();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
