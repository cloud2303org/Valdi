import {
  AbortController,
  Blob,
  DOMException,
  Headers,
  ReadableStream,
  Request,
  Response,
  TextDecoder,
  TextEncoder,
  TransformStream,
  WebSocket,
  WritableStream,
  fetch,
  installNativeWeb,
} from '../src/NativeWeb';
import * as raw from '../src/NativeWebNative';
import { installNativeWeb as installFromBarrel } from '../src/index';
import type { BodyInit, RequestInit, ReadableStreamReadResult } from '../src/NativeWeb';

installNativeWeb();
installFromBarrel();
const install: () => void = installNativeWeb;
const nativeFetch: typeof raw.fetch = fetch;
const body: BodyInit = 'hello';
const requestInit: RequestInit = { body };
const readResult: ReadableStreamReadResult<Uint8Array> = { done: true };
void install;
void nativeFetch;
void requestInit;
void readResult;
// @ts-expect-error The internal factory exposes native Web APIs, no installer.
raw.installNativeWeb();
// @ts-expect-error Installation accepts no options.
installNativeWeb({});

async function readResponse(): Promise<string> {
  const abort = new AbortController();
  const request = new Request('https://example.test/events', {
    method: 'POST',
    headers: new Headers({ accept: 'text/event-stream' }),
    body: 'hello',
    signal: abort.signal,
  });
  const response: Response = await fetch(request);
  const decoder = new TextDecoder();
  const reader = response.body?.getReader();
  let text = '';
  if (reader) {
    for (;;) {
      const result = await reader.read();
      if (result.done) break;
      text += decoder.decode(result.value, { stream: true });
    }
    reader.releaseLock();
  }
  abort.abort();
  return text;
}

const stream = new ReadableStream<Uint8Array>({
  start(controller) {
    controller.enqueue(new TextEncoder().encode('hello'));
    controller.close();
  },
});
const [left, right] = stream.tee();
void new Response(left).text();
void new Response(new Blob(['hello'])).arrayBuffer();
const sink = new WritableStream<Uint8Array>({
  write(chunk) {
    const size: number = chunk.byteLength;
    void size;
  },
});
void right.pipeTo(sink);
const transform = new TransformStream<Uint8Array, string>({
  transform(chunk, controller) {
    controller.enqueue(new TextDecoder().decode(chunk));
  },
});
void transform;

class ApplicationException extends DOMException {}
const exception: Error = new ApplicationException('stopped', 'AbortError');
const abortCode: 20 = DOMException.ABORT_ERR;
const timeoutCode: 23 = new DOMException().TIMEOUT_ERR;
void exception;
void abortCode;
void timeoutCode;
// @ts-expect-error Native DOMException fields are readonly.
new DOMException().name = 'changed';
// @ts-expect-error Native DOMException fields are readonly.
new DOMException().message = 'changed';
// @ts-expect-error Native DOMException fields are readonly.
new DOMException().code = 20;
// @ts-expect-error DOMException is constructor-only.
DOMException('stopped');
// @ts-expect-error DOMException messages are declared as strings.
new DOMException(123);

const socket = new WebSocket('wss://example.test/socket', ['updates']);
socket.binaryType = 'arraybuffer';
socket.onmessage = event => {
  const value: string | Blob | ArrayBuffer = event.data;
  void value;
};
socket.onclose = event => {
  const code: number = event.code;
  void code;
};
socket.send('hello');
socket.send(new Uint8Array([1, 2, 3]));
socket.close(1000, 'done');

// @ts-expect-error Requests require a URL or another Request.
void fetch(123);
// @ts-expect-error The stream's chunk type is Uint8Array.
new ReadableStream<Uint8Array>({ start: controller => controller.enqueue('bad') });
// @ts-expect-error WebSocket binaryType is a finite set.
socket.binaryType = 'utf8';
void readResponse;
