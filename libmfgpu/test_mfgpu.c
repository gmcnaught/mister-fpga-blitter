/*  test_mfgpu.c — libmfgpu smoke test (context + frame lifecycle). GPL-3.0. */
#include "mfgpu.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    static uint8_t ring[4096], heap[4096];
    blt_emitter_t e;
    blt_emitter_init(&e, ring, sizeof ring, heap, sizeof heap);
    mfgpu_t *m = mfgpu_create(&e);
    assert(m != NULL);
    mfgpu_frame_begin(m);
    mfgpu_batch_t empty = {0};
    assert(mfgpu_submit_batch(m, &empty) == 0);
    mfgpu_frame_end(m);
    mfgpu_destroy(m);
    printf("test_mfgpu smoke OK\n");
    return 0;
}
