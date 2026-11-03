#pragma once
#include <stdint.h>

/* PHQ1/PHC1: little endian; one request for an exact native ToF measurement.
 * Signal words retain the sensor's bit-15 compressed count encoding. */
#define RV_PHOTON_REQUEST_MAGIC 0x31514850u
#define RV_PHOTON_MAGIC 0x31434850u
#define RV_PHOTON_PORT 8833
#define RV_PHOTON_HEADER_BYTES 72
#define RV_PHOTON_MAX_PIXELS 1536
#define RV_PHOTON_MAX_AGE_NS 500000000ULL
#define RV_PHOTON_RING_SIZE 16
#define RV_PHOTON_IDLE_TIMEOUT_MS 1000u
#define RV_PHOTON_IO_TIMEOUT_MS 500u

static inline uint32_t rv_photon_count(uint16_t word) {
    return (word & 0x8000u) ? (uint32_t)(word & 0x7fffu) * 256u : word;
}

