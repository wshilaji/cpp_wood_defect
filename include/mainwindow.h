#pragma once

#include <QWidget>
#include <QString>
#include <QImage>
#include <QPixmap>
#include <vector>
#include <opencv2/opencv.hpp>

class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;

/**
 * 木板瑕疵检测 — Qt 操作界面
 *
 * 由检测主循环驱动刷新：
 *   setImage/setResult/setStats/setMeasure/setCycleMs
 *   setGpuTemp/setCpuTemp/setMemoryPct/setDiskPct（系统状态，空闲时刷新）
 * 工人设置（各类数量 / 各类尺寸门槛 / 破洞·树皮·发白三道置信度门槛 / 板长板宽 / 存图比例 /
 * 曝光增益）用输入框，主循环轮询读取后下发。三道置信度门槛是 QDoubleSpinBox，其余都是 QSpinBox。
 * （2026-09-23 起【没有面积占比这类设置了】—— 四个类都改成了数量 + 尺寸门槛，见 config.h）
 * 手动拍照 通过按钮置位标志，主循环轮询消费（takeManualTrigger）。
 * 「最小化」不走主循环：点一下就把窗口藏起来、屏幕角上留一个小条（_restoreTab）点它
 * 回来。检测全程照跑 —— 相机/PLC/推理都不断，只是屏幕让出来了。见 minimizeToDesktop。
 */
class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // ---- 图像 / 结果 / 统计刷新（主循环调用） ----
    void setImage(const cv::Mat& bgr);

    /** 往左下角「最近结果」那一条上推一张缩略图（最新的排最右，其余整体左移一格）。
     *  ok = 这块是 OK 还是 NG —— 图上那个大字和色带就是它，不看内容也能一眼扫出走向。
     *  传的是【结果图】(带框那张)，内部自己缩到小图尺寸（大小见 mainwindow.cpp 的
     *  THUMB_W/THUMB_H）。
     *  ⚠ 只留缩略图，不留原图：一张 2448×2048 是 15MB，留 6 张就是 90MB，而缩略图一张
     *    才 ~130KB。这是这个功能唯一的内存口径，别改成一整条存原图。 */
    void pushThumb(const cv::Mat& result, bool ok);

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
    // 另外破洞和 heiba 各多一道更严的第二道数量规则（dongbanBig* / heibaBig*，
    // 界面上是「大破洞」和「大油疤」两行），形状完全一样：门槛以上的才算数，
    // 块数超过上限判 NG。
    // dongban / shupi / fabai 除了尺寸门槛还各多一道置信度门槛（dongbanMinConf /
    // shupiMinConf / fabaiMinConf）—— 这是全项目仅有的三道不看框大小、只看模型把握的
    // 门槛，跟尺寸门槛是「且」的关系。
    int jiebaMaxCount() const;
    int dongbaMaxCount() const;
    int dongbaMinLenMm() const;
    int dongbanMaxCount() const;
    int dongbanMinLenMm() const;
    // 破洞的第二道更严的门槛（界面行名/原因串都叫「大破洞」）。跟上面那对是
    // 同一个类的两条规则，形状一样：门槛 dongbanBigMinLenMm 以上的破洞算数，
    // 块数超过 dongbanBigMaxCount 判 NG。
    int dongbanBigMaxCount() const;
    int dongbanBigMinLenMm() const;
    /** dongban 的置信度门槛：模型的 dongban 概率大于此值才算数（0 = 关掉这道门槛）。
     *  ⚠ 它只卡上面那条「破洞」(dongbanMaxCount)：它挂在 postprocessor 的 minConfFor 表上，
     *    是 countsTowardRule 的一部分。而「大破洞」(dongbanBigMaxCount) 在 isNG 里是自己
     *    一条 if，不走那张表，所以【不】应用这道门槛。
     *  ⚠ 有作用的区间同 fabaiMinConf：0.31~0.99（全局 CONF_THRESHOLD 在更前面就筛掉了
     *    0.3 以下，填 0.3 及以下等于没填）。出厂 0.45（现场定的数，见 config.h 的
     *    DONGBAN_MIN_CONF）。 */
    double dongbanMinConf() const;
    int heibaMaxCount() const;
    // heiba 的第二道更严的门槛（界面行名/原因串都叫「大油疤」，2026-10-03 加的）。
    // 形状跟上面 dongbanBig* 那一对一样，只是换了个类：门槛 heibaBigMinDiagMm 以上的
    // heiba 算数（画框时框线也换成黄色，同一个门槛），块数超过 heibaBigMaxCount 判 NG。
    // ⚠ 量的是【对角线】，不是最长边 —— 全项目唯一一处，理由见 config.h 的
    //    HEIBA_BIG_MIN_DIAG_MM。所以这个门槛的数值跟别的类（含「大破洞」）不可直接比。
    // ⚠ 两条各数各的：够大的 heiba 既算进 heibaMaxCount 的 24，也算进这条。
    int heibaBigMaxCount() const;
    int heibaBigMinDiagMm() const;
    int quebianMaxCount() const;
    int quebianMinLenMm() const;
    int shupiMaxCount() const;
    int shupiMinLenMm() const;
    /** shupi 的置信度门槛：模型的 shupi 概率大于此值才算数（0 = 关掉这道门槛）。
     *  ⚠ 有作用的区间同 fabaiMinConf：0.31~0.99（全局 CONF_THRESHOLD 在更前面就筛掉了
     *    0.3 以下，填 0.3 及以下等于没填）。出厂 0.40（现场定的数，见 config.h 的
     *    SHUPI_MIN_CONF；0 = 关掉这道门槛）。 */
    double shupiMinConf() const;
    int fabaiMaxCount() const;
    int fabaiMinLenMm() const;
    /** fabai 的置信度门槛：模型的 fabai 概率大于此值才算数（0 = 关掉这道门槛）。
     *  ⚠ 有作用的区间是 0.31~0.99：全局 CONF_THRESHOLD(0.3) 在更前面就把 0.3 以下的检测
     *    整个丢掉了，图上根本不会出现 conf < 0.3 的框，填 0.3 及以下等于没填。
     *    （这个区间跟着 CONF_THRESHOLD 走，全局降过一次，下界也跟着降过一次。） */
    double fabaiMinConf() const;
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

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateImageDisplay();
    /** 「最小化」：把全屏窗口藏起来让出桌面，并在屏幕角上显示 _restoreTab 小条。
     *  检测不受影响（相机/PLC/推理都还在跑）—— 这是它跟原来那个「退出」的本质区别：
     *  同一个目的（别让全屏软件占着桌面），但不断检测、不用等、不用靠 systemd 拉起。 */
    void minimizeToDesktop();
    /** 点小条回来：藏掉小条、全屏置顶恢复。 */
    void restoreFromDesktop();
    void buildRestoreTab();   // 建小条本体（构造时调一次，建完先藏着）
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
    // 大破洞：dongban 的第二道门槛，行里两个框的排法跟上面一样
    QSpinBox* _dongbanBigSpin      = nullptr;
    QSpinBox* _dongbanBigMinLenSpin= nullptr;
    // 大油疤：heiba 的第二道门槛（2026-10-03 加的），紧挨着上面那一对排。
    // 两个类各一对，形状完全一样 —— 别把这两对看成一回事：上面那对数 dongban、
    // 这对数 heiba，行名和 NG 原因串也分得开（「大破洞」/「大油疤」）。
    // ⚠ 这对右边的框是【对角线】(mm)，跟别的行量的不是一回事 —— 见 config.h 的
    //    HEIBA_BIG_MIN_DIAG_MM。【界面上不提这个】(2026-10-03 现场定的)，别到时候看见
    //    界面没写以为是漏了。
    QSpinBox* _heibaBigSpin        = nullptr;
    QSpinBox* _heibaBigMinDiagSpin = nullptr;
    QSpinBox* _quebianSpin      = nullptr;
    QSpinBox* _quebianMinLenSpin= nullptr;
    QSpinBox* _shupiSpin        = nullptr;
    QSpinBox* _shupiMinLenSpin  = nullptr;
    QSpinBox* _fabaiSpin        = nullptr;
    QSpinBox* _fabaiMinLenSpin  = nullptr;
    // 三个置信度门槛（0~1 的小数），排在发白那行数量门槛的下面：破洞独占一行，
    // 树皮和发白并成一行（2026-10-02 现场要的「省点空间」）。
    // 留神这三个的属性：在全项目所有输入框里只有它们是 QDoubleSpinBox，也只有它们带 ">"
    // 前缀（显示成 >0.65）—— 形状和宽度账在 mainwindow.cpp 的 addMinConfRow /
    // addMinConfRowPair。
    // ⚠ 别把三个并回一行: 2026-10-02 试过, 现场说「挤在一起了」。
    QDoubleSpinBox* _dongbanMinConfSpin = nullptr;
    QDoubleSpinBox* _shupiMinConfSpin = nullptr;
    QDoubleSpinBox* _fabaiMinConfSpin = nullptr;
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

    // 「最近结果」缩略图条：固定几个格子，最新的在最右，每来一块整体左移一格。
    // 格子数在构造时定死（mainwindow.cpp 的 THUMB_COUNT），这里只用 vector 装。
    std::vector<QLabel*> _thumbs;
    // 每个格子里那张图，下标跟 _thumbs 一一对应（一开始全是占位图）。
    // 自己存一份、不回读 QLabel —— QLabel::pixmap() 那个返回裸指针的重载在 Qt 5.15
    // 已经标了废弃，而且状态自己拿着比从控件里掏出来清楚，两处也不可能对不上。
    std::vector<QPixmap> _thumbPix;

    QImage    _lastImage;
    bool      _manual = false;
    // 「最小化」时留在屏幕角上的恢复小条。构造函数里建好先藏着，最小化时显示。
    // 顶层窗口（parent 传 nullptr），不是 this 的子控件 —— 主窗口藏起来之后子控件会
    // 跟着一起不可见，那就没法当恢复入口了。
    QPushButton* _restoreTab = nullptr;
    bool      _camRunning = false;   // 相机是否在运行（setCamRunning 写入）
    bool      _camFault   = false;   // 相机是否故障（setCamFault 写入，红灯）
};
