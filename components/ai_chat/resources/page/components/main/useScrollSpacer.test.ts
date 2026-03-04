// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import { renderHook, act } from '@testing-library/react'
import { useScrollSpacer } from './useScrollSpacer'

describe('useScrollSpacer', () => {
  let capturedHeightCallback: ((height: number) => void) | undefined
  const mockUseChildHeightChanged = jest.fn()

  let mockDisconnect: jest.Mock
  let mockObserve: jest.Mock
  let capturedResizeCallback: (() => void) | undefined

  beforeEach(() => {
    capturedHeightCallback = undefined
    capturedResizeCallback = undefined

    // resetMocks: true in jest.config.js resets implementations between tests,
    // so we must re-register the implementation in beforeEach.
    mockUseChildHeightChanged.mockImplementation(
      (callback: (height: number) => void) => {
        capturedHeightCallback = callback
      },
    )

    mockDisconnect = jest.fn()
    mockObserve = jest.fn()
    global.ResizeObserver = jest.fn().mockImplementation((callback) => {
      capturedResizeCallback = callback
      return { observe: mockObserve, disconnect: mockDisconnect }
    })

    global.requestAnimationFrame = jest.fn((rafCallback) => {
      rafCallback(0)
      return 0
    })
  })

  const makeScrollElement = (scrollHeight: number, clientHeight: number) => {
    const el = {
      scrollHeight,
      clientHeight,
      scrollTo: jest.fn(),
    } as unknown as HTMLDivElement
    return { current: el }
  }

  const renderUseScrollSpacer = (
    scrollElement: React.RefObject<HTMLDivElement | null>,
    conversationUuid: string | undefined = 'uuid-1',
    originalSubmit: () => void = jest.fn(),
  ) => {
    return renderHook(
      ({ uuid, submit }) =>
        useScrollSpacer(scrollElement, uuid, submit, mockUseChildHeightChanged),
      { initialProps: { uuid: conversationUuid, submit: originalSubmit } },
    )
  }

  describe('initial state', () => {
    it('starts with spacerHeight of 0', () => {
      const { result } = renderUseScrollSpacer(makeScrollElement(300, 300))
      expect(result.current.spacerHeight).toBe(0)
    })
  })

  describe('submitInputTextToAPI', () => {
    it('sets spacerHeight to clientHeight when content is scrollable', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })

      expect(result.current.spacerHeight).toBe(300)
    })

    it('calls the original submit function', () => {
      const originalSubmit = jest.fn()
      const { result } = renderUseScrollSpacer(
        makeScrollElement(500, 300),
        'uuid-1',
        originalSubmit,
      )

      act(() => {
        result.current.submitInputTextToAPI()
      })

      expect(originalSubmit).toHaveBeenCalledTimes(1)
    })

    it('does not set spacerHeight when content is not scrollable', () => {
      const scrollElement = makeScrollElement(300, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })

      expect(result.current.spacerHeight).toBe(0)
    })

    it('still calls original submit when content is not scrollable', () => {
      const originalSubmit = jest.fn()
      const { result } = renderUseScrollSpacer(
        makeScrollElement(300, 300),
        'uuid-1',
        originalSubmit,
      )

      act(() => {
        result.current.submitInputTextToAPI()
      })

      expect(originalSubmit).toHaveBeenCalledTimes(1)
    })
  })

  describe('spacer shrinks as response grows', () => {
    it('reduces spacerHeight as the iframe height increases', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      expect(result.current.spacerHeight).toBe(300)

      act(() => {
        capturedHeightCallback!(50)
      })
      expect(result.current.spacerHeight).toBe(250) // 300 - 50

      act(() => {
        capturedHeightCallback!(150)
      })
      expect(result.current.spacerHeight).toBe(150) // 300 - 150

      act(() => {
        capturedHeightCallback!(300)
      })
      expect(result.current.spacerHeight).toBe(0) // 300 - 300
    })

    it('does not go below 0', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      act(() => {
        capturedHeightCallback!(400) // growth exceeds clientHeight
      })

      expect(result.current.spacerHeight).toBe(0)
    })

    it('accounts for pre-submit iframe height in growth calculation', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      // Establish prior iframe height of 200px before submit
      act(() => {
        capturedHeightCallback!(200)
      })
      expect(result.current.spacerHeight).toBe(0) // no submit yet

      act(() => {
        result.current.submitInputTextToAPI()
      })
      expect(result.current.spacerHeight).toBe(300)

      // Iframe grows by 50px from the pre-submit baseline
      act(() => {
        capturedHeightCallback!(250)
      })
      expect(result.current.spacerHeight).toBe(250) // 300 - (250-200)
    })

    it('does not update spacer when no submit has occurred', () => {
      const { result } = renderUseScrollSpacer(makeScrollElement(500, 300))

      act(() => {
        capturedHeightCallback!(100)
      })

      expect(result.current.spacerHeight).toBe(0)
    })
  })

  describe('scrolling to new message', () => {
    it('scrolls to the pre-submit scrollHeight on first height change', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      act(() => {
        capturedHeightCallback!(50)
      })

      expect(scrollElement.current?.scrollTo).toHaveBeenCalledWith({
        top: 500,
        behavior: 'smooth',
      })
    })

    it('only scrolls once after a submit', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      act(() => {
        capturedHeightCallback!(50)
      })
      act(() => {
        capturedHeightCallback!(100)
      })
      act(() => {
        capturedHeightCallback!(150)
      })

      expect(scrollElement.current?.scrollTo).toHaveBeenCalledTimes(1)
    })

    it('does not scroll when content is not scrollable at submit time', () => {
      const scrollElement = makeScrollElement(300, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      act(() => {
        capturedHeightCallback!(50)
      })

      expect(scrollElement.current?.scrollTo).not.toHaveBeenCalled()
    })
  })

  describe('conversation change', () => {
    it('resets spacerHeight when conversation UUID changes', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result, rerender } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      expect(result.current.spacerHeight).toBe(300)

      act(() => {
        rerender({ uuid: 'uuid-2', submit: jest.fn() })
      })

      expect(result.current.spacerHeight).toBe(0)
    })

    it('does not update spacer from height changes after conversation switch', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result, rerender } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })

      act(() => {
        rerender({ uuid: 'uuid-2', submit: jest.fn() })
      })

      act(() => {
        capturedHeightCallback!(50)
      })
      expect(result.current.spacerHeight).toBe(0)
    })
  })

  describe('resize handling', () => {
    it('observes the scroll element', () => {
      const scrollElement = makeScrollElement(500, 300)
      renderUseScrollSpacer(scrollElement)

      expect(mockObserve).toHaveBeenCalledWith(scrollElement.current)
    })

    it('recalculates spacerHeight on resize', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      act(() => {
        result.current.submitInputTextToAPI()
      })
      // Response grows by 100px
      act(() => {
        capturedHeightCallback!(100)
      })
      expect(result.current.spacerHeight).toBe(200) // 300 - 100

      // Viewport grows to 400px
      ;(scrollElement.current as any).clientHeight = 400
      act(() => {
        capturedResizeCallback!()
      })

      // New spacer = 400 (new clientHeight) - 100 (growth since submit) = 300
      expect(result.current.spacerHeight).toBe(300)
    })

    it('does not update spacer on resize when no submit has occurred', () => {
      const scrollElement = makeScrollElement(500, 300)
      const { result } = renderUseScrollSpacer(scrollElement)

      ;(scrollElement.current as any).clientHeight = 400
      act(() => {
        capturedResizeCallback!()
      })

      expect(result.current.spacerHeight).toBe(0)
    })
  })

  describe('cleanup', () => {
    it('disconnects ResizeObserver on unmount', () => {
      const { unmount } = renderUseScrollSpacer(makeScrollElement(500, 300))

      unmount()

      expect(mockDisconnect).toHaveBeenCalled()
    })
  })
})
