#pragma once
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>

// Immutable copy of one displayed packet, before opening any modal file dialog.
struct DepthSnapshot {
    QByteArray metresFloat32LE;
    QImage depthImage, cameraImage;
    QJsonObject metadata;
    QDateTime capturedUtc;

    bool save(const QString &parent, bool withCamera, QString *result) const {
        auto fail=[&](const QString &message){if(result)*result=message;return false;};
        if(depthImage.isNull() || metresFloat32LE.size()!=depthImage.width()*depthImage.height()*4)
            return fail(QStringLiteral("No current depth frame"));
        if(withCamera && cameraImage.isNull())return fail(QStringLiteral("No camera image paired with this depth frame"));
        QTemporaryDir staging(QDir(parent).filePath(".depth-save-XXXXXX"));
        if(!staging.isValid())return fail(QStringLiteral("Cannot create capture directory"));
        const QString stamp=capturedUtc.toString("yyyyMMdd_HHmmss_zzz'Z'");
        auto write=[&](const QString &name,const QByteArray &data){QSaveFile f(staging.filePath(name));return f.open(QIODevice::WriteOnly)&&f.write(data)==data.size()&&f.commit();};
        auto png=[&](const QString &name,const QImage &image){QSaveFile f(staging.filePath(name));return f.open(QIODevice::WriteOnly)&&image.save(&f,"PNG")&&f.commit();};
        QJsonObject info=metadata;
        info["saved_utc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        info["snapshot_utc"]=capturedUtc.toString(Qt::ISODateWithMs);
        info["depth_file"]="depth_"+stamp+".f32";
        info["depth_encoding"]="row-major float32 little-endian, metres";
        info["width"]=depthImage.width();info["height"]=depthImage.height();
        info["camera_saved"]=withCamera;
        if(withCamera){info["camera_file"]="camera_"+stamp+".png";info["camera_width"]=cameraImage.width();info["camera_height"]=cameraImage.height();}
        if(!write("depth_"+stamp+".f32",metresFloat32LE)||!png("depth_"+stamp+".png",depthImage)
           ||(withCamera&&!png("camera_"+stamp+".png",cameraImage))
           ||!write("metadata_"+stamp+".json",QJsonDocument(info).toJson()))return fail(QStringLiteral("Failed to write capture files"));
        // Publish the complete set together. Repeated clicks never overwrite a capture.
        QString destination=QDir(parent).filePath("capture_"+stamp);
        for(int i=1;QFileInfo::exists(destination);++i)destination=QDir(parent).filePath("capture_"+stamp+QString("_%1").arg(i));
        if(!QDir().rename(staging.path(),destination))return fail(QStringLiteral("Cannot publish capture directory"));
        staging.setAutoRemove(false);if(result)*result=destination;return true;
    }
};
