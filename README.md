# Accelerator_FYP

## Overview

**Accelerator_FYP** contains the source code and FPGA accelerator designs for my Final Year Project, **“Tensor Computation Hardware Design and Applications to Finance.”**

The project applies tensor-computation hardware-design techniques to quantitative-finance workloads using Vitis HLS. It includes parameterized accelerator designs with a configurable asset count (`N`), targeting low-latency financial signal processing, risk computation, and decision generation.


The repository currently includes two design variants:

- `design_n_32/` — accelerator configuration for 32 assets
- `design_n128/` — accelerator configuration for 128 assets



## Repository Structure

```text
Accelerator_FYP/
├── design_n_32/          # 32-asset accelerator design
├── design_n128/          # 128-asset accelerator design
├── LICENSE
├── Thesis/               #Contains Block design figures and other figures from the FYP
├── README.md

```

Each design directory contains:

```text
design_n128/
├── Source/               # HLS C++ sources, headers, XO files, and build scripts
├── host/                 # Host-side application code
├── bin/                  # Build artifacts and executables
├── reports/              # Synthesis, implementation, and profiling reports
├── results/              # Simulation, validation, or performance results
└── run.sh                # Design-specific execution script
```

## Key Components

| Module | Purpose |
|---|---|
| `lob_filter` | Filters and processes limit-order-book data |
| `upd_inject` | Injects market-data updates into the processing pipeline |
| `sv_bcast` | Broadcasts singular-vector or shared-state data across the design |
| `regime_classifier` | Classifies market regimes |
| `risk_matrix` | Computes risk-related matrix operations |
| `momentum_signal` | Generates momentum-based trading signals |
| `decision_sink` | Produces the final trading or decision output |

## Repository Structure

```text
Accelerator_FYP/
├── design_n_32/          # 32-asset accelerator design
├── design_n128/          # 128-asset accelerator design
├── LICENSE
└── README.md
```

Each design directory follows a similar structure:

```text
design_n128/
├── Source/               # HLS C++ sources, headers, XO files, and build scripts
├── host/                 # Host-side application code
├── bin/                  # Build artifacts and executables
├── reports/              # Synthesis, implementation, and profiling reports
├── results/              # Simulation, validation, or performance results
└── run.sh                # Design-specific execution script
```

## Key Components

| Module | Purpose |
|---|---|
| `lob_filter` | Filters and processes limit-order-book data |
| `upd_inject` | Injects market-data updates into the processing pipeline |
| `sv_bcast` | Broadcasts singular-vector or shared-state data across the design |
| `regime_classifier` | Classifies market regimes |
| `risk_matrix` | Computes risk-related matrix operations |
| `momentum_signal` | Generates momentum-based trading signals |
| `decision_sink` | Produces the final trading or decision output |
| `inctsvd_top` | Top-level accelerator integration module |
| `quant_apps` | Host-side quantitative application logic |
| `tb_equiv` | Equivalence testbench for validating hardware behavior |

## Requirements

- Xilinx Vivado / Vitis HLS
- Vitis unified software environment
- Supported Xilinx FPGA platform
- C++ compiler for host-side development
- Bash-compatible shell

## Build and Run

Navigate to the desired design directory:

```bash
cd design_n128
```

Build the HLS design:

```bash
cd Source
./build_all.sh
```

Run the accelerator workflow:

```bash
cd ..
./run.sh
```

For the 32-asset configuration:

```bash
cd design_n_32
./run.sh
```

## Configuration

The design supports different asset-count configurations:

| Directory | Asset count |
|---|---:|
| `design_n_32` | 32 |
| `design_n128` | 128 |

The `set_nasset.sh` script can be used to configure the asset-count parameter during the build process.

## Design Goals

- Accelerate quantitative-finance computations using FPGA hardware
- Support scalable asset-count configurations
- Maintain a modular HLS-based accelerator pipeline
- Enable low-latency processing of market-data streams
- Provide reproducible build, simulation, and execution workflows

## License

This project is licensed under the terms of the [LICENSE](LICENSE) file.
