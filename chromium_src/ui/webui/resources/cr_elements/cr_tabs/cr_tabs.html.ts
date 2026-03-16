// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// Override upstream template to add data-test-id attributes for automated
// testing on each tab element.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {CrTabsElement} from './cr_tabs.js';

export function getHtml(this: CrTabsElement) {
  return html`${this.tabNames.map((item, index) => html`
<div role="tab"
    class="tab ${this.getSelectedClass_(index)}"
    aria-selected="${this.getAriaSelected_(index)}"
    tabindex="${this.getTabindex_(index)}"
    data-index="${index}"
    data-test-id="${item.toLowerCase()}"
    @click="${this.onTabClick_}">
  <div class="tab-icon" .style="${this.getIconStyle_(index)}"></div>
  ${item}
  <div class="tab-indicator-background"></div>
  <div class="tab-indicator"></div>
</div>
`)}`;
}
