/*
 * NEX firmware — serialized work queue for UI-owned resources
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

namespace nex {

using WorkFn = void (*)(void *arg);

bool ui_work_init();
bool ui_work_post(WorkFn fn, void *arg);
int ui_work_process();

}  // namespace nex
