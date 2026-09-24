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
 * 由检测主循环驱动刷新：
 *   setImage/setResult/setStats/setMeasure/setCycleMs
 *   setGpuTemp/setCpuTemp/setMemoryPct/setDiskPct（系统状态，空闲时刷新）
 * 工人设置（各类数量 / 面积占比 / 板长板宽 / 存图比例 / 曝光增益）用 QSpinBox，
 * 主循环轮询读取后下发。
 * 手动拍照 / 退出 通过按钮置位标志，主循环轮询消费（takeManualTrigger/exitRequested）。
 */
class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // ---- 图像 / 结果 / 统计刷新（主循环调用） ----
    void setImage(const cv::Mat& bgr);
    void setResult(bool ng, const QString& reason);
    void setStats(quint64 total, quint64 ng);
    void setGpuTemp(double gpu_c);    // GPU 温度（英伟达），负值显示 --
    void setCpuTemp(double cpu_c);    // CPU 温度，负值显示 --（读不到传感器时为负）
    void setMemoryPct(double pct);    // 内存占用率 %，负值显示 --
    void setDiskPct(double pct);      // 存图所在盘占用率 %，负值显示 --
    void setMeasure(double long_mm, double short_mm);
    void setCycleMs(double ms);

    // ---- 状态灯 ----
    void setPlcConnected(bool on);
    void setCamRunning(bool on);   // 绿=运行, 灰=未连接
    void setCamFault(bool on);     // 红=故障（连续空帧判故障），优先级最高，恢复后清除
    void setEngineReady(bool on);

    // ---- 存图保护：磁盘不足 / 目录超 60G 停存后界面提示 ----
    // why 由 SaveWorker::blockedReason() 给（"磁盘空间不足" / "存图目录超 60G"），
    // 空串时用一句兜底文案，不让提示框看起来像坏了
    void setSaveBlocked(bool blocked, const QString& why = QString());

    // ---- 工人设置（主循环轮询读取） ----
    // 判定用的 7 个类全是数量规则：*MaxCount 是数量上限，*MinLenMm 是尺寸门槛
    // （最长边短于此值的不计数，0 = 不过滤）。jieba/heiba 没有门槛这一半。
    int jiebaMaxCount() const;
    int dongbaMaxCount() const;
    int dongbaMinLenMm() const;
    int dongbanMaxCount() const;
    int dongbanMinLenMm() const;
    // 破洞的第二道更严的门槛（界面行名/原因串都叫「大破洞或大油疤」）。跟上面那对是
    // 同一个类的两条规则，形状一样：门槛 dongbanBigMinLenMm 以上的破洞算数，
    // 块数超过 dongbanBigMaxCount 判 NG。
    int dongbanBigMaxCount() const;
    int dongbanBigMinLenMm() const;
    int heibaMaxCount() const;
    int quebianMaxCount() const;
    int quebianMinLenMm() const;
    int shupiMaxCount() const;
    int shupiMinLenMm() const;
    int fabaiMaxCount() const;
    int fabaiMinLenMm() const;
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
    void doReboot();     // 一键重启：确认后调用 systemctl reboot
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
    QLabel*   _statGpuTemp   = nullptr;
    QLabel*   _statCpuTemp   = nullptr;
    QLabel*   _statMem       = nullptr;
    QLabel*   _statDisk      = nullptr;
    QSpinBox* _jiebaSpin        = nullptr;
    QSpinBox* _dongbaSpin       = nullptr;
    QSpinBox* _dongbaMinLenSpin = nullptr;   // 跟 _dongbaSpin 同一行，在右边
    QSpinBox* _heibaSpin        = nullptr;
    QSpinBox* _dongbanSpin      = nullptr;   // 下面 5 个都跟自己的 *MinLenSpin 同一行
    QSpinBox* _dongbanMinLenSpin= nullptr;   // （左数量、右门槛），跟 _dongbaSpin 一个样式
    // 大破洞或大油疤：dongban 的第二道门槛，行里两个框的排法跟上面一样
    QSpinBox* _dongbanBigSpin      = nullptr;
    QSpinBox* _dongbanBigMinLenSpin= nullptr;
    QSpinBox* _quebianSpin      = nullptr;
    QSpinBox* _quebianMinLenSpin= nullptr;
    QSpinBox* _shupiSpin        = nullptr;
    QSpinBox* _shupiMinLenSpin  = nullptr;
    QSpinBox* _fabaiSpin        = nullptr;
    QSpinBox* _fabaiMinLenSpin  = nullptr;
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
