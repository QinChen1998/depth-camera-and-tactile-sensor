#include "TactileSurfaceModel.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <cmath>

namespace {
bool vector(const QJsonValue &value,QVector3D &target) {
    const auto a=value.toArray();
    if(a.size()!=3)return false;
    for(const auto &v:a)if(!v.isDouble()||!std::isfinite(v.toDouble())||std::abs(v.toDouble())>1)return false;
    target=QVector3D(float(a[0].toDouble()),float(a[1].toDouble()),float(a[2].toDouble()));return true;
}
std::array<TactileSurfaceModel,3> load() {
    std::array<TactileSurfaceModel,3> models{};
    QFile file(":/tactile/surfaces.json");
    if(!file.open(QIODevice::ReadOnly))return models;
    const auto doc=QJsonDocument::fromJson(file.readAll()).object();
    const auto sensors=doc["sensors"].toArray();
    if(doc["version"].toInt()!=1||doc["units"].toString()!="m"||sensors.size()!=3)return models;
    const QStringList ids={"fingertip","terminal","ultrathin"};
    for(int i=0;i<3;++i) {
        const auto s=sensors[i].toObject();auto &model=models[i];
        const QString path=":/tactile/"+ids[i]+".mesh";
        QFile mesh(path);
        if(s["id"].toString()!=ids[i]||!mesh.open(QIODevice::ReadOnly))continue;
        if(QCryptographicHash::hash(mesh.readAll(),QCryptographicHash::Sha256).toHex()!=s["meshSha256"].toString().toLatin1())continue;
        model.mesh=QUrl("qrc"+path);
        if(!vector(s["meshPositionM"],model.meshPosition)||!vector(s["boundsCenterM"],model.boundsCenter)
            ||!vector(s["boundsSizeM"],model.boundsSize)||!vector(s["resultantPositionM"],model.resultantPosition))continue;
        if(model.boundsSize.x()<=0||model.boundsSize.y()<=0||model.boundsSize.z()<=0)continue;
        const auto q=s["meshRotation"].toArray();bool finite=q.size()==4;
        for(const auto &v:q)finite &= v.isDouble()&&std::isfinite(v.toDouble());
        if(!finite)continue;
        model.meshRotation=QQuaternion(float(q[0].toDouble()),float(q[1].toDouble()),float(q[2].toDouble()),float(q[3].toDouble()));
        if(std::abs(model.meshRotation.lengthSquared()-1)>1e-5)continue;
        const auto points=s["points"].toArray();
        if(points.size()!=(i==2?16:20))continue;
        bool okay=true;
        for(int p=0;p<points.size();++p) {
            const auto point=points[p].toObject();QVector3D position,normal;
            if(point["id"].toString()!=QString("P%1").arg(p+1)||!vector(point["positionM"],position)
                ||!vector(point["normal"],normal)||std::abs(normal.lengthSquared()-1)>1e-4||normal.z()<.2){okay=false;break;}
            model.positions.append(position);model.normals.append(normal);
        }
        const auto levels=s["interpolation"].toArray();
        okay &= levels.size()==2;
        for(int level=0;okay&&level<2;++level) {
            const auto entry=levels[level].toObject();
            const auto extras=entry["points"].toArray();
            const int total=i==2?(level==0?49:100):(level==0?65:136);
            if(entry["subdivisions"].toInt()!=level+2||extras.size()+points.size()!=total){okay=false;break;}
            for(const auto &value:extras) {
                const auto point=value.toObject();TactileInterpolatedPoint p;
                const auto sources=point["sources"].toArray(),weights=point["weights"].toArray();
                if(!vector(point["positionM"],p.position)||sources.size()<3||sources.size()>4||sources.size()!=weights.size()){okay=false;break;}
                double sum=0;QVector3D weighted;
                for(int j=0;j<sources.size();++j) {
                    const int index=sources[j].toInt(-1);const double weight=weights[j].toDouble(-1);
                    if(index<0||index>=points.size()||p.sources.contains(index)||!std::isfinite(weight)||weight<0||weight>1){okay=false;break;}
                    p.sources.append(index);p.weights.append(weight);sum+=weight;weighted+=model.positions[index]*float(weight);
                }
                if(!okay||std::abs(sum-1)>1e-6||std::abs(weighted.x()-p.position.x())>1e-6||std::abs(weighted.y()-p.position.y())>1e-6){okay=false;break;}
                model.interpolation[level].append(p);
            }
        }
        model.valid=okay;
    }
    return models;
}
}
const TactileSurfaceModel &tactileSurfaceModel(int type) {
    static const auto models=load();
    static const TactileSurfaceModel invalid;
    return type>=0&&type<3?models[type]:invalid;
}
int tactileDisplayPointCount(int type,int gridSize) {
    if(type<0||type>=3)return 0;
    const int raw=tactile::sensorLayout(type).points.size();
    const auto &surface=tactileSurfaceModel(type);
    return raw+(surface.valid&&gridSize>=3&&gridSize<=4?surface.interpolation[gridSize-3].size():0);
}
namespace {
const tactile::Force &forceValue(const tactile::Force &value) { return value; }
const tactile::Force &forceValue(const tactile::Point &value) { return value.force; }
template<class Values> tactile::Force samplePoint(const Values &raw,int type,int gridSize,int point) {
    if(type<0||type>=3||raw.size()!=tactile::sensorLayout(type).points.size()
        ||point<0||point>=tactileDisplayPointCount(type,gridSize))return {};
    if(point<raw.size())return forceValue(raw[point]);
    const auto &sample=tactileSurfaceModel(type).interpolation[gridSize-3][point-raw.size()];
    tactile::Force result;
    for(int i=0;i<sample.sources.size();++i){
        const auto &force=forceValue(raw[sample.sources[i]]);const double weight=sample.weights[i];
        result.x+=force.x*weight;result.y+=force.y*weight;result.z+=force.z*weight;
    }
    return result;
}
}
tactile::Force sampleTactileDisplayPoint(const QVector<tactile::Force> &raw,int type,int gridSize,int point) {
    return samplePoint(raw,type,gridSize,point);
}
tactile::Force sampleTactileDisplayPoint(const QVector<tactile::Point> &raw,int type,int gridSize,int point) {
    return samplePoint(raw,type,gridSize,point);
}
