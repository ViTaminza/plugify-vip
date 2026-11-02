#pragma once
// vip — the fixed-step accumulator that replaces every s2sdk timer of the original plugin.
//
// PITFALLS C9/B4: s2sdk timers (`CreateTimer`) run inside TimerSystem::RunFrame and are unsafe for
// gameplay/client/spawn/database work here - a repeating timer with delay 0 hangs the main thread
// inside one frame, and a callback that survives a changelevel touches a torn-down client list.
// The plugin therefore owns no timer at all: OnPluginUpdate(dt) feeds these accumulators and the
// work runs on the game thread, in the frame, where the map gate and the SteamID/controller checks
// are revalidated right before the action.
//
// Semantics:
//   * Feed() adds the update's delta and returns how many whole steps became due;
//   * a step is 1000 ms for the reconcile/expiry tick of the original's GameFrame loop and 10 ms
//     for the plgdb Poll() pump;
//   * at most `maxSteps` steps are reported and a backlog beyond them is dropped: a long stall
//     (a debugger, an OS suspend, a huge hitch) must not replay dozens of ticks or Poll loops in
//     a single frame, and must not leave a full step pending for the next one either.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace vip {

// The two steps the plugin runs on: the once-per-second reconcile/expiry loop of the original's
// GameFrame and the plgdb Poll() pump.
inline constexpr std::chrono::milliseconds kSecondTickInterval{1000};
inline constexpr std::chrono::milliseconds kDatabasePollInterval{10};

class TickAccumulator {
public:
	explicit TickAccumulator(std::chrono::milliseconds step)
		: m_step(step.count() > 0 ? step : std::chrono::milliseconds(1)) {}

	// Feeds one update delta; returns the number of whole steps that are now due (0..maxSteps).
	int Feed(std::chrono::milliseconds delta, int maxSteps = 1) {
		if (delta.count() > 0) {
			m_accumulator += delta;
		}
		if (maxSteps < 1) {
			maxSteps = 1;
		}
		int steps = 0;
		while (m_accumulator >= m_step && steps < maxSteps) {
			m_accumulator -= m_step;
			++steps;
		}
		// A backlog beyond the reported steps is dropped instead of being replayed later: the
		// missed work is done at most `maxSteps` times in this frame, never as a burst on the
		// following ones.
		if (m_accumulator >= m_step) {
			m_accumulator = std::chrono::milliseconds(0);
		}
		return steps;
	}

	// Makes the next Feed() report a step immediately (used by the database pump at startup: the
	// statements queued by OnPluginStart must reach their callbacks on the first update).
	void DueNow() { m_accumulator = m_step; }

	// Forgets a pending step (OnPluginEnd / a fresh start).
	void Reset() { m_accumulator = std::chrono::milliseconds(0); }

	[[nodiscard]] std::chrono::milliseconds Step() const { return m_step; }
	[[nodiscard]] std::chrono::milliseconds Pending() const { return m_accumulator; }

private:
	std::chrono::milliseconds m_step;
	std::chrono::milliseconds m_accumulator{0};
};

} // namespace vip
