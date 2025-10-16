# Overlay Scheduler Integration Checklist

This checklist summarizes the follow-up work required to exercise the
novelty-based candidate ordering that now ships with AFLNet.  It follows the
original proposal step-by-step so you can verify that the implementation is
behaving as intended and understand where to extend it next.

## 1. Prepare runtime dependencies

1. Install Graphviz headers (`libgraphviz-dev` or an equivalent package) so that
   `afl-fuzz` links successfully:

   ```bash
   sudo apt-get install -y libgraphviz-dev
   make -j$(nproc)
   ```

   The new overlay code is already included in the default build; compiling is
   enough to pick up the changes.

2. Ensure your input corpus still contains the IPSM traces required for
   clustering.  The implementation consumes the `region_t.state_sequence`
   buffers recorded by AFLNet during replay, so runs without IPSM data will
   fall back to single-cluster behavior.

## 2. Validate feature extraction

1. Launch AFLNet with `AFL_DEBUG_OVERLAY=1` (see `overlay_sched.c`) to emit
   debug messages showing how many messages and states each seed contributes.
   This allows you to confirm that message boundaries and histograms are being
   reconstructed correctly from the saved seed files.

2. If message boundaries appear incorrect, re-check the instrumentation that
   writes `region_t` entries (see `aflnet.c`).  The overlay scheduler trusts
   those offsets verbatim when slicing message histograms.

## 3. Inspect clustering output

1. Collect the scheduler statistics printed under `AFL_STAT_OVERLAY=1`.  The
   novelty scores correspond to `1 - avg_sim_all` in the proposal, so higher
   values indicate more novel seeds.

2. When you observe an unexpected single-cluster run, dump the cached
   `signature` values (via `AFL_DEBUG_OVERLAY`) to confirm whether the IPSM
   shingle hash is constant.  Identical state sequences are expected to land in
   the same cluster.

## 4. Exercise the round-robin selector

1. Run a fuzzing session long enough to cycle through multiple clusters.
   Inspect the debug log to verify that the scheduler alternates between
   clusters (`overlay_rr_pos` keeps the rotation state).

2. To stress-test fairness, build a synthetic corpus where one cluster contains
   many seeds with similar messages and another contains just one unique seed.
   The round-robin policy should keep advancing both clusters instead of
   starving the singleton.

## 5. Next extensions

* If you plan to experiment with alternative similarity measures, swap out
  `overlay_seq_similarity` with the desired routine.  The rest of the pipeline
  (clustering + round-robin) can remain untouched.
* To persist novelty scores across restarts, serialize `queue_entry->novelty`
  alongside the other metadata when saving the queue state.  The current code
  rebuilds the cache on demand.
* The stub `overlay_queue_pick_from_queue_window()` already exposes a queue
  window helper; wire it into custom scheduling strategies if you need to reuse
  the novelty ordering outside of `pick_next_entry()`.

Following these steps should help you confirm that the implementation matches
the plan and highlight the exact touch points for further experimentation.
