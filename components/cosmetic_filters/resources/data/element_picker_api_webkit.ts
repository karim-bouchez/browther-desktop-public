// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import { ElementPickerAPI } from './element_picker_api'

export class WebKitElementPickerAPI implements ElementPickerAPI {
  cosmeticFilterCreate(selector: string) {
    let message = {
      "securityToken": 'SECURITY_TOKEN',
      "data": {
        windowOrigin: window.origin,
        action: 'addCosmeticFilter',
        data: selector
      }
    }
    window.webkit.messageHandlers['\(BlockElementScriptHandler.messageHandlerName)'].postMessage(message);
  }

  cosmeticFilterManage() {
    let message = {
      "securityToken": 'SECURITY_TOKEN',
      "data": {
        windowOrigin: window.origin,
        action: 'manageCustomFilters',
        data: undefined
      }
    }
    window.webkit.messageHandlers['\(BlockElementScriptHandler.messageHandlerName)'].postMessage(message);
  }

  getElementPickerThemeInfo(
    callback: (isDarkModeEnabled: boolean, bgcolor: number) => void,
  ) {
    let message = {
      "securityToken": 'SECURITY_TOKEN',
      "data": {
        windowOrigin: window.origin,
        action: 'elementPickerThemeInfo',
        data: undefined
      }
    }
    window.webkit.messageHandlers['\(BlockElementScriptHandler.messageHandlerName)']
      .postMessage(message)
      .then((val: { isDarkModeEnabled: boolean; bgcolor: number }) => {
        callback(val.isDarkModeEnabled, val.bgcolor)
      })
  }

  getLocalizedTexts(
    callback: (
      btnCreateDisabledText: string,
      btnCreateEnabledText: string,
      btnManageText: string,
      btnShowRulesBoxText: string,
      btnHideRulesBoxText: string,
      btnQuitText: string,
    ) => void,
  ) {
    let message = {
      "securityToken": 'SECURITY_TOKEN',
      "data": {
        windowOrigin: window.origin,
        action: 'localizedTexts',
        data: undefined
      }
    }
    window.webkit.messageHandlers['\(BlockElementScriptHandler.messageHandlerName)']
      .postMessage(message)
      .then(
        (val: {
          btnCreateDisabledText: string
          btnCreateEnabledText: string
          btnManageText: string
          btnShowRulesBoxText: string
          btnHideRulesBoxText: string
          btnQuitText: string
        }) => {
          callback(
            val.btnCreateDisabledText,
            val.btnCreateEnabledText,
            val.btnManageText,
            val.btnShowRulesBoxText,
            val.btnHideRulesBoxText,
            val.btnQuitText,
          )
        },
      )
  }

  getPlatform() {
    return 'ios'
  }
}
