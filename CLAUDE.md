# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Working Norms

- **Keep this file updated** as changes are made to the codebase.
- **Never guess or make assumptions** — ask clarifying questions when requirements are unclear.
- **All code changes should include unit tests.** When possible, write tests first and implement code to make them pass (TDD).
- **After writing code, review comments** and remove any that don't explain non-obvious behavior — don't comment what the code already says.

## Development Environment

The repo is set up for development in VS Code using a devcontainer (`.devcontainer/`). When working inside the devcontainer, all compile, test, and run commands must be executed inside the container.

Large system test cases for manual runs can be kept outside the repo in `../RTLSDR-Airband_test_cases/` (a sibling of the checkout, so several checkouts can share one copy). The devcontainer mounts it read-only at `system_tests/test_cases/` (gitignored); the pytest suite does not read it. `initializeCommand` creates an empty folder on the host if it is missing, so the container starts without it. This needs a POSIX host shell (macOS, Linux, WSL) and a container rebuild to take effect.

## Wiki Documentation

User-facing documentation lives in a separate repo: https://github.com/rtl-airband/RTLSDR-Airband/wiki

Flag when code changes require wiki updates and provide suggested content — do not edit the wiki directly.

## Code Review Guidelines

When reviewing code:
- Reference specific files and line numbers (`src/foo.cpp:42`)
- Start with architecture-level concerns before line-level feedback
- Consider SDR/DSP domain context (signal processing constraints, real-time threading, buffer management)
- Verify the testing approach covers the behavior being changed
- Structure feedback clearly: separate blocking issues from suggestions
- Be pragmatic — prefer working correct code over theoretical perfection
- Check for consistency with surrounding code style and conventions

## Project Overview

RTLSDR-Airband is a C++ SDR (Software-Defined Radio) application that receives analog radio voice channels from SDR devices (RTL-SDR, SoapySDR, MiriSDR) and produces MP3 audio streams for Icecast, file recording, UDP, and PulseAudio.

## Build Commands

Dependencies: libconfig++, libmp3lame, libshout, libfftw3f, librtlsdr, libsoapysdr, libpulse. Install via `.github/install_dependencies`.

```bash
# Standard debug build with unit tests
cmake -B builds/Debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_UNITTESTS=TRUE
cmake --build builds/Debug -j4

# Release build with NFM and SoapySDR
cmake -B builds/Release -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DSOAPYSDR=ON
cmake --build builds/Release -j4

# Run unit tests
./builds/Debug/src/unittests
./builds/Release/src/unittests

# Run the binary
./builds/Debug/src/rtl_airband -c /path/to/config.conf
./builds/Release/src/rtl_airband -c /path/to/config.conf
```

Key CMake flags (all in `src/CMakeLists.txt`):

| Flag | Default | Purpose |
|------|---------|---------|
| `NFM` | OFF | Enable Narrow FM demodulation |
| `PLATFORM` | `native` | Optimization target: `native`, `generic`, `rpiv2` |
| `RTLSDR` | ON | RTL-SDR driver |
| `MIRISDR` | ON | Mirics SDR driver |
| `SOAPYSDR` | ON | SoapySDR (vendor-neutral) driver |
| `PULSEAUDIO` | ON | PulseAudio output |
| `BUILD_UNITTESTS` | OFF | Build Google Test unit tests |
| `BCM_VC` | OFF | Broadcom VideoCore GPU FFT (RPi v2 only; requires `PLATFORM=rpiv2`, which also turns it on) |

## Docker

The container image is built from a multi-stage `Dockerfile` based on `alpine:latest` (musl libc). Alpine is used because it ships a native `linux/arm/v6` image, so a single base covers every Raspberry Pi model (Pi 1/Zero armv6 through Pi 5 arm64) — Debian dropped the armel/`arm/v5` variant that the arm/v6 build previously relied on via fallback.

- Build/runtime dependencies come from `apk`. Note the Alpine-specific package names: `libconfig++` (Alpine splits the C++ binding into its own package), `libstdc++` (not in the base image), `fftw-single-libs`, `libpulse`, and `pkgconf` (provides `pkg-config`).
- SoapySDR, the rtl-sdr-blog fork (built from source for RTL-SDR Blog V4 support), and libmirisdr-4 are compiled from source with CMake and installed to `/usr` — musl only searches `/lib` and `/usr/lib`, not `/usr/local/lib`. `ENV CMAKE_POLICY_VERSION_MINIMUM=3.5` lets their pre-3.5 `cmake_minimum_required` configure under Alpine's CMake 4.
- Both stages run `unittests`, so a broken build fails `docker build`. `scripts/find_version` is POSIX `sh` (Alpine has no bash).
- If the build context is assembled on a Windows checkout with `core.autocrlf=true` (files are CRLF on disk), re-normalize the shell scripts in `scripts/` to LF before `docker build` — CRLF breaks `find_version` at configure time with an empty `RTL_AIRBAND_VERSION`.

```bash
# Build the image for the host architecture
docker build -t rtlsdr-airband .

# Build a specific architecture (requires QEMU/binfmt for cross-arch)
docker buildx build --platform linux/arm/v6 --load -t rtlsdr-airband:armv6 .
```

## Code Formatting and Pre-commit

Uses clang-format v14 with Chromium style (indent=4, column limit=200, config in `.clang-format`).

```bash
# Install pre-commit hooks (once, after cloning)
pre-commit install

# Run all pre-commit hooks manually
pre-commit run --all-files

# Format C++ source files manually (also used by CI)
./scripts/reformat_code
```

Pre-commit hooks (`.pre-commit-config.yaml`) run on every commit and check:
- YAML/JSON validity, trailing whitespace, EOF newlines, shebang permissions, large files, merge conflict markers, private keys
- clang-format on all `src/*.cpp` and `src/*.h` files
- shellcheck on all bash scripts (excluding `init.d/`)
- black, isort, and pylint on all `system_tests/**/*.py` files
- Build (AM and NFM) and C++ unit tests when `src/*.cpp`, `src/*.h`, or `CMakeLists.txt` are modified (`scripts/run_unit_tests`)
- Python system tests when `src/*.cpp`, `src/*.h`, `CMakeLists.txt`, or `system_tests/` are modified (`scripts/run_system_tests`); only runs if the build/unit-test step passes

## CI and Pull Request Checks

Four workflows run checks on open pull requests (`.github/workflows/`); `platform_build.yml` skips fork PRs. All of them except `code_formatting.yml` also run on pushes to `main`, tags, `workflow_dispatch`, and a daily schedule; `code_formatting.yml` runs on PRs and daily. A fifth, `version_bump.yml`, runs after a PR is merged — see [Version Tagging](#version-tagging).

**`code_formatting.yml`** — runs `./scripts/reformat_code` and fails if any files differ.

**`ci_build.yml`** — builds and tests four configurations on Ubuntu (x86 and ARM) and macOS:
```bash
cmake -B builds/Debug          -DCMAKE_BUILD_TYPE=Debug   -DBUILD_UNITTESTS=TRUE
cmake -B builds/Debug_nfm      -DCMAKE_BUILD_TYPE=Debug   -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release        -DCMAKE_BUILD_TYPE=Release -DBUILD_UNITTESTS=TRUE
cmake -B builds/Release_nfm    -DCMAKE_BUILD_TYPE=Release -DNFM=TRUE -DBUILD_UNITTESTS=TRUE
```
Then runs `unittests` for all four, runs the system tests (`--mode thorough`) against the Release and Release+NFM builds, installs the Release+NFM build, and smoke-tests `rtl_airband -v`.

**`platform_build.yml`** — all self-hosted hardware. A single `airband-proxy` runner rsyncs the checkout to each Pi over SSH and runs build + unit tests + system tests there, so the Pis need no runner agent (32-bit ARM has no Node 24 after the Node 20 EOL):

| Target | Arch | `PLATFORM` | `BCM_VC` | `--sudo` |
|--------|------|------------|----------|----------|
| `airband-4b` | 64-bit ARM | `native` | OFF | no |
| `airband-3b` | 32-bit ARM | `rpiv2` | ON | yes |

The workflow passes `--sudo` exactly when `BCM_VC=ON` (the VideoCore GPU FFT needs root). Each target's work dir is `~/rtlsdr-airband-ci` in the CI account's home (not a tmpfs, which is too small for the build tree and uv venv), reused across runs so builds stay warm; this relies on there being a single `airband-proxy` runner, which runs one job at a time.

The proxy runner runs as user `airband-proxy` (`700` home, no sudo) and holds the targets' SSH key. On each Pi, CI logs in as `airband-build`, which has no password and either no sudo or, on BCM targets, a sudo rule for only the per-run `rtl_airband` binary. Build deps are **pre-installed** on the Pis, so the workflow never runs `install_dependencies` there. **Re-provision the Pis when `.github/install_dependencies` changes** — optional drivers (MiriSDR, SoapySDR, PulseAudio) are auto-detected, so a stale target may silently build without them rather than fail.

Provisioning:
1. As `airband-proxy`, run `.github/setup_orchestrator_ssh` (creates the key, pins host keys, and writes the managed `~/.ssh/config.d/airband-ci`, prepending an `Include` for it to `~/.ssh/config`; don't hand-edit the managed file, it is regenerated on every run) and copy the `sudo PROXY_PUBKEY=… PROXY_FROM=… .github/setup_remote_test_target` command it prints for each target.
2. Run that command on each Pi (installs deps, the `airband-build` login with the `from=`-restricted key, a `/test_data` tmpfs, and uv). On `BCM_VC` targets (`airband-3b`) add `RTL_SUDO=1` after `sudo` (`sudo RTL_SUDO=1 PROXY_PUBKEY=…`, since `sudo` drops variables set before it) to grant the `rtl_airband` sudo rule; without it the account gets no sudo.
3. Re-run `.github/setup_orchestrator_ssh` to verify access.

Self-hosted runner security (public repo): `platform_build` is the only self-hosted job and is gated with `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository` so **fork** PRs never execute on the hardware — keep this guard on any self-hosted job. This complements the repo setting requiring approval for outside collaborators, network-segmented runners, and keeping repo secrets out of these workflows.

**`build_docker_containers.yml`** — builds the multi-arch container image (`linux/amd64`, `386`, `arm64`, `arm/v6`, `arm/v7`), one job per platform via QEMU. Every image is smoke-tested (`rtl_airband -v`) under its target platform. On push/tag/schedule/`workflow_dispatch` the images are pushed by digest to GitHub Container Registry and the `merge` job combines the per-arch digests into a single manifest. Pull requests only build and smoke-test locally (`--load`); a fork PR gets a read-only `GITHUB_TOKEN`, so the registry push is denied. The `Prepare` step's `push` output gates this.

**Before submitting a PR**, the pre-commit hooks cover most checks automatically. For build system or config changes not touching `src/`, verify all four cmake configurations build cleanly by hand.

## Version Tagging

`version_bump.yml` runs when a PR is **merged into `main`** and tags the merge commit with [`anothrNick/github-tag-action@1.64.0`](https://github.com/anothrNick/github-tag-action). Configured with `WITH_V: true` (tags look like `v5.3.0`) and `DEFAULT_BUMP: patch`.

The action scans the **full commit message body of every commit between the previous tag and the merge commit** (`git log "$tag_commit".."$commit" --format=%B`) for a bump keyword, so the keyword can live in any commit in the PR — it does not have to be in the merge commit. "Previous tag" means the highest semver tag, not the most recent one by date:

| Keyword in a commit message | Result |
|------|--------|
| `#major` | `v5.3.0` → `v6.0.0` |
| `#minor` | `v5.3.0` → `v5.4.0` |
| `#patch` | `v5.3.0` → `v5.3.1` |
| `#none` | no tag is created |
| none of the above | `DEFAULT_BUMP: patch` applies → `v5.3.1` |

Rules when writing commit messages:

- **Every merged PR creates a tag** unless a commit says `#none`. With `DEFAULT_BUMP: patch`, doing nothing still bumps the patch version, so add a keyword only to ask for something other than a patch.
- **Add `#minor` for a new user-facing feature or a new config option.** Add `#major` for a breaking change — a removed or renamed config key, or changed default behavior.
- **Highest keyword wins**, checked in the order `#major` → `#minor` → `#patch` → `#none`. One `#major` anywhere in the PR's commits bumps major even if other commits say `#minor`.
- **Matching is a plain substring search over the whole message**, body included. Never write these tokens in prose (for example "fixed a #minor issue") — it will bump the version. Refer to them as "the #minor keyword" only outside commit messages.
- **`#minor` and `#major` also publish a GitHub Release** with generated notes; `#patch` and the default bump only create the tag (`if: steps.tag.outputs.part != 'patch'`).
- Squash-merging collapses the PR's commits into one message, so make sure the keyword survives into the squash message.

After tagging, the workflow re-runs `ci_build.yml`, `platform_build.yml`, and `build_docker_containers.yml` against the new tag (a tag pushed with `GITHUB_TOKEN` does not fire their own `tags: ['v*']` triggers, so they have to be dispatched explicitly).

Note that those three dispatch steps are unguarded: on a `#none` merge the action leaves `new_tag` at the **existing** tag, so they re-run against the previous release and republish its container images.

## System Tests

End-to-end tests live in `system_tests/`. They run the actual binary against generated IQ files and validate the audio output (MP3 duration, rawfile size). Managed with [uv](https://docs.astral.sh/uv/).

```bash
# Run system tests (requires Release binaries — run scripts/run_unit_tests first)
scripts/run_system_tests

# Run manually from the system_tests directory
cd system_tests
uv sync
uv run pytest tests/ \
    --binary ../builds/Release/src/rtl_airband \
    --nfm-binary ../builds/Release_nfm/src/rtl_airband \
    -v
```

Python tooling (formatter, import sorter, linter) is configured in `system_tests/pyproject.toml` under `[tool.black]`, `[tool.isort]`, and `[tool.pylint]`. Run them manually:

```bash
cd system_tests
uv run black .
uv run isort .
uv run pylint conftest.py helpers/ tests/
```

## Architecture

### Reception Pipeline

```
SDR device (input-*.cpp)
  → RX thread → circular sample buffer
  → demod thread: FFT (FFTW3) → demod (AM/NFM) → filter → CTCSS → squelch → AGC
  → channel output handlers
  → output thread: MP3 encode (lame) → Icecast / file / UDP / PulseAudio
```

### Key Source Files

| File | Purpose |
|------|---------|
| `src/rtl_airband.cpp` | Main entry point, demod loop, thread management |
| `src/rtl_airband.h` | All major struct/enum definitions (`device_t`, `channel_t`, `mixer_t`, `output_t`) |
| `src/config.cpp` | libconfig++ parsing for devices, channels, mixers, outputs |
| `src/output-common.cpp/h` | Common output: per-type dispatch (`process_outputs`/`disable_channel_outputs`), output threads, stats file |
| `src/output-icecast.cpp/h` | Icecast output: libshout connection, MP3 encode + send, reconnect |
| `src/output-file.cpp/h` | File output (mp3/raw) lifecycle: filename/timestamp, open/append (discontinuity tones `discontinuity_tone`, default on), per-batch write, close policy (hour/split), lametag flush. In both hourly and split modes (continuous excepted) each activity's audio is buffered in `file_data::audio_buf` until it outlives `split_min_file_time` (short bursts never create a file); the file is then created — named after the activity start in split mode, after the creation hour in hourly mode — and the buffer flushed into it. Split files rotate after `split_max_file_time` (from file creation) and close on `split_max_idle_time` of silence; hourly files close only at the hour boundary. One trailing silence batch is written after an activity ends (buffer dropped if no file was created). Slot retune/release (`close_channel_file_outputs`) drops the buffered activity. |
| `src/output-udp.cpp/h` | UDP stream output (raw 32-bit float audio) |
| `src/output-pulse.cpp/h` | PulseAudio output (optional, `PULSEAUDIO` CMake flag) |
| `src/mixer.cpp` | Multi-channel mixer with ampfactor/balance |
| `src/input-*.cpp` | SDR device drivers (rtlsdr, soapysdr, mirisdr, file) |
| `src/input-common.cpp/h` | Input device abstraction (`input_t` function-pointer interface) |
| `src/filters.cpp/h` | IIR lowpass and notch filters |
| `src/squelch.cpp/h` | Noise-power-based voice activity detection |
| `src/wideband_scan.cpp/h` | Wideband range scan: carrier detection grid, debouncing, active-carrier slot assignment |
| `src/ctcss.cpp/h` | CTCSS tone detection |

### Device Modes

Each device operates in one of three modes, set via `mode = "multichannel"` (default), `mode = "scan"`, or `mode = "wideband_scan"` in config.

**`R_MULTICHANNEL`** — The SDR is tuned to a fixed center frequency and multiple channels are demodulated simultaneously from the same wideband capture. Each channel has a single `freq` value that must fall within the SDR's bandwidth. This is the common case for monitoring several frequencies at once.

**`R_SCAN`** — The device has exactly one channel, but that channel holds a `freqs` list of frequencies to cycle through. A controller thread monitors the squelch: after ~2 seconds of no signal (10 × 200 ms polls), it retunes the SDR hardware to the next frequency via `input_set_centerfreq()`. When a signal is detected, it stays on the current frequency. Per-frequency settings (labels, `squelch_threshold`/`squelch_snr_threshold`, `modulations`, `notch`/`notch_q`, `ctcss`, `bandwidth`, `ampfactor`) are **parallel lists** to `freqs` (same index = same frequency), not a list of objects — `freqs` is parsed as numbers. Only one channel per device in scan mode. (`rtl_airband.cpp:101-140`, `config.cpp:312-729`)

**`R_WIDEBAND_SCAN`** — The device covers a continuous frequency range (`freq_from`/`freq_to`, `channel_step` in kHz, default 12.5) instead of fixed channels. On every FFT batch, each grid bin's power is compared against the 25th percentile of grid noise; a carrier above the threshold (`squelch_snr_threshold`, default 9.54 dB — used for detection as well) for `min_above` (3) consecutive batches is assigned to one of `max_active_carriers` (default 8) slots, and a slot is dropped after `max_missing` (10) batches below the threshold (`wideband_scan.cpp:86-87`). Frequencies listed in `freq_blacklist` are excluded from detection — each entry is rounded to the nearest grid point, which then never opens squelch nor gets a slot (out-of-range entries are ignored with a warning). Each slot is demodulated like a regular channel at its detected frequency, using device-level file outputs only (`include_freq` is forced for file outputs, since all slots share one filename template). Because a slot only exists while its carrier is present, the per-slot squelch noise floor is seeded from the grid noise estimate on slot assignment (`Squelch::set_noise_floor`) and tracked against the current grid noise each batch while the slot is held (`Squelch::track_noise_floor`); otherwise the floor would settle on the carrier level and the squelch would never open. In foreground TUI mode (`-f`) the grid's signal/noise levels are drawn as a rolling text scope. BCM VideoCore FFT is not supported. (`config.cpp:779-953`, `wideband_scan.cpp`, `rtl_airband.cpp:767-768`)

### Threading Model

- **RX thread** (1 per device, always) — reads SDR samples into the circular buffer (`input-common.cpp`)
- **Controller thread** (1 per device, `R_SCAN` mode only) — scanning/squelch state machine for devices that scan across frequencies; not created for `R_MULTICHANNEL` devices (`rtl_airband.cpp:1005-1013`)
- **Demod thread** (1 total by default; 1 per device if `multiple_demod_threads=true` in config) — FFT, demodulation, filter, CTCSS, squelch, AGC for all assigned devices (`rtl_airband.cpp:1044`)
- **Output thread** (1 total by default; 1 per device + 1 for mixers if `multiple_output_threads=true`) — MP3 encoding and streaming
- **Mixer thread** (1 total, only if any mixers are configured) — processes all mixers; not per-mixer (`rtl_airband.cpp:1091-1092`)

### Configuration Format

Config files use libconfig++ syntax. Sample configs in `config/`. Top-level sections:

```
devices: ( { type = "rtlsdr"; centerfreq = 120.0; gain = 25;
             channels: ( { freq = 119.5; modulation = "AM";
                           outputs: ( { type = "icecast"; ... } ); } ); } );
mixers: { mix1: { outputs: ( { type = "icecast"; ... } ); } };
```

Mixers have **no `inputs` section** — a channel connects to a mixer implicitly via an output `type = "mixer"` + `name` (+ optional `ampfactor`, `balance`), see `mixer_connect_input` (`src/config.cpp:168-193`, `src/mixer.cpp:57`).

`wideband_scan` devices have no `channels` — they take `freq_from`/`freq_to` and device-level `outputs` (file outputs only, `include_freq` is forced):

```
devices: ( { type = "rtlsdr"; mode = "wideband_scan"; freq_from = 172.0; freq_to = 173.5;
             channel_step = 12.5; max_active_carriers = 8; modulation = "NFM";
             squelch_snr_threshold = 9.54;
             freq_blacklist = ( 172.500, 172.600 );
             outputs: ( { type = "file"; directory = "/var/log/radio";
                          filename_template = "SCAN-mobile"; append = true;
                                                     include_freq = true; } ); } );
```

Output types: `icecast`, `file`, `rawfile`, `udp_stream`, `mixer`, `pulse`.

### Unit Tests

Tests use Google Test (fetched via CMake FetchContent). Test files in `src/test_*.cpp` cover filters, squelch, CTCSS, wideband scan, helper functions, and signal generation. `src/test_base_class.h` provides test utilities.
