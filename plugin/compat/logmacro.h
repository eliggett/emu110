// license:BSD-3-Clause
// copyright-holders:Elliott H. Liggett
//
// The same contract as MAME's logmacro.h: VERBOSE selects which LOGMASKED calls survive,
// and the default of 0 compiles them all out.  flt_biquad.cpp is the only user here.
#ifndef VOLTAIRE_COMPAT_LOGMACRO_H
#define VOLTAIRE_COMPAT_LOGMACRO_H
#pragma once

#ifndef VERBOSE
#define VERBOSE 0
#endif

#ifndef LOG_OUTPUT_FUNC
#define LOG_OUTPUT_FUNC logerror
#endif

#ifndef LOG_GENERAL
#define LOG_GENERAL (1U << 0)
#endif

#define LOGMASKED(mask, ...) do { if (VERBOSE & (mask)) (LOG_OUTPUT_FUNC)(__VA_ARGS__); } while (false)
#define LOG(...) LOGMASKED(LOG_GENERAL, __VA_ARGS__)

#endif
