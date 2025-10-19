#ifndef OVERLAY_SCHED_H
#define OVERLAY_SCHED_H

#include "queue_entry_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sess_feat {
  u32 msg_count;
  float *msg_hists;
  u32 state_count;
  u32 *states;
  u32 state_set_count;
  u32 *state_set;
  u32 signature;
  u32 shingle_count;
  u64 *shingle_hashes;
  u64 shingle_signature;
  u8 built;
} sess_feat_t;

typedef enum {
  OVERLAY_CLUSTER_STATE_SET = 0,
  OVERLAY_CLUSTER_NONE = 1,
  OVERLAY_CLUSTER_SHINGLE_K3 = 2,
  OVERLAY_CLUSTER_MAX
} overlay_cluster_mode_t;

void overlay_queue_reset(void);
void overlay_queue_release_entry(struct queue_entry *qe);
void overlay_queue_prepare_entry(struct queue_entry *qe);
struct queue_entry *overlay_queue_current(void);
sess_feat_t *overlay_feat_get_or_build(struct queue_entry *qe);
float overlay_seq_similarity(const sess_feat_t *A, const sess_feat_t *B);
struct queue_entry *overlay_pick_next(struct queue_entry **cand, u32 n_cand);
struct queue_entry *overlay_pick_from_queue_window(struct queue_entry *start);
void overlay_set_cluster_mode(u8 mode);
overlay_cluster_mode_t overlay_get_cluster_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_SCHED_H */
