// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.

// snap_executor.ts runs inside a chrome-untrusted://snap-executor/ iframe.
// One instance is created per snap. SES lockdown() has already been called
// via ses_lockdown.bundle.js before this script executes. Compartment and
// harden are declared by the ses package types and available as globals.

console.error('XXXZZZ snap_executor.ts: bundle loaded')

const INVOKE_TIMEOUT_MS = 60_000

// In-memory state store for snap_manageState — keyed by snapId.
// Mirrors MetaMask's execution environment which handles state locally.
const snapStateStore = new Map<string, unknown>()

// Map from snap ID to the bundle URL path served from this origin.
const SNAP_BUNDLE_URLS: Record<string, string> = {
  'npm:@cosmsnap/snap': 'snap-bundles/cosmos.js',
}

// The snap's onRpcRequest handler, set after load_snap succeeds.
let snapOnRpcRequest:
  | ((args: {
      origin: string
      request: { method: string; params: unknown }
    }) => Promise<unknown>)
  | null = null

// Pending snap.request() calls waiting for a response from the parent.
const pendingSnapRequests = new Map<
  string,
  { resolve: (value: unknown) => void; reject: (reason: Error) => void }
>()

function generateRequestId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2)}`
}

function sendToParent(message: Record<string, unknown>): void {
  window.parent.postMessage(message, '*')
}

function sendResponse(
  requestId: string,
  result?: unknown,
  error?: string,
): void {
  sendToParent({ type: 'response', requestId, result, error })
}

// Returns true if s is a valid, decodable standard-base64 string.
// base64-js (used by @cosmjs/encoding) requires length to be a multiple of 4,
// so we check that here in addition to the character-set test.
function isStandardBase64(s: string): boolean {
  return s.length % 4 === 0 && /^[a-zA-Z0-9+/]*={0,2}$/.test(s)
}

// Walk an arbitrary JSON value and base64-encode any EventAttribute key/value
// that is a plain string (CometBFT v0.38+ changed bytes→string in the proto).
function normaliseEventAttrs(v: unknown): void {
  if (!v || typeof v !== 'object') return
  if (Array.isArray(v)) {
    for (const item of v) normaliseEventAttrs(item)
    return
  }
  const obj = v as Record<string, unknown>
  if (typeof obj['key'] === 'string' && typeof obj['value'] === 'string') {
    if (!isStandardBase64(obj['key']))   obj['key']   = btoa(obj['key'])
    if (!isStandardBase64(obj['value'])) obj['value'] = btoa(obj['value'])
  }
  for (const val of Object.values(obj)) normaliseEventAttrs(val)
}

// Fetch proxy that patches CometBFT v0.38 JSON-RPC responses so the snap's
// bundled @cosmjs/tendermint-rpc v0.30.x can parse event attributes.
async function cometBftFetchProxy(
  input: RequestInfo | URL,
  init?: RequestInit,
): Promise<Response> {
  const response = await fetch(input, init)
  const contentType = response.headers.get('content-type') ?? ''
  if (!contentType.includes('application/json')) return response
  const text = await response.text()
  try {
    const json = JSON.parse(text) as unknown
    normaliseEventAttrs(json)
    return new Response(JSON.stringify(json), {
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
    })
  } catch {
    return new Response(text, {
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
    })
  }
}

async function handleLoadSnap(
  snapId: string,
  requestId: string,
): Promise<void> {
  console.error('XXXZZZ snap_executor handleLoadSnap snapId=', snapId)
  try {
    const bundleUrl = SNAP_BUNDLE_URLS[snapId]
    if (!bundleUrl) {
      sendResponse(requestId, undefined, `No bundle registered for snap: ${snapId}`)
      return
    }
    console.error('XXXZZZ snap_executor: fetching bundle from', bundleUrl)
    const resp = await fetch(bundleUrl)
    if (!resp.ok) {
      sendResponse(requestId, undefined, `Failed to fetch snap bundle: ${resp.status} ${resp.statusText}`)
      return
    }
    const source = await resp.text()
    console.error('XXXZZZ snap_executor: fetched source length=', source.length)
    // snap.request() — called by snap code to invoke core APIs (e.g. key derivation).
    // Sends a snap_request message to the wallet page, which relays it to C++ core,
    // then resolves/rejects when the response arrives via snap_request_response.
    const snapRequestFn = (params: {
      method: string
      params?: unknown
    }): Promise<unknown> => {
      // Handle snap_manageState locally — state is stored per-snap in memory.
      // The C++ side never sees these calls (mirrors MetaMask's snap executor).
      if (params.method === 'snap_manageState') {
        const p = params.params as {
          operation: 'get' | 'update' | 'clear'
          newState?: unknown
        }
        if (p.operation === 'get') {
          return Promise.resolve(snapStateStore.get(snapId) ?? null)
        }
        if (p.operation === 'update') {
          snapStateStore.set(snapId, p.newState)
          return Promise.resolve(null)
        }
        if (p.operation === 'clear') {
          snapStateStore.delete(snapId)
          return Promise.resolve(null)
        }
        return Promise.reject(new Error(`snap_manageState: unknown operation ${(p as { operation: string }).operation}`))
      }

      // Relay all other snap.request() calls to the parent (C++).
      return new Promise((resolve, reject) => {
        const snapReqId = generateRequestId()
        pendingSnapRequests.set(snapReqId, { resolve, reject })
        sendToParent({
          type: 'snap_request',
          snapId,
          method: params.method,
          params: params.params,
          requestId: snapReqId,
        })
      })
    }

    // Expose snap exports so the Compartment can capture them.
    // CommonJS-style snaps set module.exports.onRpcRequest.
    const snapExports: Record<string, unknown> = {}
    const snapModule = { exports: snapExports }

    // Retrieve the original Date constructor captured before SES lockdown.
    // dateTaming:'unsafe' does not fully restore Date.now() inside Compartments.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const SnapDate: typeof Date = (globalThis as any)['__snapDate'] ?? Date

    // Retrieve Math captured before SES lockdown tamed it.
    const snapMath: Record<string, unknown> =
      (globalThis as unknown as Record<string, unknown>)['__snapMath'] as Record<string, unknown>
      ?? Math

    const compartment = new Compartment({
      module: snapModule,
      exports: snapExports,
      snap: harden({ request: snapRequestFn }),
      console: harden({
        log: console.log.bind(console),
        warn: console.warn.bind(console),
        error: console.error.bind(console),
        info: console.info.bind(console),
        debug: console.debug.bind(console),
      }),
      Date: SnapDate,
      Math: harden(snapMath),
      setTimeout: setTimeout.bind(globalThis),
      clearTimeout: clearTimeout.bind(globalThis),
      setInterval: setInterval.bind(globalThis),
      clearInterval: clearInterval.bind(globalThis),
      Promise: Promise,
      // Encoding / crypto globals used by @cosmjs
      atob: atob.bind(globalThis),
      btoa: btoa.bind(globalThis),
      // Typed array constructors that SES may not expose in Compartment scope
      Uint8Array: Uint8Array,
      Int8Array: Int8Array,
      Uint16Array: Uint16Array,
      Int16Array: Int16Array,
      Uint32Array: Uint32Array,
      Int32Array: Int32Array,
      Float32Array: Float32Array,
      Float64Array: Float64Array,
      ArrayBuffer: ArrayBuffer,
      DataView: DataView,
      // crypto.getRandomValues used by some @cosmjs internals
      crypto: globalThis.crypto,
      TextEncoder: harden(TextEncoder),
      TextDecoder: harden(TextDecoder),
      // Network APIs — needed by snaps with endowment:network-access.
      // fetch is not available in SES Compartments by default.
      // Wrapped to normalise CometBFT v0.38+ plain-string event attributes to
      // base64 so @cosmjs/tendermint-rpc v0.30.x (bundled in the snap) can
      // decode them (it expects the old Tendermint v0.34 base64 format).
      fetch: cometBftFetchProxy,
      Headers: harden(Headers),
      Request: harden(Request),
      Response: harden(Response),
      AbortController: harden(AbortController),
      AbortSignal: harden(AbortSignal),
    })

    console.error('XXXZZZ snap_executor: calling compartment.evaluate')
    compartment.evaluate(source)
    console.error('XXXZZZ snap_executor: evaluate done, exports keys=', Object.keys(snapExports))

    const handler = snapExports['onRpcRequest']
    if (typeof handler !== 'function') {
      console.error('XXXZZZ snap_executor ERROR: onRpcRequest not found in exports')
      sendResponse(requestId, undefined, 'Snap does not export onRpcRequest')
      return
    }

    snapOnRpcRequest = handler as typeof snapOnRpcRequest
    console.error('XXXZZZ snap_executor: snap loaded OK')
    sendResponse(requestId, true)
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    const stack = err instanceof Error ? err.stack : ''
    console.error('XXXZZZ snap_executor ERROR in handleLoadSnap:', msg, stack)
    sendResponse(requestId, undefined, `Failed to load snap: ${msg}`)
  }
}

async function handleInvoke(
  method: string,
  params: unknown,
  origin: string,
  requestId: string,
): Promise<void> {
  if (typeof snapOnRpcRequest !== 'function') {
    sendResponse(requestId, undefined, 'Snap not loaded')
    return
  }

  let settled = false

  const timeoutId = setTimeout(() => {
    if (!settled) {
      settled = true
      sendResponse(
        requestId,
        undefined,
        `Snap RPC timed out after ${INVOKE_TIMEOUT_MS}ms`,
      )
    }
  }, INVOKE_TIMEOUT_MS)

  try {
    const result = await snapOnRpcRequest({
      origin,
      request: { method, params },
    })
    console.error('XXXZZZ snap_executor handleInvoke result:', JSON.stringify(result)?.slice(0, 300))
    clearTimeout(timeoutId)
    if (!settled) {
      settled = true
      sendResponse(requestId, result)
    }
  } catch (err) {
    clearTimeout(timeoutId)
    if (!settled) {
      settled = true
      const msg = err instanceof Error ? err.message : String(err)
      sendResponse(requestId, undefined, msg)
    }
  }
}

function handleSnapRequestResponse(
  snapReqId: string,
  result: unknown,
  error?: string,
): void {
  const pending = pendingSnapRequests.get(snapReqId)
  if (!pending) {
    return
  }
  pendingSnapRequests.delete(snapReqId)
  if (error) {
    pending.reject(new Error(error))
  } else {
    pending.resolve(result)
  }
}

window.addEventListener('message', (event: MessageEvent) => {
  // Only accept messages from the parent wallet page.
  if (event.source !== window.parent) {
    return
  }

  const data = event.data as Record<string, unknown>
  if (!data || typeof data !== 'object' || typeof data['type'] !== 'string') {
    return
  }

  switch (data['type']) {
    case 'load_snap':
      handleLoadSnap(
        data['snapId'] as string,
        data['requestId'] as string,
      )
      break

    case 'invoke':
      handleInvoke(
        data['method'] as string,
        data['params'],
        (data['origin'] as string) ?? '',
        data['requestId'] as string,
      )
      break

    case 'snap_request_response':
      // Core (C++) responded to a snap.request() call relayed by the bridge.
      handleSnapRequestResponse(
        data['requestId'] as string,
        data['result'],
        data['error'] as string | undefined,
      )
      break

    case 'proxy_fetch':
      // Proxy an HTTP GET through this iframe (which has connect-src *).
      // Used by the wallet page which has a strict CSP.
      void (async () => {
        const requestId = data['requestId'] as string
        const url = data['url'] as string
        try {
          const res = await fetch(url)
          const text = await res.text()
          sendToParent({ type: 'response', requestId, result: text })
        } catch (err) {
          const msg = err instanceof Error ? err.message : String(err)
          sendToParent({ type: 'response', requestId, result: undefined, error: msg })
        }
      })()
      break
  }
})
