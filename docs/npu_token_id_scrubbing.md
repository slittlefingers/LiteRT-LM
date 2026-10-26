# Opt-in NPU token-ID scrubbing

This fork makes its token-ID clearing behavior explicit and disabled by default.
The setting applies to the specialized `TF_LITE_AUX` NPU executor only.

| Setting | Processed token IDs | Reset / outgoing context |
| --- | --- | --- |
| Omitted or `false` | Original retention behavior | Original token-array behavior |
| `true` | Overwrite consumed nonnegative IDs with zero | Clear this executor's processed-token array before releasing/reusing it |

The opt-in path preserves vector length and position accounting, the frontier
and pending token needed for the next step, and negative multimodal sentinels
needed by masking. The setting gates all six live-scrub call sites (including
unconstrained `DecodeNonSpeculative`) and both `ScrubAll` call sites. It is fixed
when the engine is created, not toggled while a session is generating.

## Enable explicitly

Both C++ command-line binaries (`litert_lm_main` and `litert_lm_advanced_main`)
accept:

```sh
--backend=npu --enable_npu_token_id_scrubbing=true
```

Omit the second parameter or pass `false` for the original behavior. Other
backends reject `true`. NPU models taking the generic compiler-plugin path
also reject `true`, because that executor does not implement this clearing.

For a C++ caller, set the existing backend config before engine creation:

```cpp
auto& executor = engine_settings.GetMutableMainExecutorSettings();
auto config = executor.MutableBackendConfig<litert::lm::NpuConfig>();
if (!config.ok()) return config.status();
config->enable_token_id_scrubbing = true;
executor.SetBackendConfig(*config);
```

For a C caller:

```c
LiteRtLmEngineSettings* settings =
    litert_lm_engine_settings_create(model_path, "npu", "cpu", "cpu");
if (!settings) { /* handle error */ }
if (litert_lm_engine_settings_set_npu_token_id_scrubbing(settings, true) != 0) {
  /* handle error; do not silently proceed */
}
LiteRtLmEngine* engine = litert_lm_engine_create(settings);
litert_lm_engine_settings_delete(settings);
```

The C setter returns `-1` for null settings or a non-NPU config. This API is a
fork extension. New aggregate configuration fields are appended so existing
positional initializers keep their previous meaning.

The Ghost APK explicitly passes `true`, preserving its current privacy behavior.
Its native library default remains `false`, allowing reviewers to compare both
modes without rebuilding the runtime. The APK's Debug switch only controls
inspection UI; it does not change the scrubbing policy.

## Scope and validation

This clears the processed-token array covered by the existing fork changes.
It is not a whole-process memory-erasure guarantee: pending TokenData objects,
returned token vectors, tokenizer intermediates, logs, allocator reuse, and
other copies are outside that guarantee. Spare capacity is not continuously
initialized or cleared by the live scrub. Existing KV behavior is unchanged;
retained KV continues to carry conversation context.

Features or callers requiring original historical token IDs cannot recover
them after clearing; keep the option disabled when that history is required.
The A/B device test covers two-turn greedy text generation and an active KV
slot, not every model or decoding mode.

Tests:

- `LlmExecutorConfigTest.NpuConfigDefaults`: default off and compatibility with
  pre-existing aggregate initializer arguments.
- `EngineCTest.NpuTokenIdScrubbing*`: C defaults, enable/disable, invalid input.
- `//runtime/engine:npu_token_id_scrubbing_test`: CLI settings propagation and
  rejection of the wrong backend.
- `TokenIdScrubbingRejectsGenericNpuPath`: reject unsupported NPU executor.
- `ProcessedTokensTest.Scrub*`: positions, frontier, pending ID, negative
  sentinels, and reset state.
- Ghost `TokenScrubbingFlagDeviceTest`: same two prompts under false/true,
  consumed-ID retention/clearing, identical greedy replies and active KV slot,
  and original-versus-cleared allocation behavior after session reset.
