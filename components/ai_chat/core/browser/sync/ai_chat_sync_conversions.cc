/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ai_chat/core/browser/sync/ai_chat_sync_conversions.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "brave/components/ai_chat/core/common/mojom/ai_chat.mojom.h"
#include "brave/components/ai_chat/core/common/mojom/common.mojom.h"
#include "brave/components/ai_chat/core/common/proto_conversion.h"
#include "brave/components/ai_chat/core/proto/store.pb.h"
#include "brave/components/sync/protocol/ai_chat_specifics.pb.h"
#include "components/sync/protocol/entity_data.h"
#include "components/sync/protocol/entity_specifics.pb.h"

namespace ai_chat {

namespace {

void ConvertEntryToProto(const mojom::ConversationTurn& entry,
                         sync_pb::AIChatConversationEntryProto* proto) {
  if (entry.uuid) {
    proto->set_uuid(*entry.uuid);
  }
  proto->set_date_unix_epoch_micros(
      entry.created_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
  proto->set_entry_text(entry.text);
  if (entry.prompt) {
    proto->set_prompt(*entry.prompt);
  }
  proto->set_character_type(static_cast<int32_t>(entry.character_type));
  proto->set_action_type(static_cast<int32_t>(entry.action_type));
  if (entry.selected_text) {
    proto->set_selected_text(*entry.selected_text);
  }
  if (entry.model_key) {
    proto->set_model_key(*entry.model_key);
  }

  // Convert events
  if (entry.events) {
    int order = 0;
    for (const auto& event : *entry.events) {
      auto* event_proto = proto->add_events();
      event_proto->set_event_order(order++);
      if (event->is_completion_event()) {
        event_proto->set_completion_text(
            event->get_completion_event()->completion);
      } else if (event->is_search_queries_event()) {
        const auto& queries = event->get_search_queries_event()->search_queries;
        event_proto->set_search_queries(base::JoinString(queries, "|||"));
      } else if (event->is_sources_event()) {
        store::WebSourcesEventProto store_proto;
        SerializeWebSourcesEvent(event->get_sources_event(), &store_proto);
        event_proto->set_web_sources_serialized(
            store_proto.SerializeAsString());
      } else if (event->is_inline_search_event()) {
        store::InlineSearchEventProto store_proto;
        SerializeInlineSearchEvent(event->get_inline_search_event(),
                                   &store_proto);
        event_proto->set_inline_search_serialized(
            store_proto.SerializeAsString());
      } else if (event->is_tool_use_event()) {
        store::ToolUseEventProto store_proto;
        if (SerializeToolUseEvent(event->get_tool_use_event(), &store_proto)) {
          event_proto->set_tool_use_serialized(store_proto.SerializeAsString());
        }
      }
    }
  }

  // Edits are stored as separate conversation entries with editing_entry_uuid
  // set. They are included in the entries list from the archive, so no special
  // handling is needed here.
}

}  // namespace

sync_pb::AIChatConversationSpecifics ConversationToSpecifics(
    const mojom::Conversation& conversation,
    const mojom::ConversationArchive& archive) {
  sync_pb::AIChatConversationSpecifics specifics;
  specifics.set_uuid(conversation.uuid);
  specifics.set_title(conversation.title);
  if (conversation.model_key) {
    specifics.set_model_key(*conversation.model_key);
  }
  specifics.set_total_tokens(conversation.total_tokens);
  specifics.set_trimmed_tokens(conversation.trimmed_tokens);

  // Convert associated content metadata (not full text)
  for (const auto& content : conversation.associated_content) {
    auto* content_proto = specifics.add_associated_content();
    content_proto->set_uuid(content->uuid);
    content_proto->set_title(content->title);
    if (content->url.is_valid()) {
      content_proto->set_url(content->url.spec());
    }
    content_proto->set_content_type(
        static_cast<int32_t>(content->content_type));
    content_proto->set_content_used_percentage(
        content->content_used_percentage);
    if (content->conversation_turn_uuid) {
      content_proto->set_conversation_entry_uuid(
          *content->conversation_turn_uuid);
    }
  }

  // Convert entries
  for (const auto& entry : archive.entries) {
    auto* entry_proto = specifics.add_entries();
    ConvertEntryToProto(*entry, entry_proto);
  }

  return specifics;
}

std::unique_ptr<syncer::EntityData> CreateEntityDataFromSpecifics(
    const sync_pb::AIChatConversationSpecifics& specifics) {
  auto entity_data = std::make_unique<syncer::EntityData>();
  *entity_data->specifics.mutable_ai_chat_conversation() = specifics;
  entity_data->name = specifics.uuid();
  return entity_data;
}

std::string GetStorageKeyFromSpecifics(
    const sync_pb::AIChatConversationSpecifics& specifics) {
  return specifics.uuid();
}

std::string GetClientTagFromSpecifics(
    const sync_pb::AIChatConversationSpecifics& specifics) {
  return specifics.uuid();
}

std::string GetStorageKeyFromEntitySpecifics(
    const sync_pb::EntitySpecifics& specifics) {
  return specifics.ai_chat_conversation().uuid();
}

mojom::ConversationPtr SpecificsToConversation(
    const sync_pb::AIChatConversationSpecifics& specifics) {
  auto conversation = mojom::Conversation::New();
  conversation->uuid = specifics.uuid();
  conversation->title = specifics.title();
  if (specifics.has_model_key()) {
    conversation->model_key = specifics.model_key();
  }
  conversation->total_tokens = specifics.total_tokens();
  conversation->trimmed_tokens = specifics.trimmed_tokens();

  for (const auto& content_proto : specifics.associated_content()) {
    auto content = mojom::AssociatedContent::New();
    content->uuid = content_proto.uuid();
    content->title = content_proto.title();
    if (content_proto.has_url()) {
      content->url = GURL(content_proto.url());
    }
    content->content_type =
        static_cast<mojom::ContentType>(content_proto.content_type());
    content->content_used_percentage = content_proto.content_used_percentage();
    if (content_proto.has_conversation_entry_uuid()) {
      content->conversation_turn_uuid = content_proto.conversation_entry_uuid();
    }
    conversation->associated_content.push_back(std::move(content));
  }

  return conversation;
}

mojom::ConversationArchivePtr SpecificsToArchive(
    const sync_pb::AIChatConversationSpecifics& specifics) {
  auto archive = mojom::ConversationArchive::New();

  for (const auto& entry_proto : specifics.entries()) {
    auto entry = mojom::ConversationTurn::New();
    if (entry_proto.has_uuid()) {
      entry->uuid = entry_proto.uuid();
    }
    if (entry_proto.has_date_unix_epoch_micros()) {
      entry->created_time = base::Time::FromDeltaSinceWindowsEpoch(
          base::Microseconds(entry_proto.date_unix_epoch_micros()));
    }
    entry->text = entry_proto.entry_text();
    if (entry_proto.has_prompt()) {
      entry->prompt = entry_proto.prompt();
    }
    entry->character_type =
        static_cast<mojom::CharacterType>(entry_proto.character_type());
    entry->action_type =
        static_cast<mojom::ActionType>(entry_proto.action_type());
    if (entry_proto.has_selected_text()) {
      entry->selected_text = entry_proto.selected_text();
    }
    if (entry_proto.has_model_key()) {
      entry->model_key = entry_proto.model_key();
    }
    if (entry_proto.has_editing_entry_uuid()) {
      // Edits are separate entries referencing their parent. The database
      // AddConversationEntry handles the editing_entry_uuid parameter.
    }

    // Convert events
    std::vector<mojom::ConversationEntryEventPtr> events;
    for (const auto& event_proto : entry_proto.events()) {
      if (event_proto.has_completion_text()) {
        auto completion = mojom::CompletionEvent::New();
        completion->completion = event_proto.completion_text();
        events.push_back(mojom::ConversationEntryEvent::NewCompletionEvent(
            std::move(completion)));
      } else if (event_proto.has_search_queries()) {
        auto search = mojom::SearchQueriesEvent::New();
        search->search_queries =
            base::SplitString(event_proto.search_queries(), "|||",
                              base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
        events.push_back(mojom::ConversationEntryEvent::NewSearchQueriesEvent(
            std::move(search)));
      } else if (event_proto.has_web_sources_serialized()) {
        store::WebSourcesEventProto store_proto;
        if (store_proto.ParseFromString(event_proto.web_sources_serialized())) {
          events.push_back(mojom::ConversationEntryEvent::NewSourcesEvent(
              DeserializeWebSourcesEvent(store_proto)));
        }
      } else if (event_proto.has_inline_search_serialized()) {
        store::InlineSearchEventProto store_proto;
        if (store_proto.ParseFromString(
                event_proto.inline_search_serialized())) {
          events.push_back(mojom::ConversationEntryEvent::NewInlineSearchEvent(
              DeserializeInlineSearchEvent(store_proto)));
        }
      } else if (event_proto.has_tool_use_serialized()) {
        store::ToolUseEventProto store_proto;
        if (store_proto.ParseFromString(event_proto.tool_use_serialized())) {
          events.push_back(mojom::ConversationEntryEvent::NewToolUseEvent(
              DeserializeToolUseEvent(store_proto)));
        }
      }
    }
    if (!events.empty()) {
      entry->events = std::move(events);
    }

    archive->entries.push_back(std::move(entry));
  }

  return archive;
}

}  // namespace ai_chat
