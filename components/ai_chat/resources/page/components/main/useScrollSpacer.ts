// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'

/**
 * Manages a spacer element that appears below a newly submitted user message,
 * filling the remaining viewport height so the message starts at the top.
 * The spacer shrinks as the AI response grows to fill the space.
 *
 * @param scrollElement The element which scrolls
 * @param conversationUuid The current conversation UUID (used to reset state)
 * @param originalSubmit The original submitInputTextToAPI function
 * @param useChildHeightChanged Hook that fires when the conversation iframe height changes
 */
export function useScrollSpacer(
  scrollElement: React.RefObject<HTMLDivElement | null>,
  conversationUuid: string | undefined,
  originalSubmit: () => void,
  useChildHeightChanged: (
    callback: (height: number) => void,
    deps: React.DependencyList,
  ) => void,
) {
  const lastSubmitScrollHeight = React.useRef<number | null>(null)
  const preSubmitIframeHeight = React.useRef<number>(0)
  const submitClientHeight = React.useRef<number>(0)
  const prevIframeContentHeight = React.useRef<number>(0)
  const [spacerHeight, setSpacerHeight] = React.useState(0)

  // Reset spacer state when switching conversations.
  React.useEffect(() => {
    setSpacerHeight(0)
    submitClientHeight.current = 0
    preSubmitIframeHeight.current = 0
    lastSubmitScrollHeight.current = null
  }, [conversationUuid])

  const submitInputTextToAPI = React.useCallback(() => {
    if (scrollElement.current) {
      const { scrollHeight, clientHeight } = scrollElement.current
      if (scrollHeight > clientHeight) {
        lastSubmitScrollHeight.current = scrollHeight
        preSubmitIframeHeight.current = prevIframeContentHeight.current
        submitClientHeight.current = clientHeight
        setSpacerHeight(clientHeight)
      }
    }
    originalSubmit()
  }, [originalSubmit])

  // Shrink the spacer as the AI response grows. On the first height change
  // after a submit, also scroll the user message to the top of the viewport.
  useChildHeightChanged((height) => {
    prevIframeContentHeight.current = height

    if (submitClientHeight.current > 0) {
      const growthSinceSubmit = height - preSubmitIframeHeight.current
      setSpacerHeight(
        Math.max(0, submitClientHeight.current - growthSinceSubmit),
      )
    }

    if (lastSubmitScrollHeight.current !== null) {
      const target = lastSubmitScrollHeight.current
      lastSubmitScrollHeight.current = null
      requestAnimationFrame(() => {
        scrollElement.current?.scrollTo({ top: target, behavior: 'smooth' })
      })
    }
  }, [])

  // Recalculate the spacer when the scroll container is resized.
  React.useEffect(() => {
    const el = scrollElement.current
    if (!el) return

    const observer = new ResizeObserver(() => {
      if (submitClientHeight.current > 0) {
        submitClientHeight.current = el.clientHeight
        const growthSinceSubmit =
          prevIframeContentHeight.current - preSubmitIframeHeight.current
        setSpacerHeight(
          Math.max(0, submitClientHeight.current - growthSinceSubmit),
        )
      }
    })

    observer.observe(el)
    return () => observer.disconnect()
  }, [])

  return { spacerHeight, submitInputTextToAPI }
}
