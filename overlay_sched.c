#include "overlay_sched.h"

#include "alloc-inl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define OVERLAY_QUEUE_WINDOW 16

static struct queue_entry **overlay_queue_window = NULL;
static u32 overlay_queue_count = 0;
static struct queue_entry *overlay_queue_next_ptr = NULL;
static struct queue_entry *overlay_queue_next_cur = NULL;
static u64 overlay_rr_pos = 0;

static inline u32 rol32(u32 x, u8 r) {
  return (x << r) | (x >> (32 - r));
}

void overlay_queue_prepare_entry(struct queue_entry *qe) {
  if (!qe) return;
  qe->novelty_score = 0.0f;
  if (qe->sess_feat) {
    sess_feat_t *feat = qe->sess_feat;
    if (feat->msg_hists) ck_free(feat->msg_hists);
    if (feat->states) ck_free(feat->states);
    ck_free(feat);
  }
  qe->sess_feat = NULL;
}

void overlay_queue_release_entry(struct queue_entry *qe) {
  if (!qe || !qe->sess_feat) return;
  sess_feat_t *feat = qe->sess_feat;
  if (feat->msg_hists) ck_free(feat->msg_hists);
  if (feat->states) ck_free(feat->states);
  ck_free(feat);
  qe->sess_feat = NULL;
  qe->novelty_score = 0.0f;
}

void overlay_queue_reset(void) {
  overlay_queue_count = 0;
  overlay_queue_next_ptr = NULL;
  overlay_queue_next_cur = NULL;
}

struct queue_entry *overlay_queue_current(void) {
  return overlay_queue_next_cur;
}

static u32 state_signature_k3(const u16 *states, u32 n_states) {
  if (!states || !n_states) return 0;

  u32 hash = 2166136261u;

  if (n_states < 3) {
    for (u32 i = 0; i < n_states; ++i) {
      hash ^= (u32)states[i] + 0x9e3779b9u * i;
      hash *= 16777619u;
    }
    hash ^= n_states;
    return hash;
  }

  for (u32 i = 0; i + 2 < n_states; ++i) {
    u32 a = (u32)states[i];
    u32 b = (u32)states[i + 1];
    u32 c = (u32)states[i + 2];
    u32 shingle = rol32(a ^ (b + 0x9e3779b9u), 7);
    shingle = rol32(shingle ^ (c + 0x7f4a7c15u), 11);
    hash ^= shingle + i * 0x9e3779b9u;
    hash = rol32(hash, 3) * 16777619u;
  }

  hash ^= n_states;
  return hash;
}

sess_feat_t *overlay_feat_get_or_build(struct queue_entry *qe) {
  if (!qe) return NULL;

  sess_feat_t *feat = qe->sess_feat;
  if (feat && feat->built) return feat;

  if (!feat) {
    feat = (sess_feat_t *)ck_alloc(sizeof(sess_feat_t));
  } else {
    if (feat->msg_hists) ck_free(feat->msg_hists);
    if (feat->states) ck_free(feat->states);
  }

  memset(feat, 0, sizeof(sess_feat_t));

  u32 msg_count = qe->region_count ? qe->region_count : (qe->len ? 1 : 0);
  feat->msg_count = msg_count;

  if (msg_count) {
    feat->msg_hists = (float *)ck_alloc(sizeof(float) * msg_count * 256);
    memset(feat->msg_hists, 0, sizeof(float) * msg_count * 256);

    FILE *fp = fopen((char *)qe->fname, "rb");
    if (fp) {
      u8 *buf = (u8 *)ck_alloc(qe->len ? qe->len : 1);
      size_t read_sz = fread(buf, 1, qe->len, fp);
      fclose(fp);

      if (!qe->len) read_sz = 0;

      for (u32 i = 0; i < msg_count; ++i) {
        int start = qe->region_count ? qe->regions[i].start_byte : 0;
        int end = qe->region_count ? qe->regions[i].end_byte : (qe->len ? (s32)qe->len - 1 : -1);

        if (start < 0) start = 0;
        if (end < start || (u32)start >= read_sz) continue;
        if ((u32)end >= read_sz) end = (s32)read_sz - 1;

        float *hist = feat->msg_hists + (i * 256);
        for (int pos = start; pos <= end; ++pos) {
          hist[buf[pos]] += 1.0f;
        }

        float norm_sq = 0.0f;
        for (u32 b = 0; b < 256; ++b) {
          norm_sq += hist[b] * hist[b];
        }
        if (norm_sq > 0.0f) {
          float inv = 1.0f / sqrtf(norm_sq);
          for (u32 b = 0; b < 256; ++b) {
            hist[b] *= inv;
          }
        }
      }

      ck_free(buf);
    }
  }

  u32 state_count = 0;
  const unsigned int *states_src = NULL;

  if (qe->region_count) {
    for (s32 idx = (s32)qe->region_count - 1; idx >= 0; --idx) {
      if (qe->regions[idx].state_sequence && qe->regions[idx].state_count) {
        state_count = qe->regions[idx].state_count;
        states_src = qe->regions[idx].state_sequence;
        break;
      }
    }
  }

  feat->state_count = state_count;
  if (state_count && states_src) {
    feat->states = (u16 *)ck_alloc(sizeof(u16) * state_count);
    for (u32 i = 0; i < state_count; ++i) {
      feat->states[i] = (u16)(states_src[i] & 0xFFFF);
    }
  } else {
    feat->states = NULL;
  }

  feat->signature = state_signature_k3(feat->states, feat->state_count);
  feat->built = 1;

  qe->sess_feat = feat;
  return feat;
}

static float histogram_similarity(const float *a, const float *b) {
  if (!a || !b) return 0.0f;
  float dot = 0.0f;
  for (u32 i = 0; i < 256; ++i) {
    dot += a[i] * b[i];
  }
  return dot;
}

float overlay_seq_similarity(const sess_feat_t *A, const sess_feat_t *B) {
  if (!A || !B) return 0.0f;
  u32 m = A->msg_count;
  u32 n = B->msg_count;

  if (!m || !n) return 0.0f;

  float *sims = (float *)ck_alloc(sizeof(float) * m * n);
  for (u32 i = 0; i < m; ++i) {
    for (u32 j = 0; j < n; ++j) {
      sims[i * n + j] = histogram_similarity(A->msg_hists + i * 256,
                                             B->msg_hists + j * 256);
    }
  }

  u8 *usedA = (u8 *)ck_alloc(m);
  u8 *usedB = (u8 *)ck_alloc(n);
  memset(usedA, 0, m);
  memset(usedB, 0, n);

  float total = 0.0f;
  u32 matches = 0;
  u32 max_pairs = m < n ? m : n;

  while (matches < max_pairs) {
    float best = -1.0f;
    u32 best_i = 0, best_j = 0;
    for (u32 i = 0; i < m; ++i) {
      if (usedA[i]) continue;
      for (u32 j = 0; j < n; ++j) {
        if (usedB[j]) continue;
        float val = sims[i * n + j];
        if (val > best) {
          best = val;
          best_i = i;
          best_j = j;
        }
      }
    }

    if (best < 0.0f) break;

    usedA[best_i] = 1;
    usedB[best_j] = 1;
    total += best;
    matches++;
  }

  ck_free(sims);
  ck_free(usedA);
  ck_free(usedB);

  u32 denom = m > n ? m : n;
  if (!denom) return 0.0f;
  return total / (float)denom;
}

struct queue_entry *overlay_pick_next(struct queue_entry **cand, u32 n_cand) {
  if (!cand || !n_cand) return NULL;

  struct queue_entry **candidates = cand;
  sess_feat_t **features = (sess_feat_t **)ck_alloc(sizeof(sess_feat_t *) * n_cand);

  for (u32 i = 0; i < n_cand; ++i) {
    features[i] = overlay_feat_get_or_build(candidates[i]);
    if (candidates[i]) candidates[i]->novelty_score = 0.0f;
  }

  struct cluster_info {
    u32 signature;
    u32 count;
    u32 *indices;
    float *scores;
    u32 *order;
  };

  struct cluster_info *clusters = (struct cluster_info *)ck_alloc(sizeof(struct cluster_info) * n_cand);
  u32 cluster_count = 0;

  for (u32 i = 0; i < n_cand; ++i) {
    u32 sig = features[i] ? features[i]->signature : 0;
    u32 cid = 0;
    for (; cid < cluster_count; ++cid) {
      if (clusters[cid].signature == sig) break;
    }
    if (cid == cluster_count) {
      clusters[cid].signature = sig;
      clusters[cid].count = 0;
      clusters[cid].indices = (u32 *)ck_alloc(sizeof(u32) * n_cand);
      clusters[cid].scores = NULL;
      clusters[cid].order = NULL;
      cluster_count++;
    }
    clusters[cid].indices[clusters[cid].count++] = i;
  }

  if (cluster_count > 1) {
    for (u32 i = 0; i < cluster_count - 1; ++i) {
      for (u32 j = i + 1; j < cluster_count; ++j) {
        if (clusters[i].signature > clusters[j].signature) {
          struct cluster_info tmp = clusters[i];
          clusters[i] = clusters[j];
          clusters[j] = tmp;
        }
      }
    }
  }

  for (u32 cid = 0; cid < cluster_count; ++cid) {
    u32 m = clusters[cid].count;
    clusters[cid].scores = (float *)ck_alloc(sizeof(float) * m);
    clusters[cid].order = (u32 *)ck_alloc(sizeof(u32) * m);
    for (u32 i = 0; i < m; ++i) clusters[cid].order[i] = i;

    if (m <= 1) {
      clusters[cid].scores[0] = 1.0f;
      u32 only_idx = clusters[cid].indices[0];
      if (candidates[only_idx]) candidates[only_idx]->novelty_score = 1.0f;
      continue;
    }

    for (u32 i = 0; i < m; ++i) {
      float sum = 0.0f;
      for (u32 j = 0; j < m; ++j) {
        if (i == j) continue;
        sess_feat_t *fi = features[clusters[cid].indices[i]];
        sess_feat_t *fj = features[clusters[cid].indices[j]];
        float sim = overlay_seq_similarity(fi, fj);
        sum += sim;
      }
      float avg = 0.0f;
      if (m > 1) avg = sum / (float)(m - 1);
      clusters[cid].scores[i] = 1.0f - avg;
      u32 idx = clusters[cid].indices[i];
      if (candidates[idx]) candidates[idx]->novelty_score = clusters[cid].scores[i];
    }

    for (u32 i = 0; i < m - 1; ++i) {
      for (u32 j = i + 1; j < m; ++j) {
        float si = clusters[cid].scores[clusters[cid].order[i]];
        float sj = clusters[cid].scores[clusters[cid].order[j]];
        if (sj > si) {
          u32 tmp = clusters[cid].order[i];
          clusters[cid].order[i] = clusters[cid].order[j];
          clusters[cid].order[j] = tmp;
        }
      }
    }
  }

  if (!cluster_count) {
    ck_free(features);
    ck_free(clusters);
    return candidates[0];
  }

  u32 total = 0;
  for (u32 cid = 0; cid < cluster_count; ++cid) total += clusters[cid].count;

  if (!total) {
    ck_free(features);
    for (u32 cid = 0; cid < cluster_count; ++cid) {
      ck_free(clusters[cid].indices);
      if (clusters[cid].scores) ck_free(clusters[cid].scores);
      if (clusters[cid].order) ck_free(clusters[cid].order);
    }
    ck_free(clusters);
    return candidates[0];
  }

  struct queue_entry *selected = NULL;
  u32 start_pos = (u32)(overlay_rr_pos % cluster_count);
  u32 depth = 0;

  while (!selected && depth < total) {
    int any = 0;
    for (u32 step = 0; step < cluster_count; ++step) {
      u32 cid = (start_pos + step) % cluster_count;
      struct cluster_info *cluster = &clusters[cid];
      if (depth >= cluster->count) continue;
      any = 1;
      u32 idx_in_cluster = cluster->order[depth];
      u32 candidate_index = cluster->indices[idx_in_cluster];
      selected = candidates[candidate_index];
      if (selected) break;
    }
    if (!any) break;
    depth++;
  }

  if (!selected) selected = candidates[0];

  overlay_rr_pos = (overlay_rr_pos + 1) % (cluster_count ? cluster_count : 1);

  for (u32 cid = 0; cid < cluster_count; ++cid) {
    ck_free(clusters[cid].indices);
    if (clusters[cid].scores) ck_free(clusters[cid].scores);
    if (clusters[cid].order) ck_free(clusters[cid].order);
  }

  ck_free(clusters);
  ck_free(features);

  return selected;
}

struct queue_entry *overlay_pick_from_queue_window(struct queue_entry *start) {
  if (!start) return NULL;

  if (!overlay_queue_window) {
    overlay_queue_window =
        (struct queue_entry **)ck_alloc(sizeof(struct queue_entry *) * OVERLAY_QUEUE_WINDOW);
  }

  if (!overlay_queue_count || overlay_queue_window[0] != start) {
    overlay_queue_count = 0;
    struct queue_entry *it = start;
    while (it && overlay_queue_count < OVERLAY_QUEUE_WINDOW) {
      overlay_queue_window[overlay_queue_count++] = it;
      it = it->next;
    }
    overlay_queue_next_ptr = it;
  }

  struct queue_entry *choice = overlay_pick_next(overlay_queue_window, overlay_queue_count);
  if (!choice) choice = start;

  for (u32 i = 0; i < overlay_queue_count; ++i) {
    if (overlay_queue_window[i] == choice) {
      for (u32 j = i; j + 1 < overlay_queue_count; ++j) {
        overlay_queue_window[j] = overlay_queue_window[j + 1];
      }
      overlay_queue_count--;
      break;
    }
  }

  while (overlay_queue_next_ptr && overlay_queue_count < OVERLAY_QUEUE_WINDOW) {
    overlay_queue_window[overlay_queue_count++] = overlay_queue_next_ptr;
    overlay_queue_next_ptr = overlay_queue_next_ptr->next;
  }

  if (overlay_queue_count > 0) {
    overlay_queue_next_cur = overlay_queue_window[0];
  } else {
    overlay_queue_next_cur = overlay_queue_next_ptr;
  }

  return choice;
}
