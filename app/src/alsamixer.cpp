// SPDX-License-Identifier: GPL-2.0-only
#include "alsamixer.h"

#include "devices.h"

#include <alsa/asoundlib.h>
#include <algorithm>

AlsaMixer::AlsaMixer()
{
	if (snd_ctl_open(&m_ctl, dc30::alsaDevice, 0) < 0)
		m_ctl = nullptr;
}

AlsaMixer::~AlsaMixer()
{
	if (m_ctl)
		snd_ctl_close(m_ctl);
}

long AlsaMixer::get(const char *name, int index)
{
	if (!m_ctl)
		return 0;

	snd_ctl_elem_value_t *val;
	snd_ctl_elem_value_alloca(&val);
	snd_ctl_elem_value_set_interface(val, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(val, name);
	if (snd_ctl_elem_read(m_ctl, val) < 0)
		return 0;

	snd_ctl_elem_info_t *info;
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	snd_ctl_elem_info(m_ctl, info);

	switch (snd_ctl_elem_info_get_type(info)) {
	case SND_CTL_ELEM_TYPE_BOOLEAN:
		return snd_ctl_elem_value_get_boolean(val, index);
	case SND_CTL_ELEM_TYPE_ENUMERATED:
		return snd_ctl_elem_value_get_enumerated(val, index);
	default:
		return snd_ctl_elem_value_get_integer(val, index);
	}
}

void AlsaMixer::set(const char *name, long value, int count)
{
	if (!m_ctl)
		return;

	snd_ctl_elem_info_t *info;
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	if (snd_ctl_elem_info(m_ctl, info) < 0)
		return;

	snd_ctl_elem_value_t *val;
	snd_ctl_elem_value_alloca(&val);
	snd_ctl_elem_value_set_interface(val, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(val, name);
	for (int i = 0; i < count; i++) {
		switch (snd_ctl_elem_info_get_type(info)) {
		case SND_CTL_ELEM_TYPE_BOOLEAN:
			snd_ctl_elem_value_set_boolean(val, i, value);
			break;
		case SND_CTL_ELEM_TYPE_ENUMERATED:
			snd_ctl_elem_value_set_enumerated(val, i, unsigned(value));
			break;
		default:
			snd_ctl_elem_value_set_integer(val, i, value);
		}
	}
	snd_ctl_elem_write(m_ctl, val);
}

int AlsaMixer::source() { return int(get("Capture Source")); }
void AlsaMixer::setSource(int src) { set("Capture Source", src, 1); }
int AlsaMixer::gain() { return int(get("Capture Volume")); }
void AlsaMixer::setGain(int steps) { set("Capture Volume", std::clamp(steps, 0, 15), 2); }
bool AlsaMixer::boost() { return get("External Boost Capture Switch"); }
void AlsaMixer::setBoost(bool on) { set("External Boost Capture Switch", on, 2); }

void AlsaMixer::setLoopThrough(bool on)
{
	const bool internal = source() == 1;

	set("External Playback Switch", on && !internal, 2);
	set("Internal Playback Switch", on && internal, 2);
}

int AlsaMixer::overrange()
{
	if (!m_ctl)
		return 0;

	/* One read for both channels: the driver clears the bits on read. */
	snd_ctl_elem_value_t *val;
	snd_ctl_elem_value_alloca(&val);
	snd_ctl_elem_value_set_interface(val, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(val, "ADC Overrange");
	if (snd_ctl_elem_read(m_ctl, val) < 0)
		return 0;
	return int(std::max(snd_ctl_elem_value_get_integer(val, 0),
			    snd_ctl_elem_value_get_integer(val, 1)));
}
