#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "SessionFov.h"
#include "DriverWarningPolicy.h"

int main(void)
{
    unsigned checks=0;
    for(unsigned sensor=0;sensor<256;++sensor){
        assert(dcts_processing_fov(sensor,0)==(sensor<16?(int)sensor:-1));++checks;
        assert(dcts_processing_fov(sensor,"")== (sensor<16?(int)sensor:-1));++checks;
        assert(dcts_processing_fov(sensor,"1")== (sensor<16?1:-1));++checks;
        assert(dcts_processing_fov(sensor,"2")== (sensor<16?2:-1));++checks;
        assert(dcts_processing_fov(sensor,"6")==-1);++checks;
        assert(dcts_processing_fov(sensor,"1;reboot")==-1);++checks;
    }
    assert(!dcts_valid_run_id(0) && !dcts_valid_run_id(""));++checks;
    assert(dcts_valid_run_id("dtof_0123456789abcdef0123456789abcdef"));++checks;
    assert(!dcts_valid_run_id("dtof_\"injected"));++checks;
    assert(!dcts_valid_run_id("dtof_\n"));++checks;
    char folder[]="/tmp/dcts-calibration-XXXXXX";
    assert(mkdtemp(folder));++checks;
    char path[256], receipt[280];
    snprintf(path,sizeof(path),"%s/readback",folder);
    snprintf(receipt,sizeof(receipt),"%s.calibration.json",path);
    assert(dcts_write_calibration(path,"dtof_test",6,1)==0);++checks;
    FILE *f=fopen(receipt,"r");char json[512]={0};assert(f);fread(json,1,sizeof(json)-1,f);fclose(f);
    assert(strstr(json,"\"sensor_fov_correction\":6") && strstr(json,"\"processing_fov_correction\":1") &&
           strstr(json,"\"temporary_calibration\":true"));++checks;
    assert(dcts_write_calibration(path,"dtof_test",1,1)==0);++checks;
    f=fopen(receipt,"r");memset(json,0,sizeof(json));assert(f);fread(json,1,sizeof(json)-1,f);fclose(f);
    assert(strstr(json,"\"temporary_calibration\":false"));++checks;
    assert(dcts_write_calibration(path,"unsafe\"run",6,1)==-1);++checks;
    remove(receipt);rmdir(folder);
    assert(tmf8829_observe08());++checks;
    for(unsigned warning=0;warning<256;++warning){
        assert(!tmf8829_warning_fatal(warning)==(warning==0u||warning==8u||warning==16u));++checks;
    }
    printf("PASS software FOV calibration and actual sensor receipt: %u assertions\n",checks);
    return 0;
}
