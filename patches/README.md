# patches/

Patches applied to vendored submodules at CMake configure time. The top-level
`CMakeLists.txt` greps each patched file for a sentinel string and calls
`git apply` only when the sentinel is absent — safe against re-runs and
against `git submodule update --init --recursive` wiping the submodule's
working tree.

## clap-wrapper-midi-event-bridge.patch

Target: `clap-wrapper/src/detail/vst3/process.cpp`
Pinned SHA: `62bf4f193d8dac354a946c1e0cffda9174317042`

Upstream `clap-wrapper` does not yet translate incoming VST3
`kLegacyMIDICCOutEvent` events back into `CLAP_EVENT_MIDI` for the hosted
CLAP plugin. This patch adds that input-side translation inside
`processInputEvents`, covering CC (0xB0), Program Change (0xC0), Channel
Pressure (0xD0), Polyphonic Key Pressure (0xA0) and Pitch Bend (0xE0).

Without it, `poryaaaa` would silently ignore CC/PC/pitch-bend events
forwarded by an upstream VST3 plugin (e.g. `ccomidi.vst3`) through the
DAW's plugin-to-plugin event routing.

Sentinel: `kLegacyMIDICCOutEvent` — absent in the pristine `62bf4f1`
`process.cpp`, present once this patch is applied.

## Upstreaming

The plan is to open a PR against `free-audio/clap-wrapper` with the same
translation; if it lands we bump the submodule SHA and delete this
directory.
