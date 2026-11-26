/*
 * NEX firmware — reminders (checklist) storage
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>

namespace nex {

constexpr int kReminderMax = 24;
constexpr int kReminderTextMax = 192;

struct Reminder {
    bool done;
    char text[kReminderTextMax + 1];
};

bool reminders_init();
int reminders_count();
const Reminder *reminders_get(int index);
bool reminders_toggle(int index);
bool reminders_delete(int index);
bool reminders_append_json(const char *json, size_t length);
bool reminders_set_json(const char *json, size_t length);
int reminders_to_json(char *out, size_t capacity);
int reminders_generation();

}  // namespace nex
