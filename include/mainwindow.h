#pragma once

#include <QWidget>
#include <QString>
#include <QImage>
#include <opencv2/opencv.hpp>

class QLabel;
class QSpinBox;
class QCheckBox;

/**
 * 木板瑕疵检测 — Qt 操作界面
 *
 * 由检测主循环驱动刷新：setImage/setResult/setStats/setGpuTemp/setMeasure/setCycleMs。
 * 工人设置（jieba 数量 / 存图比例 / 曝光 / 增益）用 QSpinBox，主循环轮询读取后下发。
 * 手动拍照 / 退出 通过按钮置位标志，主循环轮询消费（takeManualTrigger/exitRequested）。
 */
class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // ---- 图像 / 结果 / 统计刷新（主循环调用） ----
    void setImage(const cv::Mat& bgr);
    void setResult(bool ng, const QString& reason);
    void setStats(quint64 total, quint64 ng);
    void setGpuTemp(double c);
    void setMeasure(double long_mm, double short_mm);
    void setCycleMs(double ms);

    // ---- 状态灯 ----
    void setPlcConnected(bool on);
    void setCamRunning(bool on);
    void setEngineReady(bool on);

    // ---- 工人设置（主循环轮询读取） ----
    int jiebaMaxCount() const;
    int dongbaMaxCount() const;
    int dongbanAreaPct() const;
    int quebianAreaPct() const;
    int jiebaDongbaMaxCount() const;
    int dongbanQuebianAreaPct() const;
    int rawSaveRatioPct() const;
    int exposureUs() const;
    int gainDb() const;

    /** 存图总开关：默认关；开一次后按比例存原始+结果图。开启需密码（verifySavePassword） */
    bool saveEnabled() const;

    // ---- 按钮（主循环轮询消费） ----
    bool takeManualTrigger();      // true=工人点了手动拍照（消费一次）
    bool exitRequested() const;

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateImageDisplay();
    void doShutdown();   // 一键关机：确认后调用 systemctl poweroff
    bool verifySavePassword();   // 弹密码框，返回密码是否正确

    QLabel*   _image         = nullptr;
    QLabel*   _ledPlc        = nullptr;
    QLabel*   _ledCam        = nullptr;
    QLabel*   _ledEngine     = nullptr;
    QLabel*   _resultBlock   = nullptr;
    QLabel*   _reasonLabel   = nullptr;
    QLabel*   _statTotal     = nullptr;
    QLabel*   _statNg        = nullptr;
    QLabel*   _statRate      = nullptr;
    QLabel*   _statDims      = nullptr;
    QLabel*   _statCycle     = nullptr;
    QLabel*   _statGpu       = nullptr;
    QSpinBox* _jiebaSpin        = nullptr;
    QSpinBox* _dongbaSpin       = nullptr;
    QSpinBox* _dongbanAreaSpin  = nullptr;
    QSpinBox* _quebianAreaSpin  = nullptr;
    QSpinBox* _jiebaDongbaSpin  = nullptr;
    QSpinBox* _dongbanQuebianSpin = nullptr;
    QSpinBox* _rawSpin          = nullptr;
    QSpinBox* _expoSpin         = nullptr;
    QSpinBox* _gainSpin         = nullptr;
    QCheckBox* _saveChk         = nullptr;

    QImage    _lastImage;
    bool      _manual = false;
    bool      _exit   = false;
};
