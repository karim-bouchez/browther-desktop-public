/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/history/core/browser/sync/brave_history_data_type_controller.h"
#include "brave/components/history/core/browser/sync/brave_history_delete_directives_data_type_controller.h"

// AI Chat sync controller injection. The macro is placed in the upstream
// Build() method right before `return controllers;`.
#define BRAVE_BUILD_SYNC_CONTROLLERS  // No-op for now; bridge wiring TBD.

#define HistoryDeleteDirectivesDataTypeController \
  BraveHistoryDeleteDirectivesDataTypeController

#define HistoryDataTypeController BraveHistoryDataTypeController

#include <components/browser_sync/common_controller_builder.cc>

#undef HistoryDataTypeController
#undef HistoryDeleteDirectivesDataTypeController
#undef BRAVE_BUILD_SYNC_CONTROLLERS
