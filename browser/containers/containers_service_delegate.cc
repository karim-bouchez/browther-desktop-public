// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/containers/containers_service_delegate.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "brave/components/containers/content/browser/storage_partition_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sessions/tab_restore_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/sessions/core/tab_restore_types.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "crypto/hash.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"

namespace {

uint32_t GetStorageRemoveMask() {
  return content::StoragePartition::REMOVE_DATA_MASK_ALL;
}

uint32_t GetQuotaStorageRemoveMask() {
  return content::StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL;
}

bool DeleteContainerStoragePath(const base::FilePath& path) {
  return !base::PathExists(path) || base::DeletePathRecursively(path);
}

void DeleteContainerStoragePathBestEffort(const base::FilePath& path) {
  DeleteContainerStoragePath(path);
}

base::FilePath GetContainerStoragePartitionPath(Profile* profile,
                                                const std::string& id) {
  constexpr int kPartitionNameHashBytes = 6;
  auto hash = crypto::hash::Sha256(id);
  auto truncated_hash = base::span(hash).first<kPartitionNameHashBytes>();
  return profile->GetPath()
      .AppendASCII("Storage")
      .AppendASCII("ext")
      .AppendASCII(containers::kContainersStoragePartitionDomain)
      .AppendASCII(base::HexEncode(truncated_hash));
}

}  // namespace

ContainersServiceDelegate::ContainersServiceDelegate(Profile* profile)
    : profile_(profile),
      tab_restore_service_(TabRestoreServiceFactory::GetForProfile(profile)),
      tab_strip_tracker_(this, this) {
  CHECK(profile_);
}

ContainersServiceDelegate::~ContainersServiceDelegate() = default;

void ContainersServiceDelegate::StartObserving(
    base::RepeatingClosure on_references_changed) {
  on_references_changed_ = std::move(on_references_changed);
  tab_strip_tracker_.Init();

  if (tab_restore_service_) {
    tab_restore_service_observation_.Observe(tab_restore_service_);
    if (!tab_restore_service_->IsLoaded()) {
      tab_restore_service_->LoadTabsFromLastSession();
    }
  }

  NotifyReferencesChanged();
}

base::flat_set<std::string>
ContainersServiceDelegate::GetReferencedContainerIds() const {
  base::flat_set<std::string> ids;

  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [this, &ids](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile_) {
          return true;
        }

        for (tabs::TabInterface* tab : *browser->GetTabStripModel()) {
          CHECK(tab);
          if (content::WebContents* contents = tab->GetContents()) {
            const std::string id =
                containers::GetContainerIdForWebContents(contents);
            if (!id.empty()) {
              ids.insert(id);
            }
          }
        }
        return true;
      });

  if (!tab_restore_service_ || !tab_restore_service_->IsLoaded()) {
    return ids;
  }

  for (const auto& entry : tab_restore_service_->entries()) {
    AddReferencedContainerIdsFromEntry(*entry, ids);
  }

  return ids;
}

void ContainersServiceDelegate::DeleteContainerStorage(
    const std::string& id,
    base::OnceCallback<void(bool success)> callback) {
  const content::StoragePartitionConfig config =
      content::StoragePartitionConfig::Create(
          profile_, containers::kContainersStoragePartitionDomain, id,
          profile_->IsOffTheRecord());

  const base::FilePath partition_path =
      GetContainerStoragePartitionPath(profile_, id);

  if (content::StoragePartition* partition =
          profile_->GetStoragePartition(config, /*can_create=*/false)) {
    partition->ClearData(
        GetStorageRemoveMask(), GetQuotaStorageRemoveMask(),
        /*filter_builder=*/nullptr,
        /*storage_key_policy_matcher=*/base::NullCallback(),
        /*cookie_deletion_filter=*/network::mojom::CookieDeletionFilter::New(),
        /*perform_storage_cleanup=*/true, base::Time(), base::Time::Max(),
        base::BindOnce(
            [](base::FilePath path, base::OnceCallback<void(bool)> callback) {
              base::ThreadPool::PostTaskAndReply(
                  FROM_HERE, {base::MayBlock()},
                  base::BindOnce(&DeleteContainerStoragePathBestEffort, path),
                  base::BindOnce(std::move(callback), true));
            },
            partition_path, std::move(callback)));
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&DeleteContainerStoragePath, partition_path),
      std::move(callback));
}

void ContainersServiceDelegate::NotifyReferencesChanged() const {
  if (on_references_changed_) {
    on_references_changed_.Run();
  }
}

void ContainersServiceDelegate::AddReferencedContainerIdsFromEntry(
    const sessions::tab_restore::Entry& entry,
    base::flat_set<std::string>& ids) const {
  switch (entry.type) {
    case sessions::tab_restore::TAB:
      AddReferencedContainerIdsFromTab(
          static_cast<const sessions::tab_restore::Tab&>(entry), ids);
      break;
    case sessions::tab_restore::WINDOW:
      AddReferencedContainerIdsFromWindow(
          static_cast<const sessions::tab_restore::Window&>(entry), ids);
      break;
    case sessions::tab_restore::GROUP:
      AddReferencedContainerIdsFromGroup(
          static_cast<const sessions::tab_restore::Group&>(entry), ids);
      break;
  }
}

void ContainersServiceDelegate::AddReferencedContainerIdsFromTab(
    const sessions::tab_restore::Tab& tab,
    base::flat_set<std::string>& ids) const {
  for (const auto& navigation : tab.navigations) {
    const auto& storage_partition_key = navigation.storage_partition_key();
    if (!storage_partition_key.has_value()) {
      continue;
    }

    if (containers::IsContainersStoragePartitionKey(
            storage_partition_key->first, storage_partition_key->second)) {
      ids.insert(storage_partition_key->second);
    }
  }
}

void ContainersServiceDelegate::AddReferencedContainerIdsFromWindow(
    const sessions::tab_restore::Window& window,
    base::flat_set<std::string>& ids) const {
  for (const auto& tab : window.tabs) {
    AddReferencedContainerIdsFromTab(*tab, ids);
  }
}

void ContainersServiceDelegate::AddReferencedContainerIdsFromGroup(
    const sessions::tab_restore::Group& group,
    base::flat_set<std::string>& ids) const {
  for (const auto& tab : group.tabs) {
    AddReferencedContainerIdsFromTab(*tab, ids);
  }
}

bool ContainersServiceDelegate::ShouldTrackBrowser(
    BrowserWindowInterface* browser) {
  return browser->GetProfile() == profile_;
}

void ContainersServiceDelegate::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::OnTabChangedAt(tabs::TabInterface* tab,
                                               int index,
                                               TabChangeType change_type) {
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::TabStripEmpty() {
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::OnTabStripModelDestroyed(
    TabStripModel* tab_strip_model) {
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::TabRestoreServiceChanged(
    sessions::TabRestoreService* service) {
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::TabRestoreServiceDestroyed(
    sessions::TabRestoreService* service) {
  tab_restore_service_observation_.Reset();
  tab_restore_service_ = nullptr;
  NotifyReferencesChanged();
}

void ContainersServiceDelegate::TabRestoreServiceLoaded(
    sessions::TabRestoreService* service) {
  NotifyReferencesChanged();
}
