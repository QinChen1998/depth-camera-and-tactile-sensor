#pragma once
#include "RvTunerController.h"
#include <QtWidgets/QWidget>
#include <QtCore/QJsonArray>
#include <QtCore/QMap>
class QComboBox;class QSpinBox;class QDoubleSpinBox;class QCheckBox;class QLabel;class QPushButton;class QTableWidget;class QSplitter;class RvTunerCanvas;
class RvTunerWidget final:public QWidget {
    Q_OBJECT
public:
    explicit RvTunerWidget(RvTunerController *,QWidget *parent=nullptr);
    RvTuner::Settings settings() const;
    void acceptFrame(const RvTuner::Frame &);
protected:
    void changeEvent(QEvent *) override;
private:
    friend int runRvTunerTests(const QString &);
    void setControls(const RvTuner::Settings &);
    void modes();void refresh();void render();void retranslateUi();void record();
    void savePreset();void loadPreset();void exportComparisons();
    QString describe(const QJsonObject &) const;
    RvTunerController *m_controller;
    QComboBox *m_resolution,*m_mode,*m_target,*m_direction,*m_rotation;
    QSpinBox *m_iterations,*m_short,*m_snr,*m_period,*m_objects,*m_signalMax;
    QDoubleSpinBox *m_distanceMax;
    QCheckBox *m_signal,*m_noise,*m_log;
    QLabel *m_status,*m_pending,*m_applied,*m_saved,*m_stats,*m_hover;
    QTableWidget *m_results;
    QSplitter *m_splitter;
    RvTunerCanvas *m_distance,*m_photon;
    QMap<QString,QPushButton*> m_buttons;
    QMap<QString,QLabel*> m_labels;
    RvTuner::Frame m_frame;
    QJsonArray m_records;
    bool m_loaded=false,m_updating=false;
};
