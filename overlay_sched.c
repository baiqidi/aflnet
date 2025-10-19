#include "overlay_sched.h"

#include "alloc-inl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVERLAY_QUEUE_WINDOW 16

extern u8 *out_dir;

static overlay_cluster_mode_t overlay_cluster_mode =
    OVERLAY_CLUSTER_STATE_SET;

static u8 overlay_log_env_checked = 0;
static u8 overlay_debug_enabled = 0;
static u8 overlay_stat_enabled = 0;
static u8 overlay_stat_header_written = 0;
static FILE *overlay_stat_fp = NULL;
static u64 overlay_stat_round = 0;

static void overlay_logging_close(void) {
  if (overlay_stat_fp) {
    fclose(overlay_stat_fp);
    overlay_stat_fp = NULL;
  }
}

static void overlay_logging_try_open(void) {
  if (!overlay_stat_enabled || overlay_stat_fp || !out_dir) return;

  u8 *path = alloc_printf("%s/overlay_stats.log", out_dir);
  overlay_stat_fp = fopen((char *)path, "a");
  ck_free(path);

  if (!overlay_stat_fp) {
    overlay_stat_enabled = 0;
    return;
  }

  setvbuf(overlay_stat_fp, NULL, _IOLBF, 0);
  atexit(overlay_logging_close);
}

static void overlay_logging_init(void) {
  if (!overlay_log_env_checked) {
    const char *dbg = getenv("AFL_DEBUG_OVERLAY");
    overlay_debug_enabled = dbg && dbg[0] && dbg[0] != '0';

    const char *stat = getenv("AFL_STAT_OVERLAY");
    overlay_stat_enabled = stat && stat[0] && stat[0] != '0';

    overlay_log_env_checked = 1;
  }

  overlay_logging_try_open();
}

static void overlay_log_debug(const char *fmt, ...) {
  if (!overlay_debug_enabled) return;

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

static void overlay_logging_write_header(void) {
  if (!overlay_stat_enabled || !overlay_stat_fp || overlay_stat_header_written)
    return;

  fprintf(overlay_stat_fp,
          "# overlay scheduler log\n"
          "# fields: round type mode cluster rank candidates clusters signature "
          "key_len novelty msg_count state_count file\n");
  overlay_stat_header_written = 1;
  fflush(overlay_stat_fp);
}

static void overlay_log_stat(const char *fmt, ...) {
  if (!overlay_stat_enabled || !overlay_stat_fp) return;

  va_list ap;
  va_start(ap, fmt);
  vfprintf(overlay_stat_fp, fmt, ap);
  va_end(ap);
  fputc('\n', overlay_stat_fp);
  fflush(overlay_stat_fp);
}

static const char *overlay_cluster_mode_name(overlay_cluster_mode_t mode) {
  switch (mode) {
    case OVERLAY_CLUSTER_STATE_SET:
      return "state-set";
    case OVERLAY_CLUSTER_NONE:
      return "none";
    case OVERLAY_CLUSTER_SHINGLE_K3:
      return "shingle-k3";
    default:
      return "unknown";
  }
}

void overlay_set_cluster_mode(u8 mode) {
  overlay_logging_init();
  if (mode >= OVERLAY_CLUSTER_MAX) mode = OVERLAY_CLUSTER_STATE_SET;
  overlay_cluster_mode = (overlay_cluster_mode_t)mode;
  if (overlay_debug_enabled) {
    overlay_log_debug("[overlay] cluster mode set to %s\n",
                      overlay_cluster_mode_name(overlay_cluster_mode));
  }
}

overlay_cluster_mode_t overlay_get_cluster_mode(void) {
  return overlay_cluster_mode;
}

static const char *overlay_entry_label(const struct queue_entry *qe) {
  if (!qe || !qe->fname) return "<null>";
  const char *name = (const char *)qe->fname;
  const char *slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}

static struct queue_entry **overlay_queue_window = NULL;
static u32 overlay_queue_count = 0;
static struct queue_entry *overlay_queue_next_ptr = NULL;
static struct queue_entry *overlay_queue_next_cur = NULL;
static u64 overlay_rr_counter = 0;
static u64 overlay_rr_slots = 0;

static inline u32 rol32(u32 x, u8 r) {
  return (x << r) | (x >> (32 - r));
}

void overlay_queue_prepare_entry(struct queue_entry *qe) {
  overlay_logging_init();
  if (!qe) return;
  qe->novelty_score = 0.0f;
  if (qe->sess_feat) {
    sess_feat_t *feat = qe->sess_feat;
    if (feat->msg_hists) ck_free(feat->msg_hists);
    if (feat->states) ck_free(feat->states);
    if (feat->state_set) ck_free(feat->state_set);
    if (feat->shingle_hashes) ck_free(feat->shingle_hashes);
    ck_free(feat);
  }
  qe->sess_feat = NULL;
}

void overlay_queue_release_entry(struct queue_entry *qe) {
  overlay_logging_init();
  if (!qe || !qe->sess_feat) return;
  sess_feat_t *feat = qe->sess_feat;
  if (feat->msg_hists) ck_free(feat->msg_hists);
  if (feat->states) ck_free(feat->states);
  if (feat->state_set) ck_free(feat->state_set);
  if (feat->shingle_hashes) ck_free(feat->shingle_hashes);
  ck_free(feat);
  qe->sess_feat = NULL;
  qe->novelty_score = 0.0f;
}

void overlay_queue_reset(void) {
  overlay_logging_init();
  overlay_queue_count = 0;
  overlay_queue_next_ptr = NULL;
  overlay_queue_next_cur = NULL;
  overlay_rr_counter = 0;
  overlay_rr_slots = 0;
}

struct queue_entry *overlay_queue_current(void) {
  return overlay_queue_next_cur;
}

static int cmp_u32(const void *a, const void *b) {
  const u32 va = *(const u32 *)a;
  const u32 vb = *(const u32 *)b;
  if (va < vb) return -1;
  if (va > vb) return 1;
  return 0;
}

static int cmp_u64(const void *a, const void *b) {
  const u64 va = *(const u64 *)a;
  const u64 vb = *(const u64 *)b;
  if (va < vb) return -1;
  if (va > vb) return 1;
  return 0;
}

static inline u64 shingle_mix64(u64 x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static u64 shingle_hash_triplet(u32 a, u32 b, u32 c) {
  u64 seed = ((u64)a << 32) ^ ((u64)b << 16) ^ (u64)c;
  return shingle_mix64(seed);
}

static u64 combine_shingle_set(const u64 *vals, u32 count) {
  if (!vals || !count) return 0;

  u64 hash = 1469598103934665603ULL;        /* FNV-1a offset */
  const u64 prime = 1099511628211ULL;       /* FNV-1a prime  */

  for (u32 i = 0; i < count; ++i) {
    hash ^= vals[i];
    hash *= prime;
  }

  hash ^= count;
  hash *= prime;
  return hash;
}

static u32 state_set_signature(const u32 *states, u32 n_states) {
  if (!states || !n_states) return 0;

  u32 hash = 2166136261u;
  for (u32 i = 0; i < n_states; ++i) {
    hash ^= (u32)states[i];
    hash *= 16777619u;
  }

  hash ^= n_states;
  return hash;
}

sess_feat_t *overlay_feat_get_or_build(struct queue_entry *qe) {
  overlay_logging_init();
  if (!qe) return NULL;

  sess_feat_t *feat = qe->sess_feat;
  if (feat && feat->built) return feat;

  if (!feat) {
    feat = (sess_feat_t *)ck_alloc(sizeof(sess_feat_t));
  } else {
    if (feat->msg_hists) ck_free(feat->msg_hists);
    if (feat->states) ck_free(feat->states);
    if (feat->state_set) ck_free(feat->state_set);
    if (feat->shingle_hashes) ck_free(feat->shingle_hashes);
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
  feat->state_set_count = 0;
  feat->state_set = NULL;
  feat->shingle_count = 0;
  feat->shingle_hashes = NULL;
  feat->shingle_signature = 0;
  if (state_count && states_src) {
    feat->states = (u32 *)ck_alloc(sizeof(u32) * state_count);
    for (u32 i = 0; i < state_count; ++i) {
      feat->states[i] = states_src[i];
    }

    feat->state_set = (u32 *)ck_alloc(sizeof(u32) * state_count);
    memcpy(feat->state_set, feat->states, sizeof(u32) * state_count);
    qsort(feat->state_set, state_count, sizeof(u32), cmp_u32);

    u32 unique = 0;
    for (u32 i = 0; i < state_count; ++i) {
      if (!i || feat->state_set[i] != feat->state_set[i - 1]) {
        feat->state_set[unique++] = feat->state_set[i];
      }
    }

    if (unique) {
      u32 *dedup = (u32 *)ck_alloc(sizeof(u32) * unique);
      memcpy(dedup, feat->state_set, sizeof(u32) * unique);
      ck_free(feat->state_set);
      feat->state_set = dedup;
      feat->state_set_count = unique;
    } else {
      ck_free(feat->state_set);
      feat->state_set = NULL;
      feat->state_set_count = 0;
    }

  } else {
    feat->states = NULL;
    feat->state_set = NULL;
    feat->state_set_count = 0;
  }

  if (feat->states && state_count >= 3) {
    u32 gram_total = state_count - 2;
    u64 *hashes = (u64 *)ck_alloc(sizeof(u64) * gram_total);
    for (u32 i = 0; i < gram_total; ++i) {
      hashes[i] = shingle_hash_triplet(feat->states[i], feat->states[i + 1],
                                       feat->states[i + 2]);
    }

    qsort(hashes, gram_total, sizeof(u64), cmp_u64);

    u32 unique = 0;
    for (u32 i = 0; i < gram_total; ++i) {
      if (!i || hashes[i] != hashes[i - 1]) {
        hashes[unique++] = hashes[i];
      }
    }

    if (unique) {
      feat->shingle_hashes = (u64 *)ck_alloc(sizeof(u64) * unique);
      memcpy(feat->shingle_hashes, hashes, sizeof(u64) * unique);
      feat->shingle_count = unique;
      feat->shingle_signature = combine_shingle_set(feat->shingle_hashes, unique);
    }

    ck_free(hashes);
  }

  feat->signature = state_set_signature(feat->state_set, feat->state_set_count);
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
  overlay_logging_init();
  if (!A || !B) return 0.0f;

  u32 m = A->msg_count;
  u32 n = B->msg_count;

  if (!m && !n) return 0.0f;

  u32 denom = m > n ? m : n;
  if (!denom) return 0.0f;

  if (!A->msg_hists || !B->msg_hists) return 0.0f;

  u32 pair_count = m < n ? m : n;
  float sum = 0.0f;

  if (pair_count) {
    u8 *used_a = (u8 *)ck_alloc(m);
    u8 *used_b = (u8 *)ck_alloc(n);
    memset(used_a, 0, m);
    memset(used_b, 0, n);

    for (u32 k = 0; k < pair_count; ++k) {
      float best = -1.0f;
      u32 best_i = (u32)-1;
      u32 best_j = (u32)-1;

      for (u32 i = 0; i < m; ++i) {
        if (used_a[i]) continue;
        const float *hist_a = A->msg_hists + i * 256;
        for (u32 j = 0; j < n; ++j) {
          if (used_b[j]) continue;
          const float *hist_b = B->msg_hists + j * 256;
          float sim = histogram_similarity(hist_a, hist_b);
          if (sim > best) {
            best = sim;
            best_i = i;
            best_j = j;
          }
        }
      }

      if (best_i == (u32)-1 || best_j == (u32)-1) break;

      used_a[best_i] = 1;
      used_b[best_j] = 1;
      if (best > 0.0f) sum += best;
    }

    ck_free(used_a);
    ck_free(used_b);
  }

  return sum / (float)denom;
}

static u64 overlay_feature_signature(const sess_feat_t *feat) {
  if (!feat) return 0;

  switch (overlay_cluster_mode) {
    case OVERLAY_CLUSTER_STATE_SET:
      return (u64)feat->signature;
    case OVERLAY_CLUSTER_NONE:
      return 0;
    case OVERLAY_CLUSTER_SHINGLE_K3:
      return feat->shingle_signature;
    default:
      return (u64)feat->signature;
  }
}

static u32 overlay_feature_key_len(const sess_feat_t *feat) {
  if (!feat) return 0;

  switch (overlay_cluster_mode) {
    case OVERLAY_CLUSTER_STATE_SET:
      return feat->state_set_count;
    case OVERLAY_CLUSTER_NONE:
      return 0;
    case OVERLAY_CLUSTER_SHINGLE_K3:
      return feat->shingle_count;
    default:
      return feat->state_set_count;
  }
}

static u8 overlay_feature_key_size(void) {
  switch (overlay_cluster_mode) {
    case OVERLAY_CLUSTER_STATE_SET:
      return sizeof(u32);
    case OVERLAY_CLUSTER_NONE:
      return 0;
    case OVERLAY_CLUSTER_SHINGLE_K3:
      return sizeof(u64);
    default:
      return sizeof(u32);
  }
}

static const void *overlay_feature_key_ptr(const sess_feat_t *feat) {
  if (!feat) return NULL;

  switch (overlay_cluster_mode) {
    case OVERLAY_CLUSTER_STATE_SET:
      return feat->state_set;
    case OVERLAY_CLUSTER_NONE:
      return NULL;
    case OVERLAY_CLUSTER_SHINGLE_K3:
      return feat->shingle_hashes;
    default:
      return feat->state_set;
  }
}

struct queue_entry *overlay_pick_next(struct queue_entry **cand, u32 n_cand) {
  overlay_logging_init();
  if (!cand || !n_cand) return NULL;

  struct queue_entry **candidates = cand;
  sess_feat_t **features = (sess_feat_t **)ck_alloc(sizeof(sess_feat_t *) * n_cand);

  u64 round_id = 0;
  const char *mode_name = overlay_cluster_mode_name(overlay_cluster_mode);
  if (overlay_debug_enabled || overlay_stat_enabled) {
    round_id = ++overlay_stat_round;
    overlay_logging_write_header();
    overlay_log_debug("[overlay] round %llu: %u candidates mode=%s\n",
                      (unsigned long long)round_id, n_cand, mode_name);
    overlay_log_stat(
        "round=%llu type=start mode=%s cluster=-1 rank=-1 candidates=%u "
        "clusters=0 signature=0 key_len=0 novelty=0 msg_count=0 state_count=0 "
        "file=\"-\"",
        (unsigned long long)round_id, mode_name, n_cand);
  }

  for (u32 i = 0; i < n_cand; ++i) {
    features[i] = overlay_feat_get_or_build(candidates[i]);
    if (candidates[i]) candidates[i]->novelty_score = 0.0f;

    if (overlay_debug_enabled || overlay_stat_enabled) {
      const char *label = overlay_entry_label(candidates[i]);
      if (features[i]) {
        u64 sig = overlay_feature_signature(features[i]);
        u32 key_len = overlay_feature_key_len(features[i]);
        overlay_log_debug(
            "[overlay]   cand[%u] file=%s msgs=%u states=%u key=%u sig=0x%016llx\n",
            i, label, features[i]->msg_count, features[i]->state_count,
            key_len, (unsigned long long)sig);
        overlay_log_stat(
            "round=%llu type=candidate mode=%s cluster=-1 rank=%u candidates=%u "
            "clusters=0 signature=0x%016llx key_len=%u novelty=0 msg_count=%u "
            "state_count=%u file=\"%s\"",
            (unsigned long long)round_id, mode_name, i, n_cand,
            (unsigned long long)sig, key_len, features[i]->msg_count,
            features[i]->state_count, label);
      } else {
        overlay_log_debug(
            "[overlay]   cand[%u] file=%s has no cached features\n", i, label);
        overlay_log_stat(
            "round=%llu type=candidate mode=%s cluster=-1 rank=%u candidates=%u "
            "clusters=0 signature=0 key_len=0 novelty=0 msg_count=0 "
            "state_count=0 file=\"%s\"",
            (unsigned long long)round_id, mode_name, i, n_cand, label);
      }
    }
  }

  struct cluster_info {
    u64 signature;
    u32 key_len;
    u8 key_size;
    const void *key_ptr;
    u32 count;
    u32 *indices;
    float *scores;
    u32 *order;
  };

  struct cluster_info *clusters =
      (struct cluster_info *)ck_alloc(sizeof(struct cluster_info) * n_cand);
  memset(clusters, 0, sizeof(struct cluster_info) * n_cand);
  u32 cluster_count = 0;

  for (u32 i = 0; i < n_cand; ++i) {
    sess_feat_t *feat = features[i];
    u64 sig = overlay_feature_signature(feat);
    u32 key_len = overlay_feature_key_len(feat);
    const void *key_ptr = overlay_feature_key_ptr(feat);
    u8 key_size = overlay_feature_key_size();
    u32 cid = 0;

    for (; cid < cluster_count; ++cid) {
      if (clusters[cid].signature != sig) continue;
      if (clusters[cid].key_len != key_len) continue;
      if (clusters[cid].key_size != key_size) continue;
      if (!key_len || !key_size || !clusters[cid].key_ptr || !key_ptr) break;
      if (memcmp(clusters[cid].key_ptr, key_ptr, (size_t)key_len * key_size) == 0)
        break;
    }

    if (cid == cluster_count) {
      clusters[cid].signature = sig;
      clusters[cid].key_len = key_len;
      clusters[cid].key_size = key_size;
      clusters[cid].key_ptr = key_ptr;
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

    if (overlay_debug_enabled || overlay_stat_enabled) {
      overlay_log_debug(
          "[overlay] cluster[%u] signature=0x%016llx members=%u key=%u\n", cid,
          (unsigned long long)clusters[cid].signature, clusters[cid].count,
          clusters[cid].key_len);
      overlay_log_stat(
          "round=%llu type=cluster mode=%s cluster=%u rank=-1 candidates=%u "
          "clusters=%u signature=0x%016llx key_len=%u novelty=0 msg_count=0 "
          "state_count=0 file=\"-\"",
          (unsigned long long)round_id, mode_name, cid, clusters[cid].count,
          cluster_count, (unsigned long long)clusters[cid].signature,
          clusters[cid].key_len);
    }

    if (m <= 1) {
      clusters[cid].scores[0] = 1.0f;
      u32 only_idx = clusters[cid].indices[0];
      if (candidates[only_idx]) candidates[only_idx]->novelty_score = 1.0f;
      if (overlay_debug_enabled || overlay_stat_enabled) {
        const char *label = overlay_entry_label(candidates[only_idx]);
        overlay_log_debug(
            "[overlay]   single-member cluster[%u] file=%s novelty=1.0000\n",
            cid, label);
        u32 msg_cnt = features[clusters[cid].indices[0]]
                          ? features[clusters[cid].indices[0]]->msg_count
                          : 0;
        u32 state_cnt = features[clusters[cid].indices[0]]
                            ? features[clusters[cid].indices[0]]->state_count
                            : 0;
        overlay_log_stat(
            "round=%llu type=member mode=%s cluster=%u rank=0 candidates=%u "
            "clusters=%u signature=0x%016llx key_len=%u novelty=1 msg_count=%u "
            "state_count=%u file=\"%s\"",
            (unsigned long long)round_id, mode_name, cid, clusters[cid].count,
            cluster_count, (unsigned long long)clusters[cid].signature,
            clusters[cid].key_len, msg_cnt, state_cnt, label);
      }
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

    if (overlay_debug_enabled || overlay_stat_enabled) {
      for (u32 layer = 0; layer < m; ++layer) {
        u32 order_idx = clusters[cid].order[layer];
        u32 cand_index = clusters[cid].indices[order_idx];
        const char *label = overlay_entry_label(candidates[cand_index]);
        float novelty = clusters[cid].scores[order_idx];
        u32 msg_cnt = features[cand_index] ? features[cand_index]->msg_count : 0;
        u32 state_cnt =
            features[cand_index] ? features[cand_index]->state_count : 0;
        overlay_log_debug(
            "[overlay]   cluster[%u] rank=%u file=%s novelty=%.4f\n", cid,
            layer, label, novelty);
        overlay_log_stat(
            "round=%llu type=member mode=%s cluster=%u rank=%u candidates=%u "
            "clusters=%u signature=0x%016llx key_len=%u novelty=%0.6f msg_count=%u "
            "state_count=%u file=\"%s\"",
            (unsigned long long)round_id, mode_name, cid, layer,
            clusters[cid].count, cluster_count,
            (unsigned long long)clusters[cid].signature, clusters[cid].key_len,
            novelty, msg_cnt, state_cnt, label);
      }
    }
  }

  if (!cluster_count) {
    ck_free(features);
    ck_free(clusters);
    return candidates[0];
  }

  u32 total = 0;
  u32 max_depth = 0;
  for (u32 cid = 0; cid < cluster_count; ++cid) {
    total += clusters[cid].count;
    if (clusters[cid].count > max_depth) max_depth = clusters[cid].count;
  }

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
  u32 selected_cluster = (u32)-1;
  u32 selected_rank = (u32)-1;
  u32 selected_index = (u32)-1;
  float selected_score = 0.0f;
  u64 selected_signature = 0;
  u64 slots = (u64)cluster_count * (u64)(max_depth ? max_depth : 1);
  if (!slots) {
    overlay_rr_counter = 0;
    overlay_rr_slots = 0;
    selected = candidates[0];
    selected_index = 0;
  } else {
    if (overlay_rr_slots != slots) {
      overlay_rr_counter = slots ? (overlay_rr_counter % slots) : 0;
    }
    overlay_rr_slots = slots;

    for (u64 step = 0; step < slots && !selected; ++step) {
      u64 pos = overlay_rr_counter;
      overlay_rr_counter = (overlay_rr_counter + 1) % slots;
      u32 layer = (u32)(pos / cluster_count);
      u32 cid = (u32)(pos % cluster_count);
      if (layer >= clusters[cid].count) continue;
      struct cluster_info *cluster = &clusters[cid];
      u32 order_idx = cluster->order[layer];
      u32 candidate_index = cluster->indices[order_idx];
      selected = candidates[candidate_index];
      selected_cluster = cid;
      selected_rank = layer;
      selected_index = candidate_index;
      selected_score = cluster->scores[order_idx];
      selected_signature = cluster->signature;
    }

    if (!selected) {
      selected = candidates[0];
      selected_index = 0;
    }
  }

  if (selected && selected_index != (u32)-1 && selected_cluster == (u32)-1) {
    for (u32 cid = 0; cid < cluster_count && selected_cluster == (u32)-1; ++cid) {
      struct cluster_info *cluster = &clusters[cid];
      for (u32 layer = 0; layer < cluster->count; ++layer) {
        u32 order_idx = cluster->order[layer];
        if (cluster->indices[order_idx] == selected_index) {
          selected_cluster = cid;
          selected_rank = layer;
          selected_score = cluster->scores[order_idx];
          selected_signature = cluster->signature;
          break;
        }
      }
    }
  }

  if (selected && (overlay_debug_enabled || overlay_stat_enabled)) {
    const char *label = overlay_entry_label(selected);
    u32 key_len_log = 0;
    if (selected_cluster != (u32)-1 && selected_cluster < cluster_count) {
      key_len_log = clusters[selected_cluster].key_len;
    }
    overlay_log_debug(
        "[overlay] selected file=%s cluster=%u rank=%u novelty=%.4f\n", label,
        selected_cluster == (u32)-1 ? 0 : selected_cluster,
        selected_rank == (u32)-1 ? 0 : selected_rank, selected_score);
    overlay_log_stat(
        "round=%llu type=selection mode=%s cluster=%u rank=%u candidates=%u "
        "clusters=%u signature=0x%016llx key_len=%u novelty=%0.6f msg_count=0 "
        "state_count=0 file=\"%s\"",
        (unsigned long long)round_id, mode_name,
        selected_cluster == (u32)-1 ? 0 : selected_cluster,
        selected_rank == (u32)-1 ? 0 : selected_rank, total, cluster_count,
        (unsigned long long)selected_signature, key_len_log, selected_score,
        label);
  }

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
