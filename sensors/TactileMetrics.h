#pragma once
#include <QJsonObject>
#include <QVector>
#include <algorithm>
#include <chrono>
namespace tactile {
inline double metricNowMs() { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct MetricStream {
    quint64 count=0, repaints=0, lastSerial=0;
    double lastMs=0;
    QVector<double> intervals, ages;
    void record(quint64 serial,double sampleMs) {
        ++repaints;
        if(!serial || serial==lastSerial) return;
        const double now=metricNowMs();
        if(lastMs && intervals.size()<65536) intervals.append(now-lastMs);
        if(ages.size()<65536) ages.append(std::max(0.0,now-sampleMs));
        lastMs=now;lastSerial=serial;++count;
    }
    static double percentile(QVector<double> values,double fraction) {
        if(values.isEmpty())return 0;std::sort(values.begin(),values.end());
        return values[qMin(values.size()-1,qsizetype(fraction*(values.size()-1)))];
    }
    QJsonObject json(double seconds) const {return {{"new_frames",double(count)},{"all_repaints",double(repaints)},
        {"new_frame_hz",count/seconds},{"interval_median_ms",percentile(intervals,.5)},
        {"interval_p95_ms",percentile(intervals,.95)},{"age_p95_ms",percentile(ages,.95)}};}
};
struct DisplayMetrics {
    bool enabled=false;
    MetricStream source, view, curve;
    double sampleMs=0;
    void reset() {source={};view={};curve={};enabled=true;}
};
}
