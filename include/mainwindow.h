#pragma once

#include <QWidget>
#include <QString>
#include <QImage>
#include <opencv2/opencv.hpp>

class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;

/**
 * 木板瑕疵检测 — Qt 操作界面
 *
 * 由检测主循环驱动刷新：setImage/setResult/setStats/setTemps/setMeasure/setCycleMs。
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
    void setTemps(double gpu_c, double cam_c);   // GPU + 相机温度，负值显示 --
    void setMeasure(double long_mm, double short_mm);
    void setCycleMs(double ms);

    // ---- 状态灯 ----
    void setPlcConnected(bool on);
    void setCamRunning(bool on);   // 绿=运行, 灰=未连接
    void setCamFault(bool on);     // 红=故障（连续空帧判故障），优先级最高，恢复后清除
    void setEngineReady(bool on);

    // ---- 存图保护：累计超 1GB 停存后界面提示 ----
    void setSaveBlocked(bool blocked);

    // ---- 工人设置（主循环轮询读取） ----
    int jiebaMaxCount() const;
    int dongbaMaxCount() const;
    double dongbanAreaPct() const;
    double quebianAreaPct() const;
    int jiebaDongbaMaxCount() const;
    double dongbanQuebianAreaPct() const;
    int minLengthMm() const;
    int minWidthMm() const;
    int rawSaveRatioPct() const;
    int resultSaveRatioPct() const;
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
    void renderCamLed();   // 相机灯三态渲染：故障红 > 运行绿 > 未连接灰

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
    QLabel*   _statTemp      = nullptr;
    QSpinBox* _jiebaSpin        = nullptr;
    QSpinBox* _dongbaSpin       = nullptr;
    QDoubleSpinBox* _dongbanAreaSpin  = nullptr;
    QDoubleSpinBox* _quebianAreaSpin  = nullptr;
    QSpinBox* _jiebaDongbaSpin  = nullptr;
    QDoubleSpinBox* _dongbanQuebianSpin = nullptr;
    QSpinBox* _lenSpin          = nullptr;
    QSpinBox* _widSpin          = nullptr;
    QSpinBox* _rawSpin          = nullptr;
    QWidget*  _rawRow           = nullptr;   // 原始图保存%整行，开发者模式开关开启后才显示
    QSpinBox* _resultSpin       = nullptr;
    QWidget*  _resultRow        = nullptr;   // 结果图保存%整行，开发者模式开关开启后才显示
    QSpinBox* _expoSpin         = nullptr;
    QSpinBox* _gainSpin         = nullptr;
    QCheckBox* _saveChk         = nullptr;
    QLabel*    _saveBlocked     = nullptr;

    QImage    _lastImage;
    bool      _manual = false;
    bool      _exit   = false;
    bool      _camRunning = false;   // 相机是否在运行（setCamRunning 写入）
    bool      _camFault   = false;   // 相机是否故障（setCamFault 写入，红灯）
};
