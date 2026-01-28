# Asynchronous Web Server

![Language](https://img.shields.io/badge/language-C-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![License](https://img.shields.io/badge/license-BSD--3--Clause-green)

A high-performance, non-blocking HTTP server implementation in C using advanced Linux kernel features. This project demonstrates low-level system programming concepts, including I/O multiplexing, asynchronous file operations, and zero-copy data transfer.

## 🚀 Key Features

* **I/O Multiplexing:** Uses `epoll` (Edge Triggered) to efficiently handle thousands of concurrent connections on a single thread.
* **Non-blocking Architecture:** All sockets are configured as `O_NONBLOCK` to prevent I/O wait times from stalling the CPU.
* **Zero-Copy Transfer:** Implements `sendfile()` for static files, allowing data to move directly from disk cache to the network socket without copying through user space (significantly reducing CPU usage).
* **Asynchronous I/O (Linux AIO):** Uses `libaio` (`io_submit`, `io_getevents`) for dynamic file handling, ensuring that disk reads do not block the event loop.
* **HTTP State Machine:** Implements a custom Finite State Machine (FSM) to handle partial reads/writes and HTTP protocol parsing.

## 🛠️ Architecture

The server operates on an event-driven model:

1.  **Connection Handling:** The main loop waits for events via `epoll_wait`. New connections are accepted and added to the epoll instance.
2.  **Request Parsing:** Uses `http_parser` to extract the request path and headers.
3.  **Resource Routing:**
    * **Static Files (`/static/`):** Served using the `sendfile` syscall for maximum throughput.
    * **Dynamic Files (`/dynamic/`):** Served by triggering an asynchronous read request (`io_submit`). The server continues handling other clients while the disk operation completes in the background. Once the `io_event` is ready, the data is sent to the client.
4.  **State Machine:** Each connection maintains a state (`STATE_INITIAL`, `STATE_RECEIVING_DATA`, `STATE_SENDING_HEADER`, `STATE_ASYNC_ONGOING`, etc.) to handle network fragmentation and non-blocking behavior correctly.

## ⚙️ Prerequisites

* **OS:** Linux (Requires Kernel headers for `epoll` and `aio`).
* **Libraries:** `libaio-dev`.
* **Compiler:** GCC or Clang.

## 📦 Build & Run

### 1. Install Dependencies
On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install libaio-dev
