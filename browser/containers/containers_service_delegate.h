// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_CONTAINERS_CONTAINERS_SERVICE_DELEGATE_H_
#define BRAVE_BROWSER_CONTAINERS_CONTAINERS_SERVICE_DELEGATE_H_

#include <string>

#include "base/containers/flat_set.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "brave/components/containers/core/browser/containers_service.h"
#include "chrome/browser/ui/browser_tab_strip_tracker.h"
#include "chrome/browser/ui/browser_tab_strip_tracker_delegate.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/sessions/core/tab_restore_service_observer.h"

class Profile;

namespace sessions {
class TabRestoreService;
namespace tab_restore {
struct Entry;
struct Tab;
struct Window;
struct Group;
}  // namespace tab_restore
}  // namespace sessions

class ContainersServiceDelegate
    : public containers::ContainersService::Delegate,
      public BrowserTabStripTrackerDelegate,
      public TabStripModelObserver,
      public sessions::TabRestoreServiceObserver {
 public:
  explicit ContainersServiceDelegate(Profile* profile);
  ~ContainersServiceDelegate() override;

  ContainersServiceDelegate(const ContainersServiceDelegate&) = delete;
  ContainersServiceDelegate& operator=(const ContainersServiceDelegate&) =
      delete;

  void StartObserving(base::RepeatingClosure on_references_changed) override;
  base::flat_set<std::string> GetReferencedContainerIds() const override;
  void DeleteContainerStorage(
      const std::string& id,
      base::OnceCallback<void(bool success)> callback) override;

 private:
  void NotifyReferencesChanged() const;
  void AddReferencedContainerIdsFromEntry(
      const sessions::tab_restore::Entry& entry,
      base::flat_set<std::string>& ids) const;
  void AddReferencedContainerIdsFromTab(const sessions::tab_restore::Tab& tab,
                                        base::flat_set<std::string>& ids) const;
  void AddReferencedContainerIdsFromWindow(
      const sessions::tab_restore::Window& window,
      base::flat_set<std::string>& ids) const;
  void AddReferencedContainerIdsFromGroup(
      const sessions::tab_restore::Group& group,
      base::flat_set<std::string>& ids) const;

  bool ShouldTrackBrowser(BrowserWindowInterface* browser) override;
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;
  void OnTabChangedAt(tabs::TabInterface* tab,
                      int index,
                      TabChangeType change_type) override;
  void TabStripEmpty() override;
  void OnTabStripModelDestroyed(TabStripModel* tab_strip_model) override;

  void TabRestoreServiceChanged(sessions::TabRestoreService* service) override;
  void TabRestoreServiceDestroyed(
      sessions::TabRestoreService* service) override;
  void TabRestoreServiceLoaded(sessions::TabRestoreService* service) override;

  raw_ptr<Profile> profile_;
  raw_ptr<sessions::TabRestoreService> tab_restore_service_ = nullptr;
  BrowserTabStripTracker tab_strip_tracker_;
  mutable base::RepeatingClosure on_references_changed_;
  base::ScopedObservation<sessions::TabRestoreService,
                          sessions::TabRestoreServiceObserver>
      tab_restore_service_observation_{this};
};

#endif  // BRAVE_BROWSER_CONTAINERS_CONTAINERS_SERVICE_DELEGATE_H_
