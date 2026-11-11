#!/usr/bin/env python3
"""Deterministic mock of the AI model services for the ai_function mysqltests.

The mock implements the protocols which are used by the ai functions of
seekdb, so that the test cases of the ai function suite do not depend on any
real model service:

  * POST /compatible-mode/v1/chat/completions  the openai completion protocol
  * POST /compatible-mode/v1/embeddings          the openai embedding protocol
  * POST /v1/rerank                              the siliconflow rerank protocol
  * POST /api/generate                           the ollama completion protocol
  * POST /api/embed                              the ollama embedding protocol

The mock never hard codes the content of the responses, the responses are
derived from the requests, so that the test cases can assert the behavior of
the ai functions instead of the behavior of one single model service.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

# the default dimension of the embedding vectors
DEFAULT_DIMENSION = 768


def state_dir(port):
    return "/tmp/seekdb_ai_function_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, "events.jsonl")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    for name in ("events.jsonl", "server.out"):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("AI function mock did not start on port %d" % port)


def embedding_vector(text, dimension):
    """Build a deterministic vector of the given dimension for the given text."""
    vector = []
    seed = 0
    for char in str(text):
        seed = (seed * 31 + ord(char)) % 1000003
    for index in range(dimension):
        vector.append(round(((seed + index * 7919) % 10000) / 10000.0, 6))
    return vector


def message_text(body):
    """Extract the text of the request, no matter which protocol is used."""
    texts = []
    for message in body.get("messages", []) or []:
        content = message.get("content", "")
        if isinstance(content, list):
            for item in content:
                if isinstance(item, dict):
                    texts.append(str(item.get("text", "")))
                else:
                    texts.append(str(item))
        else:
            texts.append(str(content))
    if not texts:
        if "prompt" in body:
            texts.append(str(body.get("prompt", "")))
        elif "input" in body:
            value = body.get("input")
            if isinstance(value, list):
                texts.extend(str(item) for item in value)
            else:
                texts.append(str(value))
    return "\n".join(text for text in texts if text)


def relevance_score(query, document, index):
    """Score the document against the query, the exact match wins."""
    left = str(query).lower()
    right = str(document).lower()
    if left and left in right:
        return 1.0 - index * 0.001
    words = [word for word in left.split() if word]
    hit = sum(1 for word in words if word in right)
    if words:
        return round(hit / len(words) * 0.5, 6) - index * 0.001
    return round(0.1 - index * 0.001, 6)


def serve(port):
    lock = threading.Lock()
    attempts = {}
    os.makedirs(state_dir(port), exist_ok=True)
    log_path = state_path(port, "events.jsonl")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            path = parsed.path
            text = message_text(body)
            key = path + "|" + text
            with lock:
                attempts[key] = attempts.get(key, 0) + 1
                attempt = attempts[key]

            status = 200
            response = {}
            if path.endswith("/chat/completions"):
                # the openai completion protocol
                response = {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion",
                    "created": 1735120033,
                    "model": str(body.get("model", "mock-model")),
                    "choices": [
                        {
                            "index": 0,
                            "message": {"role": "assistant", "content": "mock: " + text},
                            "finish_reason": "stop",
                        }
                    ],
                    "usage": {
                        "prompt_tokens": len(text.split()),
                        "completion_tokens": 1,
                        "total_tokens": len(text.split()) + 1,
                    },
                }
            elif path.endswith("/embeddings"):
                # the openai embedding protocol
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]
                dimension = body.get("dimensions", DEFAULT_DIMENSION)
                try:
                    dimension = int(dimension)
                except (TypeError, ValueError):
                    dimension = DEFAULT_DIMENSION
                if dimension <= 0:
                    dimension = DEFAULT_DIMENSION
                response = {
                    "object": "list",
                    "model": str(body.get("model", "mock-embedding-model")),
                    "data": [
                        {
                            "object": "embedding",
                            "index": index,
                            "embedding": embedding_vector(value, dimension),
                        }
                        for index, value in enumerate(values)
                    ],
                    "usage": {
                        "prompt_tokens": len(text.split()),
                        "total_tokens": len(text.split()),
                    },
                }
            elif path.endswith("/rerank"):
                # the siliconflow rerank protocol
                query = str(body.get("query", ""))
                documents = body.get("documents", [])
                if not isinstance(documents, list):
                    documents = [documents]
                results = []
                for index, document in enumerate(documents):
                    if isinstance(document, dict):
                        document = document.get("text", "")
                    results.append(
                        {
                            "index": index,
                            "relevance_score": relevance_score(query, document, index),
                        }
                    )
                response = {
                    "id": "rerank-mock",
                    "model": str(body.get("model", "mock-rerank-model")),
                    "results": results,
                    "meta": {"tokens": {"input_tokens": len(query.split())}},
                }
            elif path.endswith("/api/generate"):
                # the ollama completion protocol
                response = {
                    "model": str(body.get("model", "mock-model")),
                    "response": "mock: " + text,
                    "done": True,
                    "done_reason": "stop",
                }
            elif path.endswith("/api/embed"):
                # the ollama embedding protocol
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]
                dimension = body.get("dimensions", DEFAULT_DIMENSION)
                try:
                    dimension = int(dimension)
                except (TypeError, ValueError):
                    dimension = DEFAULT_DIMENSION
                if dimension <= 0:
                    dimension = DEFAULT_DIMENSION
                response = {
                    "model": str(body.get("model", "mock-embedding-model")),
                    "embeddings": [
                        embedding_vector(value, dimension) for value in values
                    ],
                }
            else:
                status = 404
                response = {"error": {"message": "unsupported path: " + path}}

            event = {
                "path": path,
                "status": status,
                "attempt": attempt,
                "authorization": self.headers.get("Authorization", ""),
                "body": body,
            }
            with lock:
                try:
                    with open(log_path, "a", encoding="utf-8") as stream:
                        stream.write(json.dumps(event, ensure_ascii=False) + "\n")
                except OSError:
                    pass
            payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    # the endpoints of the test cases are configured with the address 0.0.0.0
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.serve_forever()


def assert_events(port, assertion):
    events = read_events(port)
    if assertion == "no-requests":
        ok = not events
    elif assertion == "completion":
        ok = any(event["path"].endswith("/chat/completions") for event in events)
    elif assertion == "embedding":
        ok = any(event["path"].endswith("/embeddings") for event in events)
    elif assertion == "rerank":
        ok = any(event["path"].endswith("/rerank") for event in events)
    elif assertion == "ollama":
        ok = any(
            event["path"].endswith("/api/generate")
            or event["path"].endswith("/api/embed")
            for event in events
        )
    elif assertion == "no-authorization":
        ok = events and all(
            event["authorization"] == "" for event in events
        )
    else:
        raise ValueError("unknown assertion: " + assertion)
    if not ok:
        raise AssertionError("assertion %s failed: %r" % (assertion, events))
    print("mock_%s=ok" % assertion.replace("-", "_"))


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: ai_function_mock.py MODE PORT [ASSERTION]")
    mode = sys.argv[1]
    port = int(sys.argv[2])
    if mode == "start":
        start_server(port)
    elif mode == "stop":
        stop_server(port)
    elif mode == "serve":
        serve(port)
    elif mode == "reset":
        with open(state_path(port, "events.jsonl"), "w", encoding="utf-8"):
            pass
    elif mode == "assert":
        assert_events(port, sys.argv[3])
    else:
        raise SystemExit("unknown mode: " + mode)


if __name__ == "__main__":
    main()
