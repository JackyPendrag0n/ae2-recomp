#ifndef SPU2_H
#define SPU2_H
#include <stdint.h>
void spu2_init(int headless);
void spu2_render(int16_t *out, int frames);
void spu2_set_cb_hook(void (*h)(int chan, uint32_t func));
void sd_init(int flag);
void sd_set_param(uint32_t entry, uint32_t value);
uint32_t sd_get_param(uint32_t entry);
void sd_set_switch(uint32_t entry, uint32_t value);
uint32_t sd_get_switch(uint32_t entry);
void sd_set_addr(uint32_t entry, uint32_t value);
uint32_t sd_get_addr(uint32_t entry);
void sd_set_core_attr(uint32_t entry, uint32_t value);
int sd_set_effect_attr(int core, int mode, int16_t depth_l, int16_t depth_r, int delay, int feedback);
int sd_voice_trans(int chan, uint32_t mode, uint32_t iopaddr, uint32_t spuaddr, uint32_t size);
int sd_voice_trans_status(int chan, int flag);
uint32_t sd_block_trans(int chan, uint32_t mode, uint32_t iopaddr, uint32_t size, uint32_t start);
uint32_t sd_block_trans_status(int chan, int flag);
uint32_t sd_set_trans_callback(int chan, uint32_t func);
extern uint64_t g_spu_samples;
#endif
