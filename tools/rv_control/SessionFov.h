#pragma once
#include <stdio.h>
#include <string.h>

// Select the ray table used for XYZ reconstruction, without writing the sensor.
// The independently read sensor value remains in the original readback.
static inline int dcts_processing_fov(unsigned sensor, const char *requested)
{
    if (sensor > 15) return -1;
    if (!requested || !*requested) return (int)sensor;
    return (requested[0]=='1' || requested[0]=='2') && requested[1]==0 ? requested[0]-'0' : -1;
}
static inline int dcts_valid_run_id(const char *value)
{
    if (!value || !*value || strlen(value)>80) return 0;
    for (const unsigned char *p=(const unsigned char *)value; *p; ++p)
        if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
              (*p>='0' && *p<='9') || *p=='_' || *p=='-')) return 0;
    return 1;
}
static inline int dcts_write_calibration(const char *readback, const char *run,
                                         unsigned sensor, unsigned processing)
{
    char path[256], tmp[260];
    if (!readback || !dcts_valid_run_id(run) || sensor>15 || processing>15 ||
        snprintf(path,sizeof(path),"%s.calibration.json",readback)>=(int)sizeof(path) ||
        snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp)) return -1;
    FILE *f=fopen(tmp,"w");
    if (!f) return -1;
    int written=fprintf(f,"{\"schema\":1,\"run_id\":\"%s\",\"sensor_fov_correction\":%u,"
        "\"processing_fov_correction\":%u,\"temporary_calibration\":%s}\n",
        run,sensor,processing,sensor!=processing?"true":"false");
    int closed=fclose(f);
    if (written<0 || closed!=0 || rename(tmp,path)!=0) { remove(tmp); return -1; }
    return 0;
}
