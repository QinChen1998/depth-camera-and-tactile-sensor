#include "TactileFrame.h"
#include <cmath>
namespace tactile {
const Layout &sensorLayout(int type)
{
    static const QVector<Layout> layouts = [] {
        QVector<Layout> result;
        for(int type=0;type<3;++type) {
            Layout l;
            l.fingerIds={"fingertip","terminal","ultrathin"};
            const int rows=type==2?4:6;
            int id=0;
            for(int row=0;row<rows;++row) {
                const int columns=type!=2&&(row==0||row==rows-1)?2:4;
                for(int col=0;col<columns;++col)
                    l.points.append({"M1",QString("P%1").arg(++id),
                        QPointF(.5+(col-(columns-1)/2.0)*.16, .5+(row-(rows-1)/2.0)*(type==2?.16:.144))});
            }
            result.append(l);
        }
        return result;
    }();
    return layouts.at(type);
}
static bool finite(const Force &v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool sameForce(const Force &a, const Force &b)
{
    const auto near=[](double x,double y) { return std::abs(x-y)<=1e-9*(1+std::max(std::abs(x),std::abs(y))); };
    return finite(a)&&finite(b)&&near(a.x,b.x)&&near(a.y,b.y)&&near(a.z,b.z);
}
bool validateFrame(const Frame &f, QString *error)
{
    const auto fail=[&](const char *s) { if(error) *error=QString::fromLatin1(s); return false; };
    const auto &l=sensorLayout();
    if(!f.valid || f.schemaVersion!=1 || f.layoutVersion!=l.version
       || f.sourceMode!="Serial" || f.layoutId!=l.id || f.hardwareType<0 || f.hardwareType>2 || (f.crcVerified&&!f.crcOk)
       || f.sessionId.isNull() || !f.generation || !f.sequence || f.monotonicNs<0)
        return fail("Invalid frame header");
    if(f.fingers.size()!=l.fingerIds.size()) return fail("Invalid finger count");
    for(int i=0;i<f.fingers.size();++i) {
        const auto &finger=f.fingers[i];
        if(i!=f.hardwareType) {
            if(finger.valid||finger.id!=l.fingerIds[i]||!finger.modules.isEmpty()||!sameForce(finger.resultant,{}))return fail("Unexpected hardware slot");
            continue;
        }
        if(!finger.valid || finger.id!=l.fingerIds[i] || finger.modules.size()!=1) return fail("Invalid finger identity/count/state");
        Force sum;
        for(int m=0;m<1;++m) {
            const auto &module=finger.modules[m];
            if(!module.valid || module.id!="M1" || module.points.size()!=sensorLayout(i).points.size()) return fail("Invalid module identity/count/state");
            Force points;
            for(int p=0;p<sensorLayout(i).points.size();++p) {
                const auto &point=module.points[p];
                if(!point.valid || point.id!=sensorLayout(i).points[p].pointId || !finite(point.force) || point.force.z<0)
                    return fail("Invalid point identity/force/state");
                if(point.force.x<-32768||point.force.x>32767||point.force.y<-32768||point.force.y>32767||point.force.z>65535
                    || std::trunc(point.force.x)!=point.force.x||std::trunc(point.force.y)!=point.force.y||std::trunc(point.force.z)!=point.force.z)return fail("Invalid raw word domain");
                points+=point.force;
            }
            if(!sameForce(points,module.resultant)) return fail("Invalid module resultant");
            sum+=module.resultant;
        }
        if(!sameForce(sum,finger.resultant)) return fail("Invalid finger resultant");
    }
    if(error) error->clear();
    return true;
}
void FrameGate::reset(const QUuid &s, quint64 g) { m_session=s; m_generation=g; m_sequence=0; m_time=-1; }
bool FrameGate::accept(const Frame &f, QString *error)
{
    if(!validateFrame(f,error)) return false;
    if(f.sessionId!=m_session || f.generation!=m_generation || f.sequence<=m_sequence || f.monotonicNs<=m_time) {
        if(error) *error="Stale frame identity or time";
        return false;
    }
    m_sequence=f.sequence; m_time=f.monotonicNs; return true;
}
}
