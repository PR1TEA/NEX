/*
 * NEX firmware — physical keys (KEYA / KEYB)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

namespace nex {

// Configure GPIO1/GPIO2 as pulled-up inputs and start the polling task.
bool keys_start();

}  // namespace nex
