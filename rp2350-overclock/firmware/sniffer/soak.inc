// 大负载拷机：12 SM @600 MS/s + 12 DMA + 反复 arm/abort + 周期性 USB 上传
static float soak_temp(void) {
    adc_select_input(4);
    float v = adc_read() * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

static void soak_run(uint32_t sec, uint32_t dump_every) {
    uint32_t rounds = 0, short_cnt = 0;
    float tmax = 0.0f, tend = 0.0f;
    uint64_t end = time_us_64() + (uint64_t)sec * 1000000ull;
    uint32_t t_last = time_us_32(), t_prog = time_us_32();
    while (time_us_64() < end) {
        capture(400, 0);
        tud_task();
        rounds++;
        bool all_full = true;
        for (int i = 0; i < NCH; i++) if (got[i] < BUF_WORDS * 32u) all_full = false;
        if (!all_full) short_cnt++;
        if (dump_every && (rounds % dump_every) == 0) dump_ch(10);   // USB 压力
        if ((time_us_32() - t_last) > 500000u) {                     // 每 0.5 s 采一次温度
            t_last = time_us_32();
            float t = soak_temp();
            if (t > tmax) tmax = t;
            tend = t;
            watchdog_update();
            if ((time_us_32() - t_prog) > 30000000u) {   // 每 30 s 报一次进度
                t_prog = time_us_32();
                printf("P %lu %lu %.1f\n", (unsigned long)(rounds), (unsigned long)short_cnt, (double)t);
            }
        }
    }
    watchdog_hw->scratch[0] = 0x50AC0000u | (short_cnt & 0xFFFFu);   // 结果落地：CDC 挂了也能在下次开机读到
    watchdog_hw->scratch[1] = rounds;
    watchdog_hw->scratch[2] = (uint32_t)(tmax * 10.0f);
    watchdog_hw->scratch[3] = (uint32_t)(tend * 10.0f);
    printf("SOAK %lu %lu %lu %.1f %.1f %s\n",
           (unsigned long)sec, (unsigned long)rounds, (unsigned long)short_cnt,
           (double)tmax, (double)tend, short_cnt ? "SHORT" : "OK");
}
