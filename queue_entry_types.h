#define QUEUE_ENTRY_TYPES_H

#include "types.h"
#include "aflnet.h"

struct sess_feat;

struct queue_entry {

  u8 *fname;                          /* File name for the test case      */
  u32 len;                            /* Input length                     */

  u8  cal_failed,                     /* Calibration failed?              */
      trim_done,                      /* Trimmed?                         */
      was_fuzzed,                     /* Had any fuzzing done yet?        */
      passed_det,                     /* Deterministic stages passed?     */
      has_new_cov,                    /* Triggers new coverage?           */
      var_behavior,                   /* Variable behavior?               */
      favored,                        /* Currently favored?               */
      fs_redundant;                   /* Marked as redundant in the fs?   */

  u32 bitmap_size,                    /* Number of bits set in bitmap     */
      exec_cksum;                     /* Checksum of the execution trace  */

  u64 exec_us,                        /* Execution time (us)              */
      handicap,                       /* Number of queue cycles behind    */
      depth;                          /* Path depth                       */

  u8 *trace_mini;                     /* Trace bytes, if kept             */
  u32 tc_ref;                         /* Trace bytes ref count            */

  struct queue_entry *next,           /* Next element, if any             */
                     *next_100;       /* 100 elements ahead               */

  region_t *regions;                  /* Regions keeping information of message(s) sent to the server under test */
  u32 region_count;                   /* Total number of regions in this seed */
  u32 index;                          /* Index of this queue entry in the whole queue */
  u32 generating_state_id;            /* ID of the start at which the new seed was generated */
  u8 is_initial_seed;                 /* Is this an initial seed */
  u32 unique_state_count;             /* Unique number of states traversed by this queue entry */

  float novelty_score;                /* Cached novelty score used for scheduling */
  struct sess_feat *sess_feat;        /* Cached session-level features */

};

typedef struct queue_entry queue_entry_t;

#endif /* QUEUE_ENTRY_TYPES_H */
