#include <stddef.h>
/* chain_bs.h — chain_bs.c 的介面（產生物的手寫外殼） */
#ifndef CHAIN_BS_H
#define CHAIN_BS_H
#include <stdint.h>
typedef uint64_t vec_t;
#define VZERO ((vec_t)0)
#define VONES (~(vec_t)0)
struct chain_state_;
typedef struct chain_state_ chain_state_t;
#ifdef __cplusplus
extern "C" {
#endif
void chain_eval_cycle(chain_state_t *s, const chain_state_t *p,
                      vec_t rst, vec_t in_valid, const vec_t in_tag[6],
                      const vec_t in_lat[4], vec_t dn_credit_ret,
                      vec_t *in_ready, vec_t *out_valid, vec_t out_tag[6]);
size_t chain_state_size(void);
#ifdef __cplusplus
}
#endif
#endif
