# HeteroCompute v0.4

HeteroCompute is a web application for configuring and monitoring experiments with a heterogeneous master/worker computing system.

- **Backend:** C++20, Boost.Asio, Boost.Beast, nlohmann/json.
- **Frontend:** React, TypeScript, Vite.
- **Architecture:** REST API, Controller, `IExecutionBackend` interface.
- **Current execution backend:** `SimulationBackend`.
- **Simulation model:** one shared SPI resource and up to three independent workers.
- **Intended hardware platform:** NXP i.MX91 Linux master and STM32F407 workers. Physical SPI and GPIO integration is not implemented.

## Architecture

```text
React UI
   |
   | REST API
   |
HTTP Server
   |
Controller
   |
IExecutionBackend
   |
SimulationBackend
   |
Event Queue + Shared SPI Resource
   |
Worker 1 | Worker 2 | Worker 3
```

The `IExecutionBackend` interface separates the control layer from the execution backend and provides an extension point for a future hardware implementation.

## Build and Run

### Dependencies

Ubuntu 22.04 / WSL2:

```bash
sudo apt update
sudo apt install -y build-essential cmake libboost-system-dev nlohmann-json3-dev
```

Frontend requirements: Node.js and npm.

### Backend

```bash
cmake -S backend -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hetero_backend
```

The REST API listens on port `8080`.

### Frontend

In a separate terminal:

```bash
cd frontend
npm install
npm run dev
```

Open `http://localhost:5173`.

## REST API

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/state` | Returns the current system state |
| GET | `/api/log.txt` | Exports the experiment event log |
| POST | `/api/configure` | Configures matrix dimensions and worker count |
| POST | `/api/start` | Starts an experiment |
| POST | `/api/pause` | Pauses execution |
| POST | `/api/resume` | Resumes execution |
| POST | `/api/reset` | Resets the current experiment |
| POST | `/api/policy` | Changes the worker assignment policy |
| POST | `/api/worker` | Enables or disables a worker |

Example configuration:

```json
{
  "matrix_size": 128,
  "tile_size": 32,
  "worker_count": 3,
  "spi_mbps": 10,
  "wall_delay_ms": 4
}
```

Available assignment policies:

- `round_robin`
- `least_loaded`
- `predicted`

## Simulation Model

The matrix multiplication task is divided into output tiles \(C_{ij}\). Each task is assigned to one worker.

The simulator models the following stages:

1. Assignment of a task to an available worker.
2. Transfer of input matrix data through the shared SPI resource.
3. Independent computation on the worker.
4. Generation of the logical READY event.
5. Transfer of the resulting tile through SPI.
6. ACK event and transition of the worker to the idle state.

SPI transfers are serialized: only one transfer can use the shared resource at a time. Computation on different workers can overlap with each other and with SPI transfers.

The input transfer time is estimated using two matrix panels of size \(N \times b\), where \(N\) is the matrix dimension and \(b\) is the tile dimension. The output transfer corresponds to a \(b \times b\) tile. Transfer times include fixed overheads.

Worker computation time is estimated from the number of arithmetic operations and an assigned speed factor. The current worker speed factors are `0.92`, `1.00` and `1.07`.

### Worker Assignment Policies

- **Round Robin:** assigns tasks to available workers in cyclic order.
- **Predicted:** selects the available worker with the smallest estimated completion time. The estimate accounts for computation time and the shared SPI transfer queue.
- **Least Loaded:** selects the worker using the estimated completion time and accumulated worker busy time.

Tasks are taken from the queue in their existing order. The selected policy determines which available worker receives the next task.

## Numerical Validation

The simulator performs actual floating-point matrix multiplication. Input matrices are initialized deterministically, and a sequential reference result is calculated separately.

After all output tiles have been computed, the resulting matrix is compared with the reference matrix using the relative Frobenius error:

\[
\varepsilon_F =
\frac{\|C_{\mathrm{sim}}-C_{\mathrm{ref}}\|_F}
{\|C_{\mathrm{ref}}\|_F}
\]

The result is considered valid if:

\[
\varepsilon_F \leq 10^{-6}
\]

Numerical validation is reported separately from the event indicating completion of result transfer.

## Monitoring and Experiment Results

The web interface provides:

- Experiment configuration and execution controls.
- Current job state and execution progress.
- Worker states, speed factors and completed tile counts.
- Task assignment and event history.
- Simulation time and worker utilization.
- Numerical validation results.
- Experiment history with configuration parameters, execution time, utilization and validation status.

The latest three experiment results are retained in memory. The event log can be exported as a text file.

## Current Limitations

- The application currently uses only the simulation backend. Physical SPI communication and GPIO READY/ACK signals are not implemented.
- Computation speed factors and SPI transfer overheads are model assumptions, not measurements of the target hardware.
- READY and ACK are represented as logical simulation events; electrical signal timing is not modeled.
- Packet framing, CRC, retransmission, sequence numbers and timeout recovery are not implemented.
- A worker can be disabled only when it is idle. Reassignment of tasks during execution is not supported.
- Experiment history is stored in memory and is not preserved after application restart.
- The HTTP server has no authentication or access control and is intended for a trusted development environment only.