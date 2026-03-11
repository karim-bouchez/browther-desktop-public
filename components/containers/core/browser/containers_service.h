// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SERVICE_H_
#define BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SERVICE_H_

#include <memory>
#include <string>

#include "base/containers/flat_set.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/containers/core/mojom/containers.mojom-forward.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"

class PrefService;

namespace containers {

class ContainersService : public KeyedService {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;

    virtual void StartObserving(
        base::RepeatingClosure on_references_changed) = 0;
    virtual base::flat_set<std::string> GetReferencedContainerIds() const = 0;
    virtual void DeleteContainerStorage(
        const std::string& id,
        base::OnceCallback<void(bool success)> callback) = 0;
  };

  ContainersService(PrefService* prefs, std::unique_ptr<Delegate> delegate);
  ~ContainersService() override;

  ContainersService(const ContainersService&) = delete;
  ContainersService& operator=(const ContainersService&) = delete;

  void Initialize();
  void MarkContainerUsed(const mojom::ContainerPtr& container);
  void OnReferencesChanged();

  mojom::ContainerPtr GetContainerById(std::string_view id) const;
  std::vector<mojom::ContainerPtr> GetContainers() const;

 private:
  void OnSyncedContainersChanged();
  void SyncReferencedContainersIntoCache();
  void MaybeCleanupOrphanedContainers();
  void OnContainerStorageDeleted(const std::string& id, bool success);

  raw_ptr<PrefService> prefs_;
  std::unique_ptr<Delegate> delegate_;
  PrefChangeRegistrar pref_change_registrar_;
  base::flat_set<std::string> cleanup_in_progress_;
  base::WeakPtrFactory<ContainersService> weak_factory_{this};
};

}  // namespace containers

#endif  // BRAVE_COMPONENTS_CONTAINERS_CORE_BROWSER_CONTAINERS_SERVICE_H_
