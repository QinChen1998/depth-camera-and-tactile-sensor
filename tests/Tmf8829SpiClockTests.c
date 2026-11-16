#include "tmf8829_spi_clock.h"

#include <assert.h>
#include <stdio.h>

static const uint64_t host_origin = UINT64_C(4200000000);
static const uint64_t host_step = 100640;
static const uint32_t sensor_step = 12500;
static const uint16_t fitted = 32978;

static void observe_step(Tmf8829SpiClock *clock, uint32_t sensor_origin, unsigned i)
{
    const uint64_t host_us = host_origin + host_step * i;
    tmf8829SpiClockObserve(clock, host_us, host_us + 105, sensor_origin + sensor_step * i);
}

static void learns_over_a_second_and_latches_a_full_frame(void)
{
    Tmf8829SpiClock clock;
    tmf8829SpiClockReset(&clock);
    for (unsigned i = 0; i < 10; ++i)
        observe_step(&clock, 1, i);
    assert(clock.estimate_q15 == TMF_SPI_CLOCK_RATIO_ONE);
    assert(tmf8829SpiClockFrameRatio(&clock, 0) == TMF_SPI_CLOCK_RATIO_ONE);
    observe_step(&clock, 1, 10);
    assert(clock.estimate_q15 == fitted);
    assert(tmf8829SpiClockFrameRatio(&clock, 1) == TMF_SPI_CLOCK_RATIO_ONE);
    assert(tmf8829SpiClockFrameRatio(&clock, 0) == fitted);
    assert(tmf8829SpiClockFrameRatio(&clock, 1) == fitted);
}

static void rejects_unreliable_samples(void)
{
    Tmf8829SpiClock clock;
    tmf8829SpiClockReset(&clock);
    for (unsigned i = 0; i <= 10; ++i)
        observe_step(&clock, 1, i);
    const unsigned count = clock.count;
    const uint64_t next = host_origin + host_step * 11;
    tmf8829SpiClockObserve(&clock, next, next + 7000, 1 + sensor_step * 11);
    tmf8829SpiClockObserve(&clock, next, next + 105, sensor_step * 11);
    tmf8829SpiClockObserve(&clock, next, next - 1, 1 + sensor_step * 11);
    tmf8829SpiClockObserve(&clock, next, next + 105, 1 + sensor_step * 10);
    tmf8829SpiClockObserve(&clock, host_origin, host_origin + 105, 1);
    assert(clock.count == count);
    assert(clock.estimate_q15 == fitted);
    observe_step(&clock, 1, 11);
    assert(clock.count == count + 1);
    assert(clock.estimate_q15 == fitted);
}

static void handles_sensor_wrap_and_resets_stale_history(void)
{
    Tmf8829SpiClock clock;
    tmf8829SpiClockReset(&clock);
    const uint32_t sensor_origin = UINT32_MAX - 60000;
    for (unsigned i = 0; i < 37; ++i)
        observe_step(&clock, sensor_origin, i);
    assert(clock.estimate_q15 == fitted);
    assert(clock.count == TMF_SPI_CLOCK_SAMPLES);

    observe_step(&clock, 101, 100);
    assert(clock.count == 1);
    assert(clock.estimate_q15 == fitted);
    tmf8829SpiClockReset(&clock);
    assert(clock.count == 0);
    assert(tmf8829SpiClockFrameRatio(&clock, 1) == TMF_SPI_CLOCK_RATIO_ONE);
}

int main(void)
{
    learns_over_a_second_and_latches_a_full_frame();
    rejects_unreliable_samples();
    handles_sensor_wrap_and_resets_stale_history();
    puts("SPI clock estimator tests passed");
    return 0;
}
