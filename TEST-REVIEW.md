# Jam2 Manual Review Register

This file contains only decisions that still require genuine product or design
review. Approved, implemented, or explicitly retained items are recorded in
`TEST-LOG.md` and are removed from this register.

## REVIEW-008 - flexible Section length, recording duration, and memory ownership

Jam2 currently has two overlapping limits that do not describe one consistent
product contract:

- A Section is capped at 512 beats. Its dense normalized model owns chords,
  targets, notes, lyrics, beat patterns, and musical-step containers for each
  represented beat.
- Loopback recording and prepared rendering are capped at five minutes. At
  high BPM or with compound pulse units, five minutes can require many more
  than 512 beats; at low BPM, 512 beats can describe much more than five
  minutes.
- A possible replacement is a visible limit of 512 bars, with each bar bounded
  by the supported meter and no separate arbitrary total-beat cap. This would
  make meter the natural per-bar bound, but it still does not guarantee that
  every five-minute recording fits: at 400 BPM, five minutes contains 2,000
  quarter-note beats, or up to 6,000 temporal pulses when
  `tempo_pulse_units == 3`. Depending on meter, that can exceed 512 bars.

The current Section allocation is demand-driven, not reserved at the maximum:
the default project allocates only its current Sections and beats, and growth
or load normalizes the newly represented beats. Pagination limits visible
widgets but does not make the underlying model sparse. A rough current dense
cost is about 0.8--1.1 KiB per represented beat before heap allocations made by
nonempty strings and step data. Therefore a completely expanded 512-bar
Section would be on the order of roughly 0.4--9 MiB for common 1--16 beat
meters, per Section, before populated content and allocator overhead. Multiple
expanded Sections multiply that cost. Shrinking destroys nested content but a
Qt vector may retain some outer capacity for reuse.

Audio has a different cost profile. PCM buffers dominate memory: five minutes
of mono float audio at 48 kHz is about 55 MiB, and the prepared-source engine
currently reserves multiple slots, so extending fixed resident buffers to the
full theoretical Section domain would be substantially more expensive than
expanding beat metadata. This is the main reason not to solve the mismatch by
simply increasing every fixed buffer.

Continued review should produce one clear, flexible contract covering:

- whether Section length is bounded by bars, musical beats, elapsed duration,
  or a documented combination;
- how a five-minute loopback take expands/matches a Section at every supported
  BPM, meter, and pulse-unit mode, including what the user sees if it cannot;
- whether Section metadata should remain dense, become sparse/lazy, or use a
  hybrid representation so unused cells do not allocate per-beat containers;
- whether prepared playback should stream/chunk audio, page a bounded cache,
  or reject over-limit renders explicitly instead of silently truncating;
- total serialized/synchronized size bounds for four-peer sessions, including
  malicious or accidentally enormous but structurally valid projects;
- predictable reclamation and diagnostics: current/reserved metadata bytes,
  resident audio bytes, render-cache depth, and any streaming underrun/error.

No limit or renderer change should be implemented until those choices are
reviewed together. The desired result is not merely a larger numeric cap: it is
one understandable duration model whose memory is allocated only when useful,
whose audio working set stays bounded, and whose failure behavior is explicit.
