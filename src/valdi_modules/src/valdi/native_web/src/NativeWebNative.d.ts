// Generated from NSH Rust Web API metadata and native C++ additions; do not edit.
// Upstream: longbridge/nsh, Apache-2.0.
// Commit: 701bdcee9512e0f6aed39042d1fe24564dcd0b43.

export class DOMException extends Error {
  constructor(message?: string, name?: string);
  readonly message: string;
  readonly name: string;
  readonly code: number;
  static readonly INDEX_SIZE_ERR: 1;
  readonly INDEX_SIZE_ERR: 1;
  static readonly DOMSTRING_SIZE_ERR: 2;
  readonly DOMSTRING_SIZE_ERR: 2;
  static readonly HIERARCHY_REQUEST_ERR: 3;
  readonly HIERARCHY_REQUEST_ERR: 3;
  static readonly WRONG_DOCUMENT_ERR: 4;
  readonly WRONG_DOCUMENT_ERR: 4;
  static readonly INVALID_CHARACTER_ERR: 5;
  readonly INVALID_CHARACTER_ERR: 5;
  static readonly NO_DATA_ALLOWED_ERR: 6;
  readonly NO_DATA_ALLOWED_ERR: 6;
  static readonly NO_MODIFICATION_ALLOWED_ERR: 7;
  readonly NO_MODIFICATION_ALLOWED_ERR: 7;
  static readonly NOT_FOUND_ERR: 8;
  readonly NOT_FOUND_ERR: 8;
  static readonly NOT_SUPPORTED_ERR: 9;
  readonly NOT_SUPPORTED_ERR: 9;
  static readonly INUSE_ATTRIBUTE_ERR: 10;
  readonly INUSE_ATTRIBUTE_ERR: 10;
  static readonly INVALID_STATE_ERR: 11;
  readonly INVALID_STATE_ERR: 11;
  static readonly SYNTAX_ERR: 12;
  readonly SYNTAX_ERR: 12;
  static readonly INVALID_MODIFICATION_ERR: 13;
  readonly INVALID_MODIFICATION_ERR: 13;
  static readonly NAMESPACE_ERR: 14;
  readonly NAMESPACE_ERR: 14;
  static readonly INVALID_ACCESS_ERR: 15;
  readonly INVALID_ACCESS_ERR: 15;
  static readonly VALIDATION_ERR: 16;
  readonly VALIDATION_ERR: 16;
  static readonly TYPE_MISMATCH_ERR: 17;
  readonly TYPE_MISMATCH_ERR: 17;
  static readonly SECURITY_ERR: 18;
  readonly SECURITY_ERR: 18;
  static readonly NETWORK_ERR: 19;
  readonly NETWORK_ERR: 19;
  static readonly ABORT_ERR: 20;
  readonly ABORT_ERR: 20;
  static readonly URL_MISMATCH_ERR: 21;
  readonly URL_MISMATCH_ERR: 21;
  static readonly QUOTA_EXCEEDED_ERR: 22;
  readonly QUOTA_EXCEEDED_ERR: 22;
  static readonly TIMEOUT_ERR: 23;
  readonly TIMEOUT_ERR: 23;
  static readonly INVALID_NODE_TYPE_ERR: 24;
  readonly INVALID_NODE_TYPE_ERR: 24;
  static readonly DATA_CLONE_ERR: 25;
  readonly DATA_CLONE_ERR: 25;
}

export type EventListenerOrEventListenerObject = ((event: Event) => void) | { handleEvent(event: Event): void };

export interface AddEventListenerOptions {
  capture?: boolean;
  once?: boolean;
  passive?: boolean;
  signal?: AbortSignal;
}

export interface EventInit {
  bubbles?: boolean;
  cancelable?: boolean;
  composed?: boolean;
}

export type BufferSource = ArrayBuffer | ArrayBufferView;

export type BlobPart = BufferSource | Blob | string;

export interface BlobPropertyBag {
  type?: string;
  endings?: "transparent" | "native";
}

export type FormDataEntryValue = File | string;

export interface QueuingStrategy<T = any> {
  highWaterMark?: number;
  size?: (chunk: T) => number;
}

export interface UnderlyingDefaultSource<R = any> {
  start?(controller: ReadableStreamDefaultController<R>): any;
  pull?(controller: ReadableStreamDefaultController<R>): void | PromiseLike<void>;
  cancel?(reason?: any): void | PromiseLike<void>;
  type?: undefined;
}

export interface UnderlyingByteSource {
  type: "bytes";
  autoAllocateChunkSize?: number;
  start?(controller: ReadableByteStreamController): any;
  pull?(controller: ReadableByteStreamController): void | PromiseLike<void>;
  cancel?(reason?: any): void | PromiseLike<void>;
}

export type ReadableStreamReadResult<T> = { done: false; value: T } | { done: true; value?: T };

export interface StreamPipeOptions {
  preventAbort?: boolean;
  preventCancel?: boolean;
  preventClose?: boolean;
  signal?: AbortSignal;
}

export interface UnderlyingSink<W = any> {
  start?(controller: WritableStreamDefaultController): any;
  write?(chunk: W, controller: WritableStreamDefaultController): void | PromiseLike<void>;
  close?(): void | PromiseLike<void>;
  abort?(reason?: any): void | PromiseLike<void>;
  type?: undefined;
}

export interface Transformer<I = any, O = any> {
  start?(controller: TransformStreamDefaultController<O>): any;
  transform?(chunk: I, controller: TransformStreamDefaultController<O>): void | PromiseLike<void>;
  flush?(controller: TransformStreamDefaultController<O>): void | PromiseLike<void>;
  cancel?(reason?: any): void | PromiseLike<void>;
}

export type HeadersInit = Headers | Record<string, string> | Iterable<[string, string]>;

export type BodyInit = ReadableStream<Uint8Array> | Blob | BufferSource | FormData | URLSearchParams | string;

export interface Body {
  /** The body as it arrives (a byte stream), or null. */
  readonly body: ReadableStream<Uint8Array> | null;
  readonly bodyUsed: boolean;
  arrayBuffer(): Promise<ArrayBuffer>;
  blob(): Promise<Blob>;
  bytes(): Promise<Uint8Array>;
  formData(): Promise<FormData>;
  json(): Promise<any>;
  text(): Promise<string>;
}

export interface RequestInit {
  method?: string;
  headers?: HeadersInit;
  /** A ReadableStream is sent as it is read (chunked), with `duplex: "half"`. */
  body?: BodyInit | null;
  duplex?: "half";
  signal?: AbortSignal | null;
  /** "manual" gives the 3xx response itself. */
  redirect?: "follow" | "manual" | "error";
  credentials?: "omit" | "same-origin" | "include";
  cache?: "default" | "no-store" | "reload" | "no-cache" | "force-cache" | "only-if-cached";
  mode?: "same-origin" | "no-cors" | "cors";
  referrer?: string;
  referrerPolicy?: string;
  integrity?: string;
  keepalive?: boolean;
  priority?: "high" | "low" | "auto";
}

export interface ResponseInit {
  status?: number;
  statusText?: string;
  headers?: HeadersInit;
}

export interface WebSocketMessageEvent extends Event { readonly data: string | Blob | ArrayBuffer; }
export interface WebSocketCloseEvent extends Event { readonly code: number; readonly reason: string; readonly wasClean: boolean; }
export interface WebSocketErrorEvent extends Event { readonly message: string; }

export class EventTarget {
  constructor();
  addEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject | null,
    options?: AddEventListenerOptions | boolean,
  ): void;
  removeEventListener(
    type: string,
    listener: EventListenerOrEventListenerObject | null,
    options?: { capture?: boolean } | boolean,
  ): void;
  dispatchEvent(event: Event): boolean;
}

export class Event {
  static readonly NONE: number;
  readonly NONE: number;
  static readonly CAPTURING_PHASE: number;
  readonly CAPTURING_PHASE: number;
  static readonly AT_TARGET: number;
  readonly AT_TARGET: number;
  static readonly BUBBLING_PHASE: number;
  readonly BUBBLING_PHASE: number;
  constructor(type: string, init?: EventInit);
  readonly type: string;
  readonly target: EventTarget | null;
  readonly currentTarget: EventTarget | null;
  readonly eventPhase: number;
  readonly bubbles: boolean;
  readonly cancelable: boolean;
  readonly defaultPrevented: boolean;
  readonly composed: boolean;
  readonly isTrusted: boolean;
  readonly timeStamp: number;
  returnValue: boolean;
  /** `target` (legacy). */
  readonly srcElement: EventTarget | null;
  /** Sets `type`, `bubbles` and `cancelable` of an event not dispatched yet (legacy). */
  initEvent(type: string, bubbles?: boolean, cancelable?: boolean): void;
  cancelBubble: boolean;
  composedPath(): EventTarget[];
  preventDefault(): void;
  stopPropagation(): void;
  stopImmediatePropagation(): void;
}

export class CustomEvent<T = any> extends Event {
  constructor(type: string, init?: EventInit & { detail?: T });
  readonly detail: T;
}

export class AbortSignal extends EventTarget {
  readonly aborted: boolean;
  readonly reason: any;
  onabort: ((event: Event) => void) | null;
  throwIfAborted(): void;
  static abort(reason?: any): AbortSignal;
  static timeout(milliseconds: number): AbortSignal;
  static any(signals: Iterable<AbortSignal>): AbortSignal;
}

export class AbortController {
  readonly signal: AbortSignal;
  abort(reason?: any): void;
}

export class URLSearchParams {
  constructor(init?: string | Record<string, string> | Iterable<[string, string]> | URLSearchParams);
  readonly size: number;
  append(name: string, value: string): void;
  delete(name: string, value?: string): void;
  get(name: string): string | null;
  getAll(name: string): string[];
  has(name: string, value?: string): boolean;
  set(name: string, value: string): void;
  sort(): void;
  forEach(callback: (value: string, key: string, parent: URLSearchParams) => void, thisArg?: unknown): void;
  entries(): IterableIterator<[string, string]>;
  keys(): IterableIterator<string>;
  values(): IterableIterator<string>;
  [Symbol.iterator](): IterableIterator<[string, string]>;
  toString(): string;
}

export class URL {
  constructor(url: string | URL, base?: string | URL);
  static canParse(url: string | URL, base?: string | URL): boolean;
  static parse(url: string | URL, base?: string | URL): URL | null;
  /** A `blob:` URL `fetch` reads `blob` from, until revoked. */
  static createObjectURL(blob: Blob): string;
  static revokeObjectURL(url: string): void;
  href: string;
  readonly origin: string;
  protocol: string;
  username: string;
  password: string;
  host: string;
  hostname: string;
  port: string;
  pathname: string;
  search: string;
  hash: string;
  readonly searchParams: URLSearchParams;
  toString(): string;
  toJSON(): string;
}

export class TextEncoder {
  readonly encoding: "utf-8";
  encode(input?: string): Uint8Array;
  encodeInto(source: string, destination: Uint8Array): { read: number; written: number };
}

export class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: string;
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: BufferSource, options?: { stream?: boolean }): string;
}

export class Blob {
  constructor(blobParts?: Iterable<BlobPart>, options?: BlobPropertyBag);
  readonly size: number;
  readonly type: string;
  slice(start?: number, end?: number, contentType?: string): Blob;
  arrayBuffer(): Promise<ArrayBuffer>;
  bytes(): Promise<Uint8Array>;
  text(): Promise<string>;
  stream(): ReadableStream<Uint8Array>;
}

export class File extends Blob {
  constructor(fileBits: Iterable<BlobPart>, fileName: string, options?: BlobPropertyBag & { lastModified?: number });
  readonly name: string;
  readonly lastModified: number;
  readonly webkitRelativePath: string;
}

export class FormData {
  constructor();
  append(name: string, value: string | Blob, fileName?: string): void;
  delete(name: string): void;
  get(name: string): FormDataEntryValue | null;
  getAll(name: string): FormDataEntryValue[];
  has(name: string): boolean;
  set(name: string, value: string | Blob, fileName?: string): void;
  forEach(callback: (value: FormDataEntryValue, key: string, parent: FormData) => void, thisArg?: unknown): void;
  entries(): IterableIterator<[string, FormDataEntryValue]>;
  keys(): IterableIterator<string>;
  values(): IterableIterator<FormDataEntryValue>;
  [Symbol.iterator](): IterableIterator<[string, FormDataEntryValue]>;
}

export class ByteLengthQueuingStrategy implements QueuingStrategy<ArrayBufferView> {
  constructor(init: { highWaterMark: number });
  readonly highWaterMark: number;
  readonly size: (chunk: ArrayBufferView) => number;
}

export class CountQueuingStrategy implements QueuingStrategy {
  constructor(init: { highWaterMark: number });
  readonly highWaterMark: number;
  readonly size: (chunk: any) => 1;
}

export class ReadableStream<R = any> {
  constructor(underlyingSource: UnderlyingByteSource, strategy?: { highWaterMark?: number });
  constructor(underlyingSource?: UnderlyingDefaultSource<R>, strategy?: QueuingStrategy<R>);
  static from<R>(iterable: Iterable<R | PromiseLike<R>> | AsyncIterable<R>): ReadableStream<R>;
  readonly locked: boolean;
  cancel(reason?: any): Promise<void>;
  getReader(options: { mode: "byob" }): ReadableStreamBYOBReader;
  getReader(): ReadableStreamDefaultReader<R>;
  pipeThrough<T>(transform: { writable: WritableStream<R>; readable: ReadableStream<T> }, options?: StreamPipeOptions): ReadableStream<T>;
  pipeTo(destination: WritableStream<R>, options?: StreamPipeOptions): Promise<void>;
  tee(): [ReadableStream<R>, ReadableStream<R>];
  values(options?: { preventCancel?: boolean }): AsyncIterableIterator<R>;
  [Symbol.asyncIterator](options?: { preventCancel?: boolean }): AsyncIterableIterator<R>;
}

export class ReadableStreamDefaultReader<R = any> {
  constructor(stream: ReadableStream<R>);
  readonly closed: Promise<void>;
  read(): Promise<ReadableStreamReadResult<R>>;
  releaseLock(): void;
  cancel(reason?: any): Promise<void>;
}

export class ReadableStreamBYOBReader {
  constructor(stream: ReadableStream<Uint8Array>);
  readonly closed: Promise<void>;
  read<T extends ArrayBufferView>(view: T, options?: { min?: number }): Promise<ReadableStreamReadResult<T>>;
  releaseLock(): void;
  cancel(reason?: any): Promise<void>;
}

export class ReadableStreamDefaultController<R = any> {
  readonly desiredSize: number | null;
  close(): void;
  enqueue(chunk?: R): void;
  error(e?: any): void;
}

export class ReadableByteStreamController {
  readonly byobRequest: ReadableStreamBYOBRequest | null;
  readonly desiredSize: number | null;
  close(): void;
  enqueue(chunk: ArrayBufferView): void;
  error(e?: any): void;
}

export class ReadableStreamBYOBRequest {
  readonly view: ArrayBufferView | null;
  respond(bytesWritten: number): void;
  respondWithNewView(view: ArrayBufferView): void;
}

export class WritableStream<W = any> {
  constructor(underlyingSink?: UnderlyingSink<W>, strategy?: QueuingStrategy<W>);
  readonly locked: boolean;
  abort(reason?: any): Promise<void>;
  close(): Promise<void>;
  getWriter(): WritableStreamDefaultWriter<W>;
}

export class WritableStreamDefaultWriter<W = any> {
  constructor(stream: WritableStream<W>);
  readonly closed: Promise<void>;
  readonly desiredSize: number | null;
  readonly ready: Promise<void>;
  abort(reason?: any): Promise<void>;
  close(): Promise<void>;
  releaseLock(): void;
  write(chunk?: W): Promise<void>;
}

export class WritableStreamDefaultController {
  readonly signal: AbortSignal;
  error(e?: any): void;
}

export class TransformStream<I = any, O = any> {
  constructor(transformer?: Transformer<I, O>, writableStrategy?: QueuingStrategy<I>, readableStrategy?: QueuingStrategy<O>);
  readonly readable: ReadableStream<O>;
  readonly writable: WritableStream<I>;
}

export class TransformStreamDefaultController<O = any> {
  readonly desiredSize: number | null;
  enqueue(chunk?: O): void;
  error(reason?: any): void;
  terminate(): void;
}

export class TextEncoderStream {
  readonly encoding: "utf-8";
  readonly readable: ReadableStream<Uint8Array>;
  readonly writable: WritableStream<string>;
}

export class TextDecoderStream {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: string;
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  readonly readable: ReadableStream<string>;
  readonly writable: WritableStream<BufferSource>;
}

export class Headers {
  constructor(init?: HeadersInit);
  append(name: string, value: string): void;
  delete(name: string): void;
  get(name: string): string | null;
  getSetCookie(): string[];
  has(name: string): boolean;
  set(name: string, value: string): void;
  forEach(callback: (value: string, key: string, parent: Headers) => void, thisArg?: unknown): void;
  entries(): IterableIterator<[string, string]>;
  keys(): IterableIterator<string>;
  values(): IterableIterator<string>;
  [Symbol.iterator](): IterableIterator<[string, string]>;
}

export class Request implements Body {
  constructor(input: string | URL | Request, init?: RequestInit);
  readonly method: string;
  readonly url: string;
  readonly headers: Headers;
  readonly destination: string;
  readonly referrer: string;
  readonly referrerPolicy: string;
  readonly mode: string;
  readonly credentials: string;
  readonly cache: string;
  readonly redirect: "follow" | "manual" | "error";
  readonly integrity: string;
  readonly keepalive: boolean;
  readonly isReloadNavigation: boolean;
  readonly isHistoryNavigation: boolean;
  readonly signal: AbortSignal;
  readonly duplex: "half";
  readonly body: ReadableStream<Uint8Array> | null;
  readonly bodyUsed: boolean;
  arrayBuffer(): Promise<ArrayBuffer>;
  blob(): Promise<Blob>;
  bytes(): Promise<Uint8Array>;
  formData(): Promise<FormData>;
  json(): Promise<any>;
  text(): Promise<string>;
  clone(): Request;
}

export class Response implements Body {
  constructor(body?: BodyInit | null, init?: ResponseInit);
  static json(data: unknown, init?: ResponseInit): Response;
  static error(): Response;
  static redirect(url: string | URL, status?: number): Response;
  readonly type: "basic" | "default" | "error";
  readonly url: string;
  readonly redirected: boolean;
  readonly status: number;
  readonly ok: boolean;
  readonly statusText: string;
  readonly headers: Headers;
  readonly body: ReadableStream<Uint8Array> | null;
  readonly bodyUsed: boolean;
  arrayBuffer(): Promise<ArrayBuffer>;
  blob(): Promise<Blob>;
  bytes(): Promise<Uint8Array>;
  formData(): Promise<FormData>;
  json(): Promise<any>;
  text(): Promise<string>;
  clone(): Response;
}

export class WebSocket extends EventTarget {
  static readonly CONNECTING: number;
  readonly CONNECTING: number;
  static readonly OPEN: number;
  readonly OPEN: number;
  static readonly CLOSING: number;
  readonly CLOSING: number;
  static readonly CLOSED: number;
  readonly CLOSED: number;
  constructor(url: string | URL, protocols?: string | string[]);
  readonly url: string;
  readonly readyState: number;
  readonly protocol: string;
  readonly extensions: string;
  readonly bufferedAmount: number;
  binaryType: "blob" | "arraybuffer";
  onopen: ((event: Event) => void) | null;
  onmessage: ((event: WebSocketMessageEvent) => void) | null;
  onerror: ((event: WebSocketErrorEvent) => void) | null;
  onclose: ((event: WebSocketCloseEvent) => void) | null;
  send(data: string | Blob | ArrayBuffer | ArrayBufferView): void;
  close(code?: number, reason?: string): void;
}

export function fetch(input: string | URL | Request, init?: RequestInit): Promise<Response>;
