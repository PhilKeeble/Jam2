# Latency stability regression — 2026-11-03

Introducing commit **unproven**; rollback tests also failed. Oct 27's good-run
commit was inferred, not verified. Current fixes are uncommitted/provisional.

## Inputs / directions

- Good Oct 23: `release/logs/jam2_stats_20261023_120503_496_pid33228.csv`
  and `jam2_gui_20261023_110410_830_pid33228.log`.
- Good Oct 27 LAN: `release/logs/jam2_stats_20261027_181624_332_pid68908.csv`.
- Bad Oct 30 Robbie: `C:\Users\Phil\Documents\Jam2-windows\logs`;
  `*20261030*pid24448*` = Windows joiner, `*pid7609*` = Mac creator.
- Bad LAN: `release/logs/jam2_stats_20261029_151420_795_pid37968.csv`.
- Paired captures: `captures/`, `tools/incoming_uploads/`, `release/logs/`;
  [packet review](artifacts/packet-regression-20261102/packet-review.json).
- New evidence/patches: `artifacts/regression-fixes-20261103/`;
  [results review](artifacts/regression-fixes-20261103/results-review.json),
  [test profiles / commands](tests/profiles/README.md).

## Findings / latest experiment

- 64-frame 4/8 ms batching reproduces repeated underruns and valid-audio trimming.
  Fix experiments cover short-gap learning, release/target decay and interpolation
  runway. The original mixer/stream logic was unchanged from pre-UDP `21a45d3`.
- Delayed beacons independently cause ~427 ms mapping error. Candidate filters
  delayed/reordered **unwrapped** observations, preserves epochs and exposes CSV
  diagnostics. Latest delayed-beacon errors remain below **2.59 ms**.
- Baseline: `artifacts/asio-delivery/20261103-095809-018-two-asio`.
  Latest J: `artifacts/asio-delivery/20261103-113622-699-two-asio`.
  Real **TONEX creator / Focusrite joiner, 44.1 kHz / 32-frame ASIO, PCM24**.
- All four clean cases pass unchanged gates: no added steady underruns/trimming
  or tone discontinuities. Callback audio round trips (64/128 packets):
  **7.91 / 10.11 ms**, baseline **7.39 / 12.52 ms**. The +0.52 ms difference is
  within the declared callback tolerance; driver output/converters are separate.
- 8 ms batching: Focusrite underruns/trimming improve from **56.52 / 598.04**
  to **2.84 / 4.83 ms/min**. Recurring holds and the Oct 30 envelope recover.
- **Full J matrix: 9/10 pass.** At 300 BPM, 16 TONEX / 10 Focusrite underrun
  frames occur in seconds 45..63 after extra delay ends; mapping remains bounded.
  Identical isolated repeat passes: `20261103-114802-999-two-asio`.
  **Do not call the intermittent issue fixed or the candidate fully accepted.**
- Twenty focused native checks pass, including four peers, 44.1 kHz cadence,
  bounded jitter, ordering/loss and hour-long clock simulations. One clean runtime
  check failed zero-padding with a 24 ms proxy-pump stall, then passed isolated;
  both logs retained, causation unproven.

Next: capture callback/worker/buffer state at the intermittent underrun and add
irregular-wakeup coverage. Keep clean latency/quality gates unchanged.
Nov 3 manual Direct/Jam2-server GUI review: [results](artifacts/manual-local-review-20261103/review.json).
Server playback queues are ~1.3–1.4 ms higher, with 64/63 recovery-padding
frames retained while targets return to 64; RTT remains ~0.04 ms. No loss,
trimming or recovery loops; small underruns remain. Add startup/rejoin and
padding-retirement coverage for the persistent GUI engine.
Native startup regression retains 64 frames in PCM16/24; an archived worker
state probe explains the blocked release but does not prove safe headroom. Proposed fixes were **rejected**:
real-ASIO mode comparisons failed; more aggressive release introduced clock-skew
underruns. Playback changes from this experiment were reverted; tests/harness
and [evidence](artifacts/startup-recovery-fix-20261103/review.json) remain.
No-growth check: `jam2_audio_flow_repeated-recovery` passes 18 deterministic
four-participant cases, thirty identical 1.6/4 ms pauses over 496 seconds each.
Actual settled depth stays within one frame of the first recovery despite growing
cumulative padding; no loss/trimming/drops. Retention still need not clear.
[Evidence](artifacts/repeated-recovery-20261103/review.json); production unchanged.
Larger single-pause baseline: `jam2_audio_flow_large-recovery` fails 24/30 comparisons.
4..20 ms pauses retain ~128 samples / 2.9 ms above clean for ~8 minutes. Target
returns to 64, but touching the 128-frame surplus floor resets release's 1 s
stability timer. This is the baseline for the following candidate.
[Evidence](artifacts/large-recovery-20261103/review.json).
Recovery-accounting candidate: larger-pause comparisons now pass 30/30, with
~66 samples / 1.5 ms retained instead of ~128 / 2.9 ms; repeated pauses stay
bounded. Uses measured extra source consumption and recent low-water headroom.
Strict startup retirement still fails (~34 extra samples vs equal-underrun
control); other pre-existing native failures remain.
[Current experiment](artifacts/recovery-accounting-fix-20261103/review.json).
Real ASIO Direct/server follow-up: 8 fresh pre-fix + 8 candidate cases completed.
Native paired checks flag 7/8; one is an exact-32-frame CSV-rounding boundary,
six have other buffer/quality differences. New release path inactive in 14/16
device-runs; both mode matrices already vary. Candidate is still installed;
**user decides keep/revert**, no automatic decision.
[Hardware results](artifacts/asio-mode-review-20261103/review.json).
Nov 2 Wi-Fi receive pauses do **not** establish Robbie's Ethernet/WAN cause;
the short [transport A/B](artifacts/udp-transport-comparison-20261102/summary.json)
passed without devices/mixer/full runtime. Wired captures are follow-up, not a
prerequisite; user cannot arrange that run alone. Oct 30's ~200 ms GUI estimate
can follow RTT/2; ~417 ms mapping error is not an OS clock jump:
[GUI review](artifacts/gui-latency-review-20261103.json).
