// Regression test for the server hang a 0 interval timer causes (vip_jumps / vip_duckspeed):
//
//   vip_jumps and vip_duckspeed ported the original's per-frame timers as
//   pisex::CreateTimer(0.0f, ...) (and returned 0.0f to keep them running). On s2sdk that is
//   FATAL: s2sdk's TimerSystem::RunFrame() keeps firing every timer that is due, and a repeat
//   timer's next execution time is recomputed from its delay - a 0 delay is immediately due
//   again, so the timer fires over and over inside that one frame.
//
//   The server frame never returns, the engine watchdog fires ("Server took too long to process
//   (probably infinite loop)") and the server exits. TimerSystem::CreateTimer() clamps the *first*
//   execution (universalTime + max(delay, tick)), which is why the crash only happened once the
//   map was running and the first timer tick came due - a few seconds after startup.
//
// The fix in the modules: never create and never return a 0 interval. vip_jumps and vip_duckspeed
// use 1 ms ("every tick", below the 0.015625 s engine tick), vip_regen_hp/vip_regen_armor clamp
// their group-configurable delay/interval to 50 ms, and each file has a static_assert that the
// constant is positive.
//
// This test mirrors RunFrame()'s loop with the interval values the VIP plugins use, so the
// invariant "an interval the VIP family passes to pisex::CreateTimer terminates" is checked
// without a server, and the 0 s hazard stays documented.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

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
			// The reschedule inside the loop uses the timer's stored delay (no clamp here - the
			// clamp exists only in CreateTimer).
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

// A frame that runs |frames| ticks after the map started, returning the total callback count.
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
	// The same timer also fails across frames: the first tick already loops forever.
	CHECK(RunFrames(0.0, 4) > 100000);
}

void TestPositiveDelaysTerminate() {
	// Every interval the VIP plugins use terminates and fires once per tick (1 ms), once per
	// 0.1 s, once per second, or once per the clamped 50 ms.
	std::vector<Timer> frame_timer{MakeTimer(0.001)};
	CHECK(SimulateRunFrame(kTick, frame_timer) == 1);

	// 1 ms: one fire per tick (60 ticks -> 60 fires).
	const std::size_t perTick = RunFrames(0.001, 60);
	CHECK(perTick == 60);

	// 0.1 s: fires every 7 ticks (0.109 s) -> 8 fires in the first 60 ticks (0.9375 s).
	const std::size_t perTenth = RunFrames(0.1, 60);
	CHECK(perTenth == 8);

	// 1 s (the vip core's expiry/reconcile timer): 1 fire within 70 ticks (1.09 s).
	const std::size_t perSecond = RunFrames(1.0, 70);
	CHECK(perSecond == 1);

	// The clamp used by vip_regen_hp / vip_regen_armor for a 0/negative configured delay:
	// fires every 4 ticks (0.0625, 0.125, ..., 0.9375) -> 15 fires in 60 ticks.
	const std::size_t clamped = RunFrames(0.05, 60);
	CHECK(clamped == 15);

	// A tiny positive delay is safe even when the clock stands still: executeTime becomes
	// universalTime + 0.001, which is not due again within the same pass.
	std::vector<Timer> still_timer{MakeTimer(0.001)};
	CHECK(SimulateRunFrame(100.0, still_timer) == 1);
}

void TestManyTimersTerminate() {
	// The set the VIP plugins create is a handful of timers; together they still terminate each
	// frame as long as none has delay 0.
	std::vector<Timer> timers;
	timers.push_back(MakeTimer(0.001, 0)); // vip_jumps
	timers.push_back(MakeTimer(0.001, 1)); // vip_duckspeed
	timers.push_back(MakeTimer(0.1, 2));   // vip_infiniteammo
	timers.push_back(MakeTimer(1.0, 3));   // vip core reconcile/expiry
	timers.push_back(MakeTimer(0.05, 4));  // clamped regen
	const std::size_t fires = SimulateRunFrame(1.0, timers);
	CHECK(fires <= timers.size() * 2); // a few, not unbounded
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("vip_timers_tests\n");
	TestZeroDelayHangs();
	TestPositiveDelaysTerminate();
	TestManyTimersTerminate();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
