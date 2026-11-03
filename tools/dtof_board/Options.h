#pragma once
#include "../dtof_m0/M0.h"
#include "../dtof_followup/V1Depth.h"
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace DtofBoard {
// Fixed-order numeric configuration, validated before SSH and again on device.
struct Options {
    DtofM0::Config metric;
    DtofV1::Settings visual;
    int palette=0;
    explicit Options(const std::string& csv="0.05,0.05,0.06,0.03,1,2,0.6,0.5,0.6,5,5,1,0") {
        if(csv.empty()||csv.size()>256||csv.find_first_not_of("0123456789.,")!=std::string::npos)throw std::invalid_argument("Invalid candidate options");
        std::vector<double> a;std::istringstream input(csv);std::string item;
        while(std::getline(input,item,',')){size_t used=0;double n=std::stod(item,&used);if(used!=item.size()||!std::isfinite(n))throw std::invalid_argument("Invalid candidate number");a.push_back(n);}
        if((a.size()!=13&&a.size()!=14&&a.size()!=15)||csv.back()==',')throw std::invalid_argument("Candidate options count");
        if(a.size()==13)a.push_back(0); // Original interpolation default.
        if(a.size()==14)a.push_back(0); // Original clients do not enable presentation smoothing.
        const double lo[]={.005,.005,0,.005,0,0,.1,.1,.1,1,0,0,0,0,0};
        const double hi[]={.2,.2,.2,.15,1,2,1.4,2,1,64,64,1,2,1,1};
        for(int i=0;i<15;++i)if(a[i]<lo[i]||a[i]>hi[i]||((i==4||i==5||(i>=9&&i<=13))&&std::floor(a[i])!=a[i]))throw std::invalid_argument("Candidate options range");
        metric.layerM=a[0];metric.changeM=a[1];metric.holeSpreadM=a[2];metric.sigmaM=a[3];metric.edgeMisses=int(a[4]);metric.innerMisses=int(a[5]);
        visual.nearM=a[6];visual.sigma=a[7];visual.currentWeight=a[8];visual.minimumComponent=int(a[9]);visual.fillHoles=int(a[10]);visual.bezier=a[11]!=0;palette=int(a[12]);
        visual.layerM=a[0];visual.spreadM=a[2];visual.sigmaM=a[3];
        visual.interpolation=int(a[13]);visual.presentation=a[14];
    }
};
}
