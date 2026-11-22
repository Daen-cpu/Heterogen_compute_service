# Architecture decisions

## ADR-001: Backend-independent execution interface

The controller depends on `IExecutionBackend`, not on simulation or hardware implementation details. This permits replacing the simulator with an i.MX91 hardware adapter without changing the UI contract.

## ADR-002: One schedulable unit is an output tile

A task represents one resultant tile `C_ij`. An executor receives the required A/B panels, accumulates the tile locally and returns the completed tile. Scheduling individual scalar products would create excessive control traffic.

## ADR-003: SPI is a single serialized resource

Input and result transfers share one SPI resource. The simulator serializes those intervals using `spi_free_at`. Worker compute intervals are independent and may overlap with each other and with SPI activity.

## ADR-004: READY/ACK lifecycle

The logical worker lifecycle is input transfer -> compute -> READY -> result transfer -> validation -> ACK -> IDLE. The executor is not considered available until ACK is issued.

## ADR-005: Simulation first, hardware second

The first milestone validates control-plane behavior and experiment workflows using a discrete-event model. The simulator's parameters must be calibrated before its timing output is treated as a performance prediction.
