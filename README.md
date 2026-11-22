# HeteroCompute v0.2

HeteroCompute is a web control plane for experiments with a heterogeneous master/worker computing platform.

- Master/control plane: C++20 REST server.
- UI: React + TypeScript + Vite.
- Execution boundary: `IExecutionBackend`.
- Current backend: discrete-event simulation of one serialized SPI resource and up to three independent workers.
- Intended hardware target: Linux master (NXP i.MX91) + STM32F407 workers. Physical SPI/GPIO integration is **not implemented** in this version.

## Architecture

```text
React UI
   | REST
C++ HTTP API -> Controller -> IExecutionBackend
                                  |
                           SimulationBackend
                                  |
                    Event queue + SPI resource
                     /          |          \
                 Worker 1    Worker 2    Worker 3
```

The `IExecutionBackend` interface is the seam for a future `HardwareBackend`. The UI and REST API should not depend on SPI/GPIO details.

## Build and run (Ubuntu 22.04 / WSL2)

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-system-dev nlohmann-json3-dev
```

Build and run backend:

```bash
cmake -S backend -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hetero_backend
```

In another terminal:

```bash
cd frontend
npm install
npm run dev
```

Open <http://localhost:5173>. REST API listens on `http://127.0.0.1:8080`.

## API

- `GET /api/state`
- `POST /api/configure` — `{ "matrix_size":128, "tile_size":32, "worker_count":3, "spi_mbps":10, "wall_delay_ms":4 }`
- `POST /api/start`
- `POST /api/pause`
- `POST /api/resume`
- `POST /api/reset`
- `POST /api/policy` — `{ "policy":"predicted|least_loaded|round_robin" }`
- `POST /api/worker` — `{ "id":0, "enabled":false }`

## Model semantics

Each task is one output tile `C_ij`. Its input payload is approximated as two dense panels of size `N × b` float32; the output tile is `b × b` float32. SPI transfers are serialized globally. Worker computation is modeled independently, allowing compute overlap between workers and with later SPI transfers. A worker becomes idle only after result transfer and ACK event.

The event trace includes input-transfer start/completion, computation completion, READY, result transfer, validation, ACK, and worker-idle events.

## Known limitations — read before interpreting results

1. This is an architectural prototype and a simulator scaffold, not a validated replica of any earlier MATLAB/CSV model.
2. The compute rate and transfer overhead are configurable assumptions, not measured STM32F407/i.MX91 performance.
3. The current scheduling policies are preliminary. `predicted` currently preserves queue order; it does not yet estimate completion time using a calibrated performance model. `least_loaded` is a placeholder policy and needs a proper load metric.
4. Input/result packet framing, CRC, retransmission, sequence numbers, task-attempt IDs, timeout recovery and stale-result rejection are not implemented yet.
5. Worker disable is currently accepted only when a worker is idle. Safe draining/reassignment of in-flight work is a future requirement.
6. No persistent experiment database, WebSocket stream, authentication, or hardware backend is included.
7. The simulator models a single shared SPI bus. GPIO READY/ACK are represented as logical events, not electrical timing.
8. The simple HTTP server is intended for local development only; do not expose it to an untrusted network.

## Next engineering increments

1. Extract a deterministic event engine and unit-test event ordering, task conservation and SPI mutual exclusion.
2. Implement calibrated scheduling policies and runtime policy switching semantics.
3. Add packet protocol and fault injection (CRC error, timeout, dropped ACK, worker loss).
4. Add experiment persistence (SQLite), reproducible run configuration and CSV export.
5. Add WebSocket telemetry.
6. Implement `HardwareBackend` on the i.MX91 Linux host and validate READY/ACK + SPI timing against the real board.
