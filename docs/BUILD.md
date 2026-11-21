# Building JamesOS / JamesOS-K 1.0b, build 3124

Use the JieLi Linux toolchain and the AC79 SDK revision pinned in `tools/build.py`. See `tools/get_toolchain.sh` for toolchain setup. Set `JIELI_TOOLCHAIN` to the toolchain directory and `AC79_SDK` to the SDK checkout; `--sdk` overrides the latter. macOS builds require Docker.

These are the distribution build commands recorded with the published build-3124 source archive.

## FM-1

```sh
python3 tools/build.py --pattern-device-candidate --app-opt Oz --build-number 3124 --diagnostics distribution --define RV2_LINE_STRUCT=1 --define BR_MODEL_MOD_EXPERIMENT=1 --define FM1_PATTERN_DELETE_EXPERIMENT=1 --define FM1_PATTERN_DELETE_PROTOCOL_EXPERIMENT=1 --define FM1_PATTERN_RAW_EXPORT_EXPERIMENT=1 --define BR_ADAPTIVE_POLY=1
```

## SMK-37 Pro

```sh
python3 tools/build.py --target smk37 --pattern-device-candidate --app-opt Oz --build-number 3124 --diagnostics distribution --define RV2_LINE_STRUCT=1 --define BR_MODEL_MOD_EXPERIMENT=1 --define FM1_PATTERN_DELETE_EXPERIMENT=1 --define FM1_PATTERN_DELETE_PROTOCOL_EXPERIMENT=1 --define BR_ADAPTIVE_POLY=1
```

The distribution configuration includes the HD interface and polyphony. Build output includes `build-settings.json`. Include the licences and notices in `docs/legal/` with distributions, including the HD font notice. The SDK and compiler are separate build inputs.

## Host tests

Host tests require Python and a native C/C++ compiler. Font regeneration checks also require fontTools; image previews use Pillow.

```sh
python3 tests/run_suite.py --config fm1
python3 tests/run_suite.py --config smk37
```

Run these sequentially: suites in the same checkout share generated files under `build/`. The suite compares runner statuses with the fixtures in `tests/baselines/`; a match is not a claim that every runner passed. Runners needing external captures or comparison trees require their documented command-line inputs. Use `python3 tests/run_suite.py --help` for selection and baseline options.

Building and running host tests do not install firmware. Use the public installer for device updates.
