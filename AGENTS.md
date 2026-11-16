# AGENTS.md — SOEM EtherCAT IO Master

## Build

- Visual Studio 2022 solution (`soem-IO.slnx`), C++20, toolset v145.
- Build configurations: Debug/Release × Win32/x64.
- Links `soem.lib`, `wpcap.lib`, `Packet.lib`, `winmm.lib`, `ws2_32.lib` (all from `lib/`).
- Additional include dirs point outside the repo to the sibling `SOEM-master-IO\soem\inc;lib`.

## Entry points

| File | Role |
|------|------|
| `src/simple_ng.c` | **Active** `main()`. Minimal EtherCAT master: init → PDO exchange loop → stop. |
| `src/main.c` | Slaveinfo diagnostic. The `slaveinfo()` function is compiled but its `main()` is **commented out**. Both files compile into the same binary. |

The project produces **`soem_io.exe`** (a single exe regardless of which `main` is active).

## Runtime

- Requires **WinPcap / Npcap** for raw Ethernet access. The NIC name is hardcoded in `simple_ng.c:352`:
  `\Device\NPF_{5E61BDDD-69A6-4213-8C1B-21B894582CF5}`
- Update this GUID to match the target network adapter.
- The slaveinfo in `main.c` shows available adapters via `ec_find_adapters()` when run without arguments.

## Key architecture

- `Fieldbus` struct (in `simple_ng.c`) wraps `ecx_contextt` + process data map.
- Data exchange: `ecx_send_processdata()` → `ecx_receive_processdata()`.
- DC sync enabled via `ecx_dcsync0()` after reaching OP state.
- 4096-byte IOmap for PDO data.

## Project conventions

- C (not C++) source, compiled as C++20 in MSVC.
- Comments use Doxygen `\brief` style and Chinese descriptions.
- `_CRT_SECURE_NO_WARNINGS` defined for x64 Debug.
- License: GPLv3 or commercial (see `LICENSE.md` — not present in this repo copy).
- No test framework, no CI config, no `.gitignore`.
