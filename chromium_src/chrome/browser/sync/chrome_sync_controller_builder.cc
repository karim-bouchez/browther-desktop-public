/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/ai_chat/ai_chat_service_factory.h"
#include "brave/components/ai_chat/core/browser/ai_chat_service.h"
#include "brave/components/ai_chat/core/common/features.h"
#include "components/sync/service/data_type_controller.h"

// Register AI Chat sync controller when building Chrome-level sync controllers.
// Uses the profile from extension_system_profile_ to get AIChatService.
#define BRAVE_BUILD_CHROME_SYNC_CONTROLLERS                                 \
  if (ai_chat::features::IsBraveSyncAIChatEnabled()) {                      \
    auto* ai_chat_service =                                                 \
        ai_chat::AIChatServiceFactory::GetForBrowserContext(                \
            extension_system_profile_.value());                             \
    if (ai_chat_service) {                                                  \
      auto delegate = ai_chat_service->CreateSyncControllerDelegate();      \
      if (delegate) {                                                       \
        controllers.push_back(std::make_unique<syncer::DataTypeController>( \
            syncer::AI_CHAT_CONVERSATION,                                   \
            /*delegate_for_full_sync_mode=*/std::move(delegate),            \
            /*delegate_for_transport_mode=*/nullptr));                      \
      }                                                                     \
    }                                                                       \
  }

#include <chrome/browser/sync/chrome_sync_controller_builder.cc>

#undef BRAVE_BUILD_CHROME_SYNC_CONTROLLERS
