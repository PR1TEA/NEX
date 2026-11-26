/*
 * NEX firmware — ES8311 speaker output
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

namespace nex {

bool audio_init();
void audio_alarm_start();
void audio_alarm_stop();

}  // namespace nex
