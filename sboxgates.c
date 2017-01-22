/*
 * Program for finding low gate count implementations of S-boxes.
 * The algorithm is an improved version of the one described in Kwan, Matthew: "Reducing the Gate
 * Count of Bitslice DES." IACR Cryptology ePrint Archive 2000 (2000): 51.
 *
 * Copyright (c) 2016-2017 Marcus Dansarie
 */

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <x86intrin.h>

#define MAX_GATES 500
#define MIN_STACK_SIZE 0x200000
#define NO_GATE ((uint64_t)-1)

typedef enum {IN, NOT, AND, OR, XOR} gate_type;

typedef __m256i ttable;

typedef struct {
  gate_type type;
  ttable table;
  uint64_t in1; /* Input 1 to the gate. NO_GATE for the inputs. */
  uint64_t in2; /* Input 2 to the gate. NO_GATE for NOT gates and the inputs. */
} gate;

typedef struct {
  uint64_t max_gates;
  uint64_t num_gates;  /* Current number of gates. */
  uint64_t outputs[8]; /* Gate number of the respective output gates, or NO_GATE. */
  gate gates[MAX_GATES];
} state;

const uint8_t g_sbox_enc[] = {
    0x9c, 0xf2, 0x14, 0xc1, 0x8e, 0xcb, 0xb2, 0x65, 0x97, 0x7a, 0x60, 0x17, 0x92, 0xf9, 0x78, 0x41,
    0x07, 0x4c, 0x67, 0x6d, 0x66, 0x4a, 0x30, 0x7d, 0x53, 0x9d, 0xb5, 0xbc, 0xc3, 0xca, 0xf1, 0x04,
    0x03, 0xec, 0xd0, 0x38, 0xb0, 0xed, 0xad, 0xc4, 0xdd, 0x56, 0x42, 0xbd, 0xa0, 0xde, 0x1b, 0x81,
    0x55, 0x44, 0x5a, 0xe4, 0x50, 0xdc, 0x43, 0x63, 0x09, 0x5c, 0x74, 0xcf, 0x0e, 0xab, 0x1d, 0x3d,
    0x6b, 0x02, 0x5d, 0x28, 0xe7, 0xc6, 0xee, 0xb4, 0xd9, 0x7c, 0x19, 0x3e, 0x5e, 0x6c, 0xd6, 0x6e,
    0x2a, 0x13, 0xa5, 0x08, 0xb9, 0x2d, 0xbb, 0xa2, 0xd4, 0x96, 0x39, 0xe0, 0xba, 0xd7, 0x82, 0x33,
    0x0d, 0x5f, 0x26, 0x16, 0xfe, 0x22, 0xaf, 0x00, 0x11, 0xc8, 0x9e, 0x88, 0x8b, 0xa1, 0x7b, 0x87,
    0x27, 0xe6, 0xc7, 0x94, 0xd1, 0x5b, 0x9b, 0xf0, 0x9f, 0xdb, 0xe1, 0x8d, 0xd2, 0x1f, 0x6a, 0x90,
    0xf4, 0x18, 0x91, 0x59, 0x01, 0xb1, 0xfc, 0x34, 0x3c, 0x37, 0x47, 0x29, 0xe2, 0x64, 0x69, 0x24,
    0x0a, 0x2f, 0x73, 0x71, 0xa9, 0x84, 0x8c, 0xa8, 0xa3, 0x3b, 0xe3, 0xe9, 0x58, 0x80, 0xa7, 0xd3,
    0xb7, 0xc2, 0x1c, 0x95, 0x1e, 0x4d, 0x4f, 0x4e, 0xfb, 0x76, 0xfd, 0x99, 0xc5, 0xc9, 0xe8, 0x2e,
    0x8a, 0xdf, 0xf5, 0x49, 0xf3, 0x6f, 0x8f, 0xe5, 0xeb, 0xf6, 0x25, 0xd5, 0x31, 0xc0, 0x57, 0x72,
    0xaa, 0x46, 0x68, 0x0b, 0x93, 0x89, 0x83, 0x70, 0xef, 0xa4, 0x85, 0xf8, 0x0f, 0xb3, 0xac, 0x10,
    0x62, 0xcc, 0x61, 0x40, 0xf7, 0xfa, 0x52, 0x7f, 0xff, 0x32, 0x45, 0x20, 0x79, 0xce, 0xea, 0xbe,
    0xcd, 0x15, 0x21, 0x23, 0xd8, 0xb6, 0x0c, 0x3f, 0x54, 0x1a, 0xbf, 0x98, 0x48, 0x3a, 0x75, 0x77,
    0x2b, 0xae, 0x36, 0xda, 0x7e, 0x86, 0x35, 0x51, 0x05, 0x12, 0xb8, 0xa6, 0x9a, 0x2c, 0x06, 0x4b};

pthread_mutex_t g_threadcount_lock; /* Lock for g_threadcount. */
uint32_t g_threadcount = 1;         /* Number of running threads. */
uint32_t g_numproc = 0;             /* Number of running logical processors. */
ttable g_target[8];                 /* Truth tables for the output bits of the sbox. */

pthread_attr_t g_thread_attr; /* Attributes for threads spawned by create_circuit. */

/* Prints a truth table to the console. Used for debugging. */
void print_ttable(ttable tbl) {
  uint64_t vec[4];
  _mm256_storeu_si256((ttable*)vec, tbl);
  uint64_t *var = &vec[0];
  for (uint16_t i = 0; i < 256; i++) {
    if (i == 64) {
      var = &vec[1];
    } else if (i == 128) {
      var = &vec[2];
    } else if (i == 192) {
      var = &vec[3];
    }
    if (i != 0 && i % 16 == 0) {
      printf("\n");
    }
    printf("%" PRIu64, (*var >> (i % 64)) & 1);
  }
  printf("\n");
}

/* Test two truth tables for equality. */
static inline bool ttable_equals(const ttable in1, const ttable in2) {
  ttable res = in1 ^ in2;
  return _mm256_testz_si256(res, res);
}

/* Performs a masked test for equality. Only bits set to 1 in the mask will be tested. */
static inline bool ttable_equals_mask(const ttable in1, const ttable in2, const ttable mask) {
  ttable res = (in1 ^ in2) & mask;
  return _mm256_testz_si256(res, res);
}

/* Adds a gate to the state st. Returns the gate id of the added gate. */
static inline uint64_t add_gate(state *st, gate_type type, ttable table, uint64_t gid1,
    uint64_t gid2) {
  if (gid1 == NO_GATE || (gid2 == NO_GATE && type != NOT)) {
    return NO_GATE;
  }
  assert(type != IN);
  assert(gid1 < st->num_gates);
  assert(gid2 < st->num_gates || type == NOT);
  if (st->num_gates >= st->max_gates) {
    return NO_GATE;
  }
  st->gates[st->num_gates].type = type;
  st->gates[st->num_gates].table = table;
  st->gates[st->num_gates].in1 = gid1;
  st->gates[st->num_gates].in2 = gid2;
  st->num_gates += 1;
  return st->num_gates - 1;
}

/* The functions below are all calls to add_gate above added to improve code readability. */

static inline uint64_t add_not_gate(state *st, uint64_t gid) {
  return add_gate(st, NOT, ~st->gates[gid].table, gid, NO_GATE);
}

static inline uint64_t add_and_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_gate(st, AND, st->gates[gid1].table & st->gates[gid2].table, gid1, gid2);
}

static inline uint64_t add_or_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_gate(st, OR, st->gates[gid1].table | st->gates[gid2].table, gid1, gid2);
}

static inline uint64_t add_xor_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_gate(st, XOR, st->gates[gid1].table ^ st->gates[gid2].table, gid1, gid2);
}

static inline uint64_t add_nand_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_not_gate(st, add_and_gate(st, gid1, gid2));
}

static inline uint64_t add_nor_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_not_gate(st, add_or_gate(st, gid1, gid2));
}

static inline uint64_t add_xnor_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_not_gate(st, add_xor_gate(st, gid1, gid2));
}

static inline uint64_t add_or_not_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_or_gate(st, add_not_gate(st, gid1), gid2);
}

static inline uint64_t add_and_not_gate(state *st, uint64_t gid1, uint64_t gid2) {
  return add_and_gate(st, add_not_gate(st, gid1), gid2);
}

static inline uint64_t add_or_3_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_or_gate(st, add_or_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_and_3_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_and_gate(st, add_and_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_xor_3_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_xor_gate(st, add_xor_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_and_or_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_or_gate(st, add_and_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_and_xor_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_xor_gate(st, add_and_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_xor_or_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_or_gate(st, add_xor_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_xor_and_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_and_gate(st, add_xor_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_or_and_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_and_gate(st, add_or_gate(st, gid1, gid2), gid3);
}

static inline uint64_t add_or_xor_gate(state *st, uint64_t gid1, uint64_t gid2, uint64_t gid3) {
  return add_xor_gate(st, add_or_gate(st, gid1, gid2), gid3);
}

static uint64_t create_circuit(state *st, const ttable target, const ttable mask,
    const int8_t *inbits);

static uint64_t create_circuit(state *st, const ttable target, const ttable mask,
    const int8_t *inbits) {

  /* 1. Look through the existing circuit. If there is a gate that produces the desired map, simply
     return the ID of that gate. */

  for (uint64_t i = 0; i < st->num_gates; i++) {
    if (ttable_equals_mask(target, st->gates[i].table, mask)) {
      return i;
    }
  }

  /* 2. If there are any gates whose inverse produces the desired map, append a NOT gate, and
     return the ID of the NOT gate. */

  for (uint64_t i = 0; i < st->num_gates; i++) {
    if (ttable_equals_mask(target, ~st->gates[i].table, mask)) {
      return add_not_gate(st, i);
    }
  }

  /* 3. Look at all pairs of gates in the existing circuit. If they can be combined with a single
     gate to produce the desired map, add that single gate and return its ID. */

  const ttable mtarget = target & mask;
  for (uint64_t i = 0; i < st->num_gates; i++) {
    ttable ti = st->gates[i].table & mask;
    for (uint64_t k = i + 1; k < st->num_gates; k++) {
      ttable tk = st->gates[k].table & mask;
      if (ttable_equals(mtarget, ti | tk)) {
        return add_or_gate(st, i, k);
      }
      if (ttable_equals(mtarget, ti & tk)) {
        return add_and_gate(st, i, k);
      }
      if (ttable_equals(mtarget, ti ^ tk)) {
        return add_xor_gate(st, i, k);
      }
    }
  }

  /* 4. Look at all combinations of two or three gates in the circuit. If they can be combined with
     two gates to produce the desired map, add the gates, and return the ID of the one that produces
     the desired map. */

  for (uint64_t i = 0; i < st->num_gates; i++) {
    ttable ti = st->gates[i].table;
    for (uint64_t k = i + 1; k < st->num_gates; k++) {
      ttable tk = st->gates[k].table;
      if (ttable_equals_mask(target, ~(ti | tk), mask)) {
        return add_nor_gate(st, i, k);
      }
      if (ttable_equals_mask(target, ~(ti & tk), mask)) {
        return add_nand_gate(st, i, k);
      }
      if (ttable_equals_mask(target, ~(ti ^ tk), mask)) {
        return add_xnor_gate(st, i, k);
      }
      if (ttable_equals_mask(target, ~ti | tk, mask)) {
        return add_or_not_gate(st, i, k);
      }
      if (ttable_equals_mask(target, ~tk | ti, mask)) {
        return add_or_not_gate(st, k, i);
      }
      if (ttable_equals_mask(target, ~ti & tk, mask)) {
        return add_and_not_gate(st, i, k);
      }
      if (ttable_equals_mask(target, ~tk & ti, mask)) {
        return add_and_not_gate(st, k, i);
      }
    }
  }

  for (uint64_t i = 0; i < st->num_gates; i++) {
    ttable ti = st->gates[i].table & mask;
    for (uint64_t k = i + 1; k < st->num_gates; k++) {
      ttable tk = st->gates[k].table & mask;
      ttable iandk = ti & tk;
      ttable iork = ti | tk;
      ttable ixork = ti ^ tk;
      for (uint64_t m = k + 1; m < st->num_gates; m++) {
        ttable tm = st->gates[m].table & mask;
        if (ttable_equals(mtarget, iandk & tm)) {
          return add_and_3_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, iandk | tm)) {
          return add_and_or_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, iandk ^ tm)) {
          return add_and_xor_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, iork | tm)) {
          return add_or_3_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, iork & tm)) {
          return add_or_and_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, iork ^ tm)) {
          return add_or_xor_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, ixork ^ tm)) {
          return add_xor_3_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, ixork | tm)) {
          return add_xor_or_gate(st, i, k, m);
        }
        if (ttable_equals(mtarget, ixork & tm)) {
          return add_xor_and_gate(st, i, k, m);
        }
        ttable iandm = ti & tm;
        if (ttable_equals(mtarget, iandm | tk)) {
          return add_and_or_gate(st, i, m, k);
        }
        if (ttable_equals(mtarget, iandm ^ tk)) {
          return add_and_xor_gate(st, i, m, k);
        }
        ttable kandm = tk & tm;
        if (ttable_equals(mtarget, kandm | ti)) {
          return add_and_or_gate(st, k, m, i);
        }
        if (ttable_equals(mtarget, kandm ^ ti)) {
          return add_and_xor_gate(st, k, m, i);
        }
        ttable ixorm = ti ^ tm;
        if (ttable_equals(mtarget, ixorm | tk)) {
          return add_xor_or_gate(st, i, m, k);
        }
        if (ttable_equals(mtarget, ixorm & tk)) {
          return add_xor_and_gate(st, i, m, k);
        }
        ttable kxorm = tk ^ tm;
        if (ttable_equals(mtarget, kxorm | ti)) {
          return add_xor_or_gate(st, k, m, i);
        }
        if (ttable_equals(mtarget, kxorm & ti)) {
          return add_xor_and_gate(st, k, m, i);
        }
        ttable iorm = ti | tm;
        if (ttable_equals(mtarget, iorm & tk)) {
          return add_or_and_gate(st, i, m, k);
        }
        if (ttable_equals(mtarget, iorm ^ tk)) {
          return add_or_xor_gate(st, i, m, k);
        }
        ttable korm = tk | tm;
        if (ttable_equals(mtarget, korm & ti)) {
          return add_or_and_gate(st, k, m, i);
        }
        if (ttable_equals(mtarget, korm ^ ti)) {
          return add_or_xor_gate(st, k, m, i);
        }
      }
    }
  }

  /* 5. Use the specified input bit to select between two Karnaugh maps. Call this function
     recursively to generate those two maps. */

  /* Copy input bits already used to new array to avoid modifying the old one. */
  int8_t next_inbits[8];
  uint8_t bitp = 0;
  while (bitp < 6 && inbits[bitp] != -1) {
    next_inbits[bitp] = inbits[bitp];
    bitp += 1;
  }
  assert(bitp < 6);
  next_inbits[bitp] = -1;
  next_inbits[bitp + 1] = -1;

  state best;
  best.num_gates = 0;

  /* Try all input bit orders. */
  for (int8_t bit = 0; bit < 8; bit++) {
    /* Check if the current bit number has already been used for selection. */
    bool skip = false;
    for (uint8_t i = 0; i < bitp; i++) {
      if (inbits[i] == bit) {
        skip = true;
        break;
      }
    }
    if (skip) {
      continue;
    }


    next_inbits[bitp] = bit;
    const ttable fsel = st->gates[bit].table; /* Selection bit. */

    state nst_and = *st; /* New state using AND multiplexer. */
    uint64_t fb = create_circuit(&nst_and, target & ~fsel, mask & ~fsel, next_inbits);
    uint64_t mux_out_and = NO_GATE;
    if (fb != NO_GATE) {
      uint64_t fc = create_circuit(&nst_and, nst_and.gates[fb].table ^ target, mask & fsel,
          next_inbits);
      uint64_t andg = add_and_gate(&nst_and, fc, bit);
      mux_out_and = add_xor_gate(&nst_and, fb, andg);
    }

    state nst_or  = *st; /* New state using OR multiplexer. */
    uint64_t fd = create_circuit(&nst_or, ~target & fsel, mask & fsel, next_inbits);
    uint64_t mux_out_or;
    if (fd != NO_GATE) {
      uint64_t fe = create_circuit(&nst_or, nst_or.gates[fd].table ^ target, mask & ~fsel,
          next_inbits);
      uint64_t org = add_or_gate(&nst_or, fe, bit);
      mux_out_or = add_xor_gate(&nst_or, fd, org);
    }

    if (mux_out_and == NO_GATE && mux_out_or == NO_GATE) {
      continue;
    }
    uint64_t mux_out;
    state nst;
    if (mux_out_or == NO_GATE || (mux_out_and != NO_GATE && nst_and.num_gates < nst_or.num_gates)) {
      nst = nst_and;
      mux_out = mux_out_and;
    } else {
      nst = nst_or;
      mux_out = mux_out_or;
    }
    assert(ttable_equals_mask(target, nst.gates[mux_out].table, mask));
    if (best.num_gates == 0 || nst.num_gates < best.num_gates) {
      best = nst;
    }
  }
  if (best.num_gates == 0) {
    return NO_GATE;
  }
  *st = best;
  uint64_t ret = best.num_gates - 1;
  return ret;
}

/* If sbox is true, a target truth table for the given bit of the sbox is generated.
   If sbox is false, the truth table of the given input bit is generated. */
static ttable generate_target(uint8_t bit, bool sbox) {
  assert(bit < 8);
  uint64_t vec[] = {0, 0, 0, 0};
  uint64_t *var = &vec[0];
  for (uint16_t i = 0; i < 256; i++) {
    if (i == 64) {
      var = &vec[1];
    } else if (i == 128) {
      var = &vec[2];
    } else if (i == 192) {
      var = &vec[3];
    }
    *var >>= 1;
    *var |= (uint64_t)(((sbox ? g_sbox_enc[i] : i) >> bit) & 1) << 63;
  }
  return _mm256_loadu_si256((ttable*)vec);
}

/* Prints the given state gate network to stdout in Graphviz dot format. */
void print_digraph(state st) {
  printf("digraph sbox {\n");
    for (uint64_t gt = 0; gt < st.num_gates; gt++) {
      char *gatename;
      char buf[10];
      switch (st.gates[gt].type) {
        case IN:
          gatename = buf;
          sprintf(buf, "IN %" PRIu64, gt);
          break;
        case NOT:
          gatename = "NOT";
          break;
        case AND:
          gatename = "AND";
          break;
        case OR:
          gatename = "OR";
          break;
        case XOR:
          gatename = "XOR";
          break;
      }
      printf("  gt%" PRIu64 " [label=\"%s\"];\n", gt, gatename);
    }
    for (uint64_t gt = 0; gt < st.num_gates; gt++) {
      if (st.gates[gt].in1 != NO_GATE) {
        printf("  gt%" PRIu64 " -> gt%" PRIu64 ";\n", st.gates[gt].in1, gt);
      }
      if (st.gates[gt].in2 != NO_GATE) {
        printf("  gt%" PRIu64 " -> gt%" PRIu64 ";\n", st.gates[gt].in2, gt);
      }
    }
    for (uint8_t i = 0; i < 8; i++) {
      if (st.outputs[i] != NO_GATE) {
        printf("  gt%" PRIu64 " -> out%" PRIu32 ";\n", st.outputs[i], i);
      }
    }
  printf("}\n");
}

/* Saves a state struct to file. */
static void save_state(const char *name, state st) {
  FILE *fp = fopen(name, "w");
  if (fp == NULL) {
    fprintf(stderr, "Error opening file for writing.\n");
    return;
  }
  if (fwrite(&st, sizeof(state), 1, fp) != 1) {
    fprintf(stderr, "File write error.\n");
  }
  fclose(fp);
}

int main(int argc, char **argv) {

  pthread_attr_init(&g_thread_attr);
  size_t stacksize;
  pthread_attr_getstacksize(&g_thread_attr, &stacksize);
  if (stacksize < MIN_STACK_SIZE) {
    pthread_attr_setstacksize(&g_thread_attr, MIN_STACK_SIZE);
  }

  /* Initialize mutex. */
  if (pthread_mutex_init(&g_threadcount_lock, NULL) != 0) {
    fprintf(stderr, "Error initializing mutexes.\n");
    return 1;
  }

  /* Generate truth tables for all output bits of the target sbox. */
  for (uint8_t i = 0; i < 8; i++) {
    g_target[i] = generate_target(i, true);
  }

  state default_state;

  if (argc == 1) {
    printf("No command line arguments - generating 1 output circuits.\n");
    /* Generate the eight input bits. */
    default_state.max_gates = MAX_GATES;
    default_state.num_gates = 8;
    memset(default_state.gates, 0, sizeof(gate) * MAX_GATES);
    for (uint8_t i = 0; i < 8; i++) {
      default_state.gates[i].type = IN;
      default_state.gates[i].table = generate_target(i, false);
      default_state.gates[i].in1 = NO_GATE;
      default_state.gates[i].in2 = NO_GATE;
      default_state.outputs[i] = NO_GATE;
    }
  } else if (argc == 2 || (argc == 3 && strcmp(argv[1], "-dot") == 0)) {
    FILE *fp = fopen(argv[argc - 1], "r");
    if (fp == NULL) {
      fprintf(stderr, "Error opening file: %s\n", argv[argc - 1]);
      return 1;
    }
    if (fread(&default_state, sizeof(state), 1, fp) != 1) {
      fprintf(stderr, "Error reading file: %s\n", argv[argc - 1]);
      fclose(fp);
      return 1;
    }
    fclose(fp);
    if (argc == 3) {
      print_digraph(default_state);
      return 0;
    }
    printf("Loaded state from %s\n", argv[1]);
    default_state.max_gates = MAX_GATES;
  } else {
    fprintf(stderr, "Illegal arguments. Exiting!\n");
    return 1;
  }

  g_numproc = sysconf(_SC_NPROCESSORS_ONLN);
  printf("%" PRIu32 " processors online.\n", g_numproc);

  const ttable mask = {(uint64_t)-1, (uint64_t)-1, (uint64_t)-1, (uint64_t)-1};
  for (uint8_t output = 0; output < 8; output++) {
    if (default_state.outputs[output] != NO_GATE) {
      printf("Skipping output %d.\n", output);
      continue;
    }
    printf("Generating circuit for output %d...\n", output);
    int8_t bits[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    state st = default_state;
    st.outputs[output] = create_circuit(&st, g_target[output], mask, bits);
    if (st.outputs[output] == NO_GATE) {
      printf("No solution for output %d.\n", output);
      continue;
    }
    assert(ttable_equals(g_target[output], st.gates[st.outputs[output]].table));
    uint8_t num_outputs = 0;
    for (uint8_t i = 0; i < 8; i++) {
      if (st.outputs[i] != NO_GATE) {
        num_outputs += 1;
      }
    }
    char out[9];
    memset(out, 0, 9);
    for (uint8_t i = 0; i < 8; i++) {
      if (st.outputs[i] != NO_GATE) {
        char str[2] = {'0' + i, '\0'};
        strcat(out, str);
      }
    }

    char fname[30];
    sprintf(fname, "%d-%03" PRIu64 "-%s.state", num_outputs, st.num_gates - 7, out);
    save_state(fname, st);
    if (default_state.max_gates > st.num_gates) {
      default_state.max_gates = st.num_gates;
      printf("New max gates: %llu\n", default_state.max_gates);
    }
  }

  pthread_mutex_destroy(&g_threadcount_lock);

  return 0;
}
