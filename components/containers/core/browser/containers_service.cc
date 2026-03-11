// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/containers/core/browser/containers_service.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "brave/components/containers/core/browser/pref_names.h"
#include "brave/components/containers/core/browser/prefs.h"
#include "brave/components/containers/core/common/features.h"
#include "brave/components/containers/core/mojom/containers.mojom.h"
#include "components/prefs/pref_service.h"

namespace containers {

ContainersService::ContainersService(PrefService* prefs,
                                     std::unique_ptr<Delegate> delegate)
    : prefs_(prefs), delegate_(std::move(delegate)) {
  CHECK(prefs_);
  CHECK(delegate_);

  pref_change_registrar_.Init(prefs_);
  pref_change_registrar_.Add(
      prefs::kContainersList,
      base::BindRepeating(&ContainersService::OnSyncedContainersChanged,
                          base::Unretained(this)));
  delegate_->StartObserving(base::BindRepeating(
      &ContainersService::OnReferencesChanged, weak_factory_.GetWeakPtr()));
}

ContainersService::~ContainersService() = default;

void ContainersService::Initialize() {
  SyncReferencedContainersIntoCache();
  MaybeCleanupOrphanedContainers();
}

void ContainersService::MarkContainerUsed(
    const mojom::ContainerPtr& container) {
  CHECK(!container.is_null());
  SetUsedContainerToPrefs(container, *prefs_);
  MaybeCleanupOrphanedContainers();
}

void ContainersService::OnReferencesChanged() {
  SyncReferencedContainersIntoCache();
  MaybeCleanupOrphanedContainers();
}

mojom::ContainerPtr ContainersService::GetContainerById(
    std::string_view id) const {
  if (auto container = GetContainerFromPrefs(*prefs_, id)) {
    return container;
  }
  if (auto container = GetUsedContainerFromPrefs(*prefs_, id)) {
    return container;
  }
  return nullptr;
}

void ContainersService::OnSyncedContainersChanged() {
  SyncReferencedContainersIntoCache();
  MaybeCleanupOrphanedContainers();
}

void ContainersService::SyncReferencedContainersIntoCache() {
  for (const std::string& id : delegate_->GetReferencedContainerIds()) {
    if (auto container = GetContainerFromPrefs(*prefs_, id)) {
      SetUsedContainerToPrefs(container, *prefs_);
    }
  }
}

std::vector<mojom::ContainerPtr> ContainersService::GetContainers() const {
  return GetContainersFromPrefs(*prefs_);
}

void ContainersService::MaybeCleanupOrphanedContainers() {
  const base::flat_set<std::string> referenced_container_ids =
      delegate_->GetReferencedContainerIds();

  for (const auto& container : GetUsedContainersFromPrefs(*prefs_)) {
    const std::string& id = container->id;
    if (GetContainerFromPrefs(*prefs_, id) ||
        referenced_container_ids.contains(id) ||
        cleanup_in_progress_.contains(id)) {
      continue;
    }

    cleanup_in_progress_.insert(id);
    delegate_->DeleteContainerStorage(
        id, base::BindOnce(&ContainersService::OnContainerStorageDeleted,
                           weak_factory_.GetWeakPtr(), id));
  }
}

void ContainersService::OnContainerStorageDeleted(const std::string& id,
                                                  bool success) {
  cleanup_in_progress_.erase(id);

  if (!success) {
    return;
  }

  if (!GetContainerFromPrefs(*prefs_, id) &&
      !delegate_->GetReferencedContainerIds().contains(id)) {
    RemoveUsedContainerFromPrefs(id, *prefs_);
  }
}

}  // namespace containers
