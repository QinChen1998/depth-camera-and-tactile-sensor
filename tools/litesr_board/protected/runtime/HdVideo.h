#pragma once
// Reuse the production VENC parser and PID/starttime checks without its daemon.
#define DEPTH_RTSP_MONITOR_NO_MAIN
#include <camera_rtsp_monitor.cpp>
#undef DEPTH_RTSP_MONITOR_NO_MAIN

namespace LiteSrHdVideo {
struct Observer {
    const std::string runtime;
    std::string start;
    pid_t pid=0;
    rtsp_camera::Progress progress;bool lastValid=true;
    const uint64_t started=rtsp_camera::monotonic();
    explicit Observer(const std::string& token):runtime("/run/dtof_candidates/hd_"+token){
        std::string text;uint64_t value=0;
        if(!rtsp_camera::read(runtime+"/encoder.pid",text,64)||!rtsp_camera::number(rtsp_camera::trim(text),value)||!value||value>2147483647)
            throw std::runtime_error("HD encoder PID is missing");
        pid=static_cast<pid_t>(value);
        if(!rtsp_camera::read(runtime+"/encoder.start",text,64)||!rtsp_camera::number(start=rtsp_camera::trim(text),value))
            throw std::runtime_error("HD encoder start identity is missing");
    }
    void poll(){
        const uint64_t now=rtsp_camera::monotonic();
        if(!rtsp_camera::owned(pid,start))throw std::runtime_error("HD encoder identity or exit failure");
        std::string mark,info;rtsp_camera::Counters counters;
        const bool guard=rtsp_camera::read(runtime+"/route_guard.active",mark,128)&&rtsp_camera::trim(mark)==std::to_string(pid);
        const bool valid=guard&&rtsp_camera::read("/proc/vcodec/enc/venc_info",info)&&rtsp_camera::parse(info,counters);
        if(!valid&&lastValid)std::cerr<<"HD_VENC_INVALID guard="<<guard<<"\n"<<info<<std::endl;
        lastValid=valid;
        if(valid&&progress.observe(counters,now)<0)throw std::runtime_error("HD encoder counter reset");
        if(now-(progress.ready?progress.observed_ns:started)>(progress.ready?3000000000ULL:10000000000ULL))
            throw std::runtime_error("HD encoder progress timeout");
    }
};
}
