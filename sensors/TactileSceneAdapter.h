#pragma once
#include "TactileFrame.h"
#include <cmath>
#include <QObject>
#include <QVariantList>
#include <QQuaternion>
#include <QVector3D>
#include <QPalette>
#include <QtQuick3D/QQuick3DInstancing>

// Interpolated glyphs share a few meshes instead of creating a QML scene per point.
class TactileInstanceBuffer final : public QQuick3DInstancing {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY instanceTableChanged)
public:
    explicit TactileInstanceBuffer(QObject *owner);
    int count() const { return m_count; }
    void setEntries(const QVector<InstanceTableEntry> &entries);
    static InstanceTableEntry entry(const QVector3D &position,const QVector3D &scale,
        const QQuaternion &rotation,const QColor &color);
protected:
    QByteArray getInstanceBuffer(int *count) override;
private:
    QByteArray m_buffer;
    int m_count=0;
};
// Read-only QML presentation snapshot. No controller, source, or motion authority.
class TactileSceneAdapter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap surface READ surface NOTIFY surfaceChanged)
    Q_PROPERTY(int rawPointCount READ rawPointCount NOTIFY surfaceChanged)
    Q_PROPERTY(bool lowCostRendering READ lowCostRendering WRITE setLowCostRendering NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList points READ points NOTIFY frameChanged)
    Q_PROPERTY(QVector3D dotBoundsMinimum READ dotBoundsMinimum NOTIFY frameChanged)
    Q_PROPERTY(QVector3D dotBoundsMaximum READ dotBoundsMaximum NOTIFY frameChanged)
    Q_PROPERTY(QVariantMap resultant READ resultant NOTIFY frameChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY frameChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY sourceLabelChanged)
    Q_PROPERTY(TactileInstanceBuffer *interpolatedPoints READ interpolatedPoints CONSTANT)
    Q_PROPERTY(TactileInstanceBuffer *interpolatedPointShafts READ interpolatedPointShafts CONSTANT)
    Q_PROPERTY(TactileInstanceBuffer *interpolatedPointHeads READ interpolatedPointHeads CONSTANT)
    Q_PROPERTY(TactileInstanceBuffer *interpolatedComponentShafts READ interpolatedComponentShafts CONSTANT)
    Q_PROPERTY(TactileInstanceBuffer *interpolatedComponentHeads READ interpolatedComponentHeads CONSTANT)
    Q_PROPERTY(bool communicationStatusVisible READ communicationStatusVisible NOTIFY optionsChanged)
    Q_PROPERTY(bool pointArrows READ pointArrows NOTIFY optionsChanged)
    Q_PROPERTY(bool resultantArrow READ resultantArrow NOTIFY optionsChanged)
    Q_PROPERTY(bool componentArrows READ componentArrows NOTIFY optionsChanged)
    Q_PROPERTY(bool reverseZArrows READ reverseZArrows NOTIFY optionsChanged)
    Q_PROPERTY(bool dotMatrix READ dotMatrix NOTIFY optionsChanged)
    Q_PROPERTY(double modelOpacity READ modelOpacity NOTIFY optionsChanged)
    Q_PROPERTY(double arrowWidthGain READ arrowWidthGain NOTIFY optionsChanged)
    Q_PROPERTY(double resultantSizeGain READ resultantSizeGain NOTIFY optionsChanged)
    Q_PROPERTY(double arrowGain READ arrowGain NOTIFY optionsChanged)
    Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
    Q_PROPERTY(QColor textColor READ textColor NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor border READ border NOTIFY themeChanged)
public:
    explicit TactileSceneAdapter(QObject *parent=nullptr);
    void publish(const tactile::Frame &,int finger,bool moduleMode,int module,const tactile::Frame *baseline=nullptr,double noiseThreshold=0);
    void invalidate(bool typeChanged=false);
    void setPalette(const QPalette &);
    void setPointArrows(bool value);
    void setResultantArrow(bool value);
    void setComponentArrows(bool value);
    void setReverseZArrows(bool value) { if(value!=m_reverseZArrows){m_reverseZArrows=value;emit optionsChanged();} }
    bool reverseZArrows() const { return m_reverseZArrows; }
    void setDotMatrix(bool value);
    bool dotMatrix() const { return m_dotMatrix; }
    void setModelOpacity(double value);
    void setLowCostRendering(bool value);
    bool lowCostRendering() const { return m_lowCostRendering; }
    void setArrowGain(double gain) { if(std::isfinite(gain)&&gain>=0.1&&gain<=100&&gain!=m_arrowGain){m_arrowGain=gain;emit optionsChanged();} }
    double arrowGain() const { return m_arrowGain; }
    void setArrowWidthGain(double gain);
    double arrowWidthGain() const { return m_arrowWidthGain; }
    void setResultantSizeGain(double gain);
    double resultantSizeGain() const { return m_resultantSizeGain; }
    int rawPointCount() const { return tactile::sensorLayout(m_type).points.size(); }
    double modelOpacity() const { return m_modelOpacity; }
    void setGridSize(int size);
    int gridSize() const { return m_gridSize; }
    QVariantMap surface() const;
    QVariantList points() const {return m_points;}
    QVector3D dotBoundsMinimum() const {return m_dotMinimum;}
    QVector3D dotBoundsMaximum() const {return m_dotMaximum;}
    QVariantMap resultant() const {return m_resultant;}
    bool valid() const {return m_valid;}
    QString sourceLabel() const { return m_sourceLabel; }
    void setSourceLabel(const QString &value) { if(m_sourceLabel!=value){m_sourceLabel=value;emit sourceLabelChanged();} }
    TactileInstanceBuffer *interpolatedPoints() const { return m_interpolatedPoints; }
    TactileInstanceBuffer *interpolatedPointShafts() const { return m_interpolatedPointShafts; }
    TactileInstanceBuffer *interpolatedPointHeads() const { return m_interpolatedPointHeads; }
    TactileInstanceBuffer *interpolatedComponentShafts() const { return m_interpolatedComponentShafts; }
    TactileInstanceBuffer *interpolatedComponentHeads() const { return m_interpolatedComponentHeads; }
    Q_INVOKABLE void setArrowMeshBounds(bool head,double bottom,double height);
    Q_INVOKABLE void selectRawPoint(int index) { if(m_valid&&index>=0&&index<rawPointCount())emit rawPointSelected(index); }
    bool communicationStatusVisible() const { return m_communicationStatusVisible; }
    void setCommunicationStatusVisible(bool value) { if(m_communicationStatusVisible!=value){m_communicationStatusVisible=value;emit optionsChanged();} }
    bool pointArrows() const {return m_pointArrows;}
    bool resultantArrow() const {return m_resultantArrow;}
    bool componentArrows() const {return m_componentArrows;}
    // Match HandView's display environment, independent of the Widgets page palette.
    QColor background() const {return QColor("#161b22");}
    QColor textColor() const {return QColor("#d2dce8");}
    QColor accent() const {return m_palette.color(QPalette::Highlight).lighter(145);}
    QColor border() const {return QColor("#455365");}
    static QVector3D pointPosition(int index, int type=0);
    static constexpr double PressureColorMaximum=2500;
    static QColor forceColor(double z);
    static QVariantMap arrow(const tactile::Force &,double scale,double maximum);
    static QVariantMap componentArrow(double input,int axis,double gain=1,bool reverseZ=false);
signals:
    void rawPointSelected(int index);
    void frameChanged();
    void sourceLabelChanged();
    void surfaceChanged();
    void themeChanged();
    void optionsChanged();
private:
    void rebuildPoints(const tactile::Finger *finger,int selectedModule);
    void rebuildInstances();
    TactileInstanceBuffer *m_interpolatedPoints,*m_interpolatedPointShafts,*m_interpolatedPointHeads;
    TactileInstanceBuffer *m_interpolatedComponentShafts,*m_interpolatedComponentHeads;
    double m_shaftBottom=-50,m_shaftHeight=100,m_headBottom=0,m_headHeight=100;
    int m_gridSize=2;
    int m_type=0;
    QVariantList m_points;
    QVector3D m_dotMinimum,m_dotMaximum;
    QVector<QQuick3DInstancing::InstanceTableEntry> m_spheres,m_pointShafts,m_pointHeads,m_componentShafts,m_componentHeads;
    QVariantMap m_resultant;
    QPalette m_palette;
    bool m_valid=false,m_pointArrows=false,m_resultantArrow=true;
    bool m_componentArrows=true;
    bool m_reverseZArrows=false;
    bool m_dotMatrix=false;
    double m_modelOpacity=0.45;
    double m_arrowGain=1;
    double m_arrowWidthGain=1;
    double m_resultantSizeGain=1;
    bool m_lowCostRendering=false;
    QString m_sourceLabel;
    bool m_communicationStatusVisible=true;
};
