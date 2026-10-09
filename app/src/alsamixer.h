// SPDX-License-Identifier: GPL-2.0-only
// The DC30 card's mixer controls (dc30_alsa.c) through libasound's control
// interface, by element name.
#pragma once

#include <string>

struct _snd_ctl;

class AlsaMixer {
public:
	AlsaMixer();
	~AlsaMixer();
	AlsaMixer(const AlsaMixer &) = delete;
	AlsaMixer &operator=(const AlsaMixer &) = delete;

	bool ok() const { return m_ctl != nullptr; }

	// Capture source: 0 = External (audio jack), 1 = Internal (connector).
	int source();
	void setSource(int src);
	// Input gain in 1.5 dB steps, 0..15, both channels.
	int gain();
	void setGain(int steps);
	bool boost();
	void setBoost(bool on);
	// Analog loop-through of the selected source to the card's output.
	void setLoopThrough(bool on);
	// ADC overrange since the last call (AD1843's sticky bits, 0..3 per
	// channel, 2 and 3 = clipped). Reading clears them.
	int overrange();

private:
	long get(const char *name, int index = 0);
	void set(const char *name, long value, int count);

	_snd_ctl *m_ctl = nullptr;
};
