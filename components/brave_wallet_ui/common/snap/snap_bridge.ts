// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.

// SnapBridge: manages snap iframes and bridges between C++ SnapController
// and chrome-untrusted://snap-executor/ iframes via Mojo + postMessage.
//
// Architecture:
//   C++ SnapController  <--Mojo-->  SnapBridge (this)  <--postMessage-->  iframe(s)
//
// The SnapBridge implements the SnapBridge Mojo interface (called by C++) and
// holds a SnapRequestHandler remote to call back into C++ when a snap calls
// snap.request().

import { BraveWallet } from '../../constants/types'
import type { Value as MojoValue } from 'chrome://resources/mojo/mojo/public/mojom/base/values.mojom-webui.js'
import {
  makeLoadSnapCommand,
  makeInvokeSnapCommand,
  makeSnapRequestResponse,
  isSnapResponse,
  isSnapRequestMessage,
  type SnapRequestMessage,
} from './snap_messages'

const SNAP_EXECUTOR_ORIGIN = 'chrome-untrusted://snap-executor'

// Convert a mojo_base.mojom.Value tagged union to a plain JS value.
export function mojoValueToJs(v: MojoValue | null | undefined): unknown {
  if (v === null || v === undefined) { return null }
  if (v.nullValue !== undefined) { return null }
  if (v.boolValue !== undefined) { return v.boolValue }
  if (v.intValue !== undefined) { return v.intValue }
  if (v.doubleValue !== undefined) { return v.doubleValue }
  if (v.stringValue !== undefined) { return v.stringValue }
  if (v.listValue !== undefined) {
    return v.listValue.storage.map((item) => mojoValueToJs(item))
  }
  if (v.dictionaryValue !== undefined) {
    const obj: Record<string, unknown> = {}
    for (const [k, val] of Object.entries(v.dictionaryValue.storage)) {
      obj[k] = mojoValueToJs(val)
    }
    return obj
  }
  return null
}

// Convert a plain JS value to a mojo_base.mojom.Value tagged union.
export function jsToMojoValue(v: unknown): MojoValue {
  if (v === null || v === undefined) { return { nullValue: 0 } }
  if (typeof v === 'boolean') { return { boolValue: v } }
  if (typeof v === 'number') {
    return Number.isInteger(v) ? { intValue: v } : { doubleValue: v }
  }
  if (typeof v === 'string') { return { stringValue: v } }
  if (Array.isArray(v)) {
    return { listValue: { storage: v.map((item) => jsToMojoValue(item)) } }
  }
  if (typeof v === 'object') {
    const storage: Record<string, MojoValue> = {}
    for (const [k, val] of Object.entries(v as Record<string, unknown>)) {
      storage[k] = jsToMojoValue(val)
    }
    return { dictionaryValue: { storage } }
  }
  return { nullValue: 0 }
}

function generateRequestId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2)}`
}

type PendingIframeRequest = {
  resolve: (value: { result?: unknown; error?: string }) => void
  reject: (reason: Error) => void
}

export class SnapBridge {
  // One iframe per snap, keyed by snapId.
  private readonly iframes = new Map<string, HTMLIFrameElement>()

  // Pending load_snap / invoke responses, keyed by requestId.
  private readonly pending = new Map<string, PendingIframeRequest>()

  // Remote to call C++ SnapRequestHandler when snap code calls snap.request().
  // Set via setSnapRequestHandler() once the Mojo pipe is established (step 12).
  private snapRequestHandler: BraveWallet.SnapRequestHandlerRemote | null =
    null

  // The Mojo receiver wrapping this class (created in step 12).
  private receiver: BraveWallet.SnapBridgeReceiver | null = null

  // Element to which snap iframes are appended.
  private readonly container: HTMLElement

  constructor(container?: HTMLElement) {
    this.container = container ?? document.body
    window.addEventListener('message', this.onMessage)
  }

  // Called during wallet page initialisation (step 12) to wire up the
  // SnapRequestHandler remote that lets us call back into C++.
  setSnapRequestHandler(
    handler: BraveWallet.SnapRequestHandlerRemote,
  ): void {
    this.snapRequestHandler = handler
  }

  // Returns the Mojo receiver for this bridge so the pipe endpoint can be
  // handed to C++ (used in step 12).
  bindNewPipeAndPassRemote() {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    this.receiver = new BraveWallet.SnapBridgeReceiver(this as any)
    return this.receiver.$.bindNewPipeAndPassRemote()
  }

  // -------------------------------------------------------------------------
  // SnapBridge Mojo interface — called by C++ SnapController
  // -------------------------------------------------------------------------

  async loadSnap(
    snapId: string,
  ): Promise<{ success: boolean; error: string | null }> {
    try {
      console.error('XXXZZZ SnapBridge.loadSnap', snapId)
      let iframe = this.iframes.get(snapId)
      if (!iframe) {
        console.error('XXXZZZ SnapBridge.loadSnap: creating iframe')
        iframe = await this.createAndWaitForIframe(snapId)
        console.error('XXXZZZ SnapBridge.loadSnap: iframe loaded', iframe.src)
      }

      const requestId = generateRequestId()
      console.error('XXXZZZ SnapBridge.loadSnap: sending load_snap requestId=', requestId)
      const response = await this.sendToIframe(
        iframe,
        makeLoadSnapCommand(snapId, requestId),
        requestId,
      )
      console.error('XXXZZZ SnapBridge.loadSnap: got response from iframe', response)

      if (response.error) {
        console.error('XXXZZZ SnapBridge.loadSnap ERROR:', response.error)
        return { success: false, error: response.error }
      }
      return { success: true, error: null }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('XXXZZZ SnapBridge.loadSnap ERROR (thrown):', msg)
      return { success: false, error: msg }
    }
  }

  async invokeSnap(
    snapId: string,
    method: string,
    params: unknown,
  ): Promise<{ result: unknown | null; error: string | null }> {
    try {
      const iframe = this.iframes.get(snapId)
      if (!iframe) {
        return { result: null, error: `Snap '${snapId}' is not loaded` }
      }

      // Convert Mojo tagged-union params to a plain JS value for postMessage.
      const plainParams = mojoValueToJs(params as MojoValue)

      const requestId = generateRequestId()
      console.error('XXXZZZ SnapBridge.invokeSnap sending invoke to iframe, method=', method)
      const response = await this.sendToIframe(
        iframe,
        makeInvokeSnapCommand(method, plainParams, this.getCallerOrigin(), requestId),
        requestId,
      )
      console.error('XXXZZZ SnapBridge.invokeSnap got response', response)

      if (response.error) {
        return { result: null, error: response.error }
      }
      // Convert plain JS result back to Mojo tagged union for serialization.
      const mojoResult = response.result !== undefined && response.result !== null
        ? jsToMojoValue(response.result)
        : null
      return { result: mojoResult, error: null }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('XXXZZZ SnapBridge.invokeSnap ERROR:', msg)
      return { result: null, error: msg }
    }
  }

  // Proxy an HTTP GET through the snap executor iframe (which has connect-src *).
  // The wallet page CSP is too strict for direct external fetches.
  async proxyFetch(snapId: string, url: string): Promise<string> {
    const iframe = this.iframes.get(snapId)
    if (!iframe) {
      throw new Error('Snap not loaded — load snap before proxyFetch')
    }
    const requestId = generateRequestId()
    const response = await this.sendToIframe(
      iframe,
      { type: 'proxy_fetch', url, requestId },
      requestId,
    )
    if (response.error) {
      throw new Error(response.error)
    }
    return (response.result as string) ?? ''
  }

  unloadSnap(snapId: string): void {
    const iframe = this.iframes.get(snapId)
    if (iframe) {
      iframe.remove()
      this.iframes.delete(snapId)
    }
  }

  // -------------------------------------------------------------------------
  // Private helpers
  // -------------------------------------------------------------------------

  // Creates the iframe and waits for it to finish loading before returning.
  private createAndWaitForIframe(
    snapId: string,
  ): Promise<HTMLIFrameElement> {
    return new Promise((resolve) => {
      const iframe = document.createElement('iframe')
      iframe.src = 'chrome-untrusted://snap-executor/'
      // allow-same-origin is required for chrome-untrusted:// WebUI to
      // initialise correctly (mirrors ledger-bridge pattern). The iframe
      // origin is chrome-untrusted://snap-executor — cross-origin from the
      // parent chrome://wallet — so allow-same-origin does NOT grant access
      // to the parent's DOM or storage.
      iframe.setAttribute('sandbox', 'allow-scripts allow-same-origin')
      iframe.style.display = 'none'
      iframe.addEventListener('load', () => resolve(iframe), { once: true })
      this.container.appendChild(iframe)
      this.iframes.set(snapId, iframe)
      console.error('XXXZZZ SnapBridge: iframe attached to DOM for', snapId)
    })
  }

  // Posts a message to the iframe and returns a promise that resolves when
  // the iframe sends back a 'response' message with the matching requestId.
  private sendToIframe(
    iframe: HTMLIFrameElement,
    message: object,
    requestId: string,
  ): Promise<{ result?: unknown; error?: string }> {
    return new Promise((resolve, reject) => {
      this.pending.set(requestId, { resolve, reject })
      iframe.contentWindow?.postMessage(message, SNAP_EXECUTOR_ORIGIN)
    })
  }

  // Returns the top-level page origin to pass as 'origin' to snap RPC calls.
  private getCallerOrigin(): string {
    return window.location.origin
  }

  // Global message handler — receives messages from all snap iframes.
  private readonly onMessage = (event: MessageEvent): void => {
    if (event.origin !== SNAP_EXECUTOR_ORIGIN) {
      return
    }

    const data = event.data as Record<string, unknown>
    if (!data || typeof data !== 'object') {
      return
    }

    if (isSnapResponse(data)) {
      // Response to a load_snap or invoke command.
      const req = this.pending.get(data.requestId as string)
      if (req) {
        this.pending.delete(data.requestId as string)
        req.resolve({ result: data.result, error: data.error })
      }
      return
    }

    if (isSnapRequestMessage(data)) {
      // Snap code called snap.request() — relay to C++ and return the result.
      void this.relaySnapRequest(data)
    }
  }

  // Relays a snap.request() call from the iframe to C++ and posts the
  // response back to the originating iframe.
  private async relaySnapRequest(msg: SnapRequestMessage): Promise<void> {
    const iframe = this.iframes.get(msg.snapId)
    if (!iframe) {
      return
    }

    const postResponse = (result?: unknown, error?: string) => {
      iframe.contentWindow?.postMessage(
        makeSnapRequestResponse(msg.requestId, result, error),
        SNAP_EXECUTOR_ORIGIN,
      )
    }

    if (!this.snapRequestHandler) {
      postResponse(undefined, 'SnapRequestHandler not connected')
      return
    }

    try {
      // JSON round-trip strips MetaMask snap Component class instances
      // (panel/text/heading) to plain objects, then wrap as Mojo Value union.
      let plainParams: unknown = {}
      try {
        plainParams = JSON.parse(JSON.stringify(msg.params ?? {}))
      } catch {
        plainParams = {}
      }

      const { result, error } =
        await this.snapRequestHandler.handleSnapRequest(
          msg.snapId,
          msg.method,
          jsToMojoValue(plainParams) as any,
        )
      // Convert Mojo Value result back to plain JS for the snap executor.
      postResponse(mojoValueToJs(result ?? null), error ?? undefined)
    } catch (err) {
      const msg2 = err instanceof Error ? err.message : String(err)
      postResponse(undefined, msg2)
    }
  }
}
