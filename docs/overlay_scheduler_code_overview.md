# Overlay Scheduler Code Overview

This note summarizes the code that implements the novelty-based candidate ordering
layer that was added on top of AFLNet. It explains how session features are
collected, how candidates are grouped and ranked, and where the scheduler hooks
into the existing fuzzing loops.

## Shared queue entry metadata

`queue_entry_types.h` centralizes the definition of `struct queue_entry` so that
both `afl-fuzz.c` and the overlay module share the same fields. Besides the
standard AFL/AFLNet bookkeeping, two fields were added for the scheduler: a
floating-point `novelty_score` cache and a pointer to the lazily built
`sess_feat` bundle that stores per-seed histograms and state data.【F:queue_entry_types.h†L1-L48】

## Overlay scheduler module (`overlay_sched.c/.h`)

The header exposes helpers for queue lifecycle events, feature extraction, and
candidate ordering. The backing implementation maintains a sliding window over
queue entries (`OVERLAY_QUEUE_WINDOW`) along with state needed for round-robin
rotation across clusters, and now also provides `overlay_set_enabled()` /
`overlay_is_enabled()` so the runtime can switch the scheduler on or off via the
`-O` flag without recompiling.【F:overlay_sched.c†L10-L64】【F:overlay_sched.h†L1-L33】

### Feature extraction and caching

`overlay_feat_get_or_build()` attaches a `sess_feat_t` structure to the queue
entry on demand. It slices each seed into messages using the recorded AFLNet
regions, builds 256-bin byte histograms per message with L2 normalization, and
copies the most recent IPSM state sequence emitted for the session. The state
sequence is duplicated, sorted, and deduplicated to form an order-insensitive
state set whose FNV1a-style hash becomes one possible cluster signature; in
parallel, every 3-gram (k=3 shingle) over the state sequence is hashed, deduped,
and collapsed into a 64-bit signature so the runtime can switch to shingle-based
clustering without recomputing features.【F:overlay_sched.c†L80-L218】

The associated cleanup helpers `overlay_queue_prepare_entry()` and
`overlay_queue_release_entry()` clear any cached feature data when new queue
entries are created or destroyed so that memory usage stays bounded even as the
queue grows and shrinks.【F:overlay_sched.c†L23-L45】

### Similarity scoring and novelty ordering

`overlay_seq_similarity()` compares two sessions by greedily pairing the most
similar message histograms and averaging their cosine similarities while
treating any leftovers as zero-contribution matches; this feeds into the
novelty computation within `overlay_pick_next()`.【F:overlay_sched.c†L197-L229】【F:overlay_sched.c†L311-L344】

`overlay_pick_next()` groups candidates according to the active clustering
mode—state-set deduplication, no clustering, or k=3 state shingles—computes the
average similarity between each member and the rest of its cluster, turns that
into a novelty score (`1 - avg_sim_all`), and orders members from most to least
novel. A shared round-robin counter then walks layer by layer across clusters so
that every class contributes its next most novel seed in turn.【F:overlay_sched.c†L227-L413】

When the environment variables `AFL_DEBUG_OVERLAY` or `AFL_STAT_OVERLAY` are
set, the scheduler now emits detailed diagnostics: human-readable cluster and
selection summaries stream to `stderr`, and machine-friendly records are
appended to `<out_dir>/overlay_stats.log` for offline analysis.【F:overlay_sched.c†L12-L173】【F:overlay_sched.c†L340-L438】

### Queue window integration

`overlay_pick_from_queue_window()` maintains a fixed-size window of contiguous
queue entries, calls `overlay_pick_next()` to choose one candidate, and then
slides the window forward while keeping track of the next entry the outer loop
should resume from after fuzzing completes. When the scheduler is disabled, the
helper simply returns the current entry and advances the pointer so AFLNet’s
original queue walk is preserved.【F:overlay_sched.c†L403-L448】

## AFLNet integration points

`afl-fuzz.c` wires the overlay scheduler into both scheduling paths while
preserving AFLNet’s original heuristics. When targeting a specific IPSM state,
`choose_seed()` now passes the state’s candidate list through `overlay_pick_next()`
and advances the state-local index based on the item actually chosen.【F:afl-fuzz.c†L651-L745】
The `-O on|off` flag toggles the scheduler itself, while `-G <mode>` selects the
clustering strategy (`state`, `none`, or `shingle`). Both options are forwarded
to the overlay module so experiments can flip policies without recompilation.【F:afl-fuzz.c†L8068-L8104】【F:afl-fuzz.c†L8838-L9171】

Queue lifecycle hooks call `overlay_queue_prepare_entry()` when new items are
added and `overlay_queue_release_entry()` during teardown so that the overlay
cache stays consistent.【F:afl-fuzz.c†L1508-L1562】

During the main fuzzing loops, the scheduler replaces the plain queue pointer
with `overlay_pick_from_queue_window()` when traversing the queue in either the
code-aware or default scheduling modes. After each iteration, the loop asks the
overlay layer which entry should be processed next and resets the sliding window
if the queue is rewound or the scheduling strategy flips back to state-guided
mode.【F:afl-fuzz.c†L9289-L9521】

Together these pieces implement the requested novelty-aware scheduling policies
while letting users toggle between state-set, flat, and shingle-based clustering
without altering how AFLNet selects candidate batches or distributes energy
outside of the candidate window.
