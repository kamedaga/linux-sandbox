/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KOBOX_BOOT_DRM_TIME_H
#define KOBOX_BOOT_DRM_TIME_H

/* Linux and its caller share the clock rate, but not their boot epoch.
 * Preserve event age: substituting the read time would hide queued frames.
 * Bases belong to the DRM file and are sampled together when it opens. */
static inline int kobox_drm_time_translate(unsigned long long value,
		unsigned long long source_base, unsigned long long target_base,
		unsigned long long *out)
{
	unsigned long long difference;

	if (!out)
		return 0;
	if (target_base >= source_base) {
		difference = target_base - source_base;
		if (value > ~0ull - difference)
			return 0;
		*out = value + difference;
	} else {
		difference = source_base - target_base;
		if (value < difference)
			return 0;
		*out = value - difference;
	}
	return 1;
}

static inline unsigned long long kobox_drm_deadline_translate(
		unsigned long long value, unsigned long long source_base,
		unsigned long long target_base)
{
	unsigned long long translated;
	const unsigned long long infinite = ~0ull >> 1;

	/* Zero polls; signed -1 and INT64_MAX represent unbounded waits. */
	if (!value || value >= infinite)
		return value;
	if (target_base < source_base && value < source_base - target_base)
		return 0;
	if (!kobox_drm_time_translate(value, source_base, target_base,
				     &translated) || translated > infinite)
		return infinite;
	return translated;
}

#endif
