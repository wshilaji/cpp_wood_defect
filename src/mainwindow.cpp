#include "mainwindow.h"
#include "config.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QPixmap>
#include <QImage>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QScrollArea>
#include <QFrame>
#include <QInputDialog>
#include <QLineEdit>

// ============================================================
// cv::Mat(BGR) → QImage（深拷贝，防止原图被后续处理改动）
// ============================================================
static QImage cvMatToQImage(const cv::Mat& m) {
    if (m.empty()) return QImage();
    if (m.type() == CV_8UC3)
        return QImage(m.data, m.cols, m.rows, (int)m.step,
                      QImage::Format_RGB888).rgbSwapped().copy();
    if (m.type() == CV_8UC1)
        return QImage(m.data, m.cols, m.rows, (int)m.step,
                      QImage::Format_Grayscale8).copy();
    return QImage();
}

// ============================================================
// 执行 systemctl 电源命令(poweroff/reboot), 返回是否成功。
// systemd 托管时进程不在登录会话, polkit 可能拒绝(无认证代理)而失败,
// 失败要让操作员看得见, 而不是 startDetached 那样静默。
// ============================================================
static bool runPowerCommand(const QString& verb) {
    QProcess p;
    p.start(QStringLiteral("systemctl"), {verb});
    if (!p.waitForStarted(3000)) return false;
    // logind 应答一般 <1s; 8s 兜底, 超时杀掉防 polkit 无人应答时挂死界面
    if (!p.waitForFinished(8000)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// ============================================================
// 小工具: 状态灯行 / 统计行 / 输入框行
// ============================================================
static QLabel* addLedRow(const QString& name, QBoxLayout* lay) {
    auto* row = new QHBoxLayout;
    auto* led = new QLabel(QString::fromUtf8("●"));
    led->setStyleSheet("color:#666; font-size:16px;");
    led->setFixedWidth(18);
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    lbl->setAlignment(Qt::AlignCenter);
    row->addWidget(led);
    row->addWidget(lbl, 1);
    lay->addLayout(row);
    return led;
}

static QLabel* addStatRow(const QString& name, QVBoxLayout* lay) {
    auto* row = new QHBoxLayout;
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    auto* val = new QLabel("--");
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setStyleSheet("color:#ffffff; font-weight:bold; font-size:16px;");
    row->addWidget(lbl, 1);
    row->addWidget(val);
    lay->addLayout(row);
    return val;
}

// 一行横排两对「名称 + 值」：统计项变多（9 项）后一列排会顶出屏幕，
// 工人就得拖滚动条。两列省下近一半高度。
// 两个标签都用 stretch=1 平分剩余空间，各自的「值」就落在本列右边缘，两列自然对齐。
static void addStatRowPair(const QString& n1, QLabel** v1,
                           const QString& n2, QLabel** v2,
                           QVBoxLayout* lay) {
    auto* row = new QHBoxLayout;
    row->setSpacing(14);

    auto* l1 = new QLabel(n1);
    l1->setStyleSheet("color:#c8c8c8;");
    auto* x1 = new QLabel("--");
    x1->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    x1->setStyleSheet("color:#ffffff; font-weight:bold; font-size:16px;");
    row->addWidget(l1, 1);
    row->addWidget(x1, 0);

    auto* l2 = new QLabel(n2);
    l2->setStyleSheet("color:#c8c8c8;");
    auto* x2 = new QLabel("--");
    x2->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    x2->setStyleSheet("color:#ffffff; font-weight:bold; font-size:16px;");
    row->addWidget(l2, 1);
    row->addWidget(x2, 0);

    lay->addLayout(row);
    *v1 = x1;
    if (v2) *v2 = x2;
}

static QSpinBox* addSpinRow(const QString& name, int lo, int hi, int def, QVBoxLayout* lay,
                            QWidget** outRow = nullptr) {
    auto* box = new QWidget;                 // 整行包成 QWidget，方便整行显隐
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    auto* sp = new QSpinBox;
    sp->setRange(lo, hi);
    sp->setValue(def);
    row->addWidget(lbl, 1);
    row->addWidget(sp);
    lay->addWidget(box);
    if (outRow) *outRow = box;
    return sp;
}

// 一行横排两个输入框（省纵向空间），单位用 spinbox 后缀显示
static void addSpinRowPair(const QString& name1, int lo1, int hi1, int def1,
                           const QString& unit1, QSpinBox** out1,
                           const QString& name2, int lo2, int hi2, int def2,
                           const QString& unit2, QSpinBox** out2,
                           QVBoxLayout* lay) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto* lbl1 = new QLabel(name1);
    lbl1->setStyleSheet("color:#c8c8c8;");
    auto* sp1  = new QSpinBox;
    sp1->setRange(lo1, hi1);
    sp1->setValue(def1);
    if (!unit1.isEmpty()) sp1->setSuffix(unit1);
    row->addWidget(lbl1, 1);
    row->addWidget(sp1);

    auto* lbl2 = new QLabel(name2);
    lbl2->setStyleSheet("color:#c8c8c8;");
    auto* sp2  = new QSpinBox;
    sp2->setRange(lo2, hi2);
    sp2->setValue(def2);
    if (!unit2.isEmpty()) sp2->setSuffix(unit2);
    row->addWidget(lbl2, 1);
    row->addWidget(sp2);

    lay->addWidget(box);
    if (out1) *out1 = sp1;
    if (out2) *out2 = sp2;
}

// 单个 QDoubleSpinBox 行（支持小数，比如 0.5%）
static QDoubleSpinBox* addSpinRowD(const QString& name, double lo, double hi, double def,
                                   const QString& unit, QVBoxLayout* lay) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    auto* sp = new QDoubleSpinBox;
    sp->setRange(lo, hi);
    sp->setDecimals(1);        // 一位小数
    sp->setSingleStep(0.5);    // 步进 0.5，支持 0.5%
    sp->setValue(def);
    if (!unit.isEmpty()) sp->setSuffix(unit);
    row->addWidget(lbl, 1);
    row->addWidget(sp);
    lay->addWidget(box);
    return sp;
}

// 一行横排两个 QDoubleSpinBox（支持小数）
static void addSpinRowPairD(const QString& name1, double lo1, double hi1, double def1,
                            const QString& unit1, QDoubleSpinBox** out1,
                            const QString& name2, double lo2, double hi2, double def2,
                            const QString& unit2, QDoubleSpinBox** out2,
                            QVBoxLayout* lay) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto* lbl1 = new QLabel(name1);
    lbl1->setStyleSheet("color:#c8c8c8;");
    auto* sp1 = new QDoubleSpinBox;
    sp1->setRange(lo1, hi1);
    sp1->setDecimals(1);
    sp1->setSingleStep(0.5);
    sp1->setValue(def1);
    if (!unit1.isEmpty()) sp1->setSuffix(unit1);
    row->addWidget(lbl1, 1);
    row->addWidget(sp1);

    auto* lbl2 = new QLabel(name2);
    lbl2->setStyleSheet("color:#c8c8c8;");
    auto* sp2 = new QDoubleSpinBox;
    sp2->setRange(lo2, hi2);
    sp2->setDecimals(1);
    sp2->setSingleStep(0.5);
    sp2->setValue(def2);
    if (!unit2.isEmpty()) sp2->setSuffix(unit2);
    row->addWidget(lbl2, 1);
    row->addWidget(sp2);

    lay->addWidget(box);
    if (out1) *out1 = sp1;
    if (out2) *out2 = sp2;
}

static void setLed(QLabel* led, bool on) {
    led->setStyleSheet(QString("color:%1; font-size:16px;")
                       .arg(on ? "#2ecc71" : "#666"));
}

// ============================================================
// 构造
// ============================================================
MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QString::fromUtf8("旭森智造"));
    resize(1500, 900);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // ---- 左: 图像显示区 ----
    _image = new QLabel(this);
    _image->setAlignment(Qt::AlignCenter);
    _image->setMinimumSize(960, 720);
    _image->setStyleSheet("background:#050505; border:1px solid #2a2a2a;");
    _image->setText(QString::fromUtf8("等待图像…"));
    root->addWidget(_image, 3);

    // ---- 右: 操作面板（放滚动区，工人设置行多了/屏幕矮时能滚动，不裁掉底部按钮） ----
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(380);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;}");
    auto* panel = new QWidget;
    panel->setFixedWidth(360);
    auto* v = new QVBoxLayout(panel);
    v->setSpacing(8);

    auto* title = new QLabel(QString::fromUtf8("旭森智造"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:22px; font-weight:bold; color:#4da6ff; padding:4px;");
    v->addWidget(title);

    // 系统状态（3 个灯横排，省空间）
    auto* grpSt = new QGroupBox(QString::fromUtf8("系统状态"), panel);
    auto* lSt   = new QHBoxLayout(grpSt);
    _ledPlc    = addLedRow(QString::fromUtf8("PLC 连接"), lSt);
    _ledCam    = addLedRow(QString::fromUtf8("相机"), lSt);
    _ledEngine = addLedRow(QString::fromUtf8("AI 引擎"), lSt);
    v->addWidget(grpSt);

    // 判定结果
    auto* grpRs = new QGroupBox(QString::fromUtf8("判定结果"), panel);
    auto* lRs   = new QVBoxLayout(grpRs);
    _resultBlock = new QLabel("--", grpRs);
    _resultBlock->setAlignment(Qt::AlignCenter);
    _resultBlock->setStyleSheet(
        "font-size:26px; font-weight:bold; color:#808080;"
        "background:#262a30; border-radius:8px; padding:6px;");
    _reasonLabel = new QLabel("", grpRs);
    _reasonLabel->setAlignment(Qt::AlignCenter);
    _reasonLabel->setStyleSheet(
        QString::fromUtf8("font-size:16px; color:#ff8080; min-height:22px;"));
    lRs->addWidget(_resultBlock);
    lRs->addWidget(_reasonLabel);
    v->addWidget(grpRs);

    // 统计
    auto* grpStt = new QGroupBox(QString::fromUtf8("统计"), panel);
    auto* lStt   = new QVBoxLayout(grpStt);
    lStt->setSpacing(4);   // 两列排布后行数多，行距收一点
    addStatRowPair(QString::fromUtf8("总检数"), &_statTotal,
                   QString::fromUtf8("NG 数"),  &_statNg,    lStt);
    addStatRowPair(QString::fromUtf8("合格率"), &_statRate,
                   QString::fromUtf8("耗时"),   &_statCycle, lStt);
    // 尺寸的值长（1200.0 × 600.0 mm），占一整行
    _statDims = addStatRow(QString::fromUtf8("木板尺寸"), lStt);
    addStatRowPair(QString::fromUtf8("GPU 温度"), &_statGpuTemp,
                   QString::fromUtf8("CPU 温度"), &_statCpuTemp, lStt);
    addStatRowPair(QString::fromUtf8("内存"), &_statMem,
                   QString::fromUtf8("硬盘"), &_statDisk, lStt);
    v->addWidget(grpStt);

    // 工人设置
    auto* grpSet = new QGroupBox(QString::fromUtf8("工人设置"), panel);
    auto* lSet   = new QVBoxLayout(grpSet);
    // 活节/死节/小油疤 数量阈值 —— 竖排成一列
    // (并排放不下三个: 面板固定 360px, 每个「6字标签+输入框」约 150px, 三个要 466px)
    // 活节 = 节扣发白、按不掉, 不影响使用; 死节 = 节扣没掉但一按就掉
    _jiebaSpin  = addSpinRow(QString::fromUtf8("活节数量大于"), 0, 50, 8, lSet);
    _dongbaSpin = addSpinRow(QString::fromUtf8("死节数量大于"), 0, 50, 8, lSet);
    // 小油疤(黑色油滴到板上, 板子不碎) 数量阈值 —— 数量 > 此值判 NG
    _heibaSpin = addSpinRow(QString::fromUtf8("小油疤数量大于"), 0, 500, 30, lSet);
    // 漏洞/缺边面积 —— 横排一行，省面板空间
    addSpinRowPairD(QString::fromUtf8("漏洞面积"), 0, 100, 1.0, " %", &_dongbanAreaSpin,
                    QString::fromUtf8("缺边面积"), 0, 100, 1.0, " %", &_quebianAreaSpin, lSet);
    // 标注备注 —— 大油疤在 labelme 里也标成 dongban, 所以跟着漏洞这条面积规则一起判
    auto* holeHint = new QLabel(QString::fromUtf8("（大油疤归到漏洞里面）"), grpSet);
    holeHint->setWordWrap(true);
    holeHint->setStyleSheet("color:#909090; font-size:12px;");
    lSet->addWidget(holeHint);
    _jiebaDongbaSpin    = addSpinRow(QString::fromUtf8("活节+死节数量"), 0, 100, 12, lSet);
    _dongbanQuebianSpin = addSpinRowD(QString::fromUtf8("漏洞+缺边面积"), 0, 100, 2.0, " %", lSet);
    // 板长/板宽最小尺寸（横排省空间）：测出长/宽低于此值判 NG（默认整板一半 600/300）
    addSpinRowPair(QString::fromUtf8("板长小于"), 0, 2000, 600, " mm", &_lenSpin,
                   QString::fromUtf8("板宽小于"), 0, 2000, 300, " mm", &_widSpin, lSet);
    // 原始图/结果图保存 %：默认隐藏，开发者模式开关开启（密码正确）后才显示
    _rawSpin    = addSpinRow(QString::fromUtf8("原始图保存 %"), 0, 100, 0, lSet, &_rawRow);
    _resultSpin = addSpinRow(QString::fromUtf8("结果图保存 %"), 0, 100, 0, lSet, &_resultRow);
    _rawRow->setVisible(false);
    _resultRow->setVisible(false);
    // 存图总开关：默认关，开启需密码（防止工人误开把硬盘写满）
    _saveChk = new QCheckBox(QString::fromUtf8("开发者模式（存图开关）"), grpSet);
    _saveChk->setStyleSheet(
        QString::fromUtf8("QCheckBox{color:#e0e0e0;} QCheckBox::indicator{width:18px;height:18px;}"));
    lSet->addWidget(_saveChk);
    // 存图保护提示：累计超 1GB 停存后显示
    _saveBlocked = new QLabel(QString::fromUtf8("⚠ 存图已停：累计超 1GB"), grpSet);
    _saveBlocked->setStyleSheet(QString::fromUtf8("color:#ff8080; font-size:14px;"));
    _saveBlocked->setVisible(false);
    lSet->addWidget(_saveBlocked);
    v->addWidget(grpSet);

    // 相机调参（工程师）
    auto* grpCam = new QGroupBox(QString::fromUtf8("相机调参"), panel);
    auto* lCam   = new QVBoxLayout(grpCam);
    // 曝光/增益并成一行省纵向空间。单位写死在标签里（不用 suffix）：两个框各带后缀
    // 会把宽度撑开，360px 的面板放不下两列。
    // 上限 24dB 是 MV-CS050-60GC 的标称增益范围(0~24dB, V5 高满阱模式只有 12.8)。
    // 原先写死的 30 没有任何出处, 提示里那句 0-300 更离谱, 一起对齐到这里。
    addSpinRowPair(QString::fromUtf8("曝光(us)"), 0, 100000, 7000, "", &_expoSpin,
                   QString::fromUtf8("增益(dB)"), 0, 24, 0, "", &_gainSpin, lCam);
    // 增益提示单独占一行: 面板固定 360px, 这么长的说明塞进标签会被挤没
    auto* gainHint = new QLabel(
        QString::fromUtf8("（增益范围0-24，0 是默认；除非太暗，否则不要动默认 0）"), grpCam);
    gainHint->setWordWrap(true);
    gainHint->setStyleSheet("color:#909090; font-size:12px;");
    lCam->addWidget(gainHint);
    v->addWidget(grpCam);

    // ---- 设置持久化: 存到当前目录 config.ini（可见文件，重启后保留） ----
    QSettings s(QStringLiteral("config.ini"), QSettings::IniFormat);
    _jiebaSpin->setValue(s.value("jieba_max", 8).toInt());
    _dongbaSpin->setValue(s.value("dongba_max", 8).toInt());
    _heibaSpin->setValue(s.value("heiba_max", 30).toInt());
    _dongbanAreaSpin->setValue(s.value("dongban_area_pct", 1).toDouble());
    _quebianAreaSpin->setValue(s.value("quebian_area_pct", 1).toDouble());
    _jiebaDongbaSpin->setValue(s.value("jieba_dongba_max", 12).toInt());
    _dongbanQuebianSpin->setValue(s.value("dongban_quebian_area_pct", 2).toDouble());
    _lenSpin->setValue(s.value("min_len_mm", 600).toInt());
    _widSpin->setValue(s.value("min_wid_mm", 300).toInt());
    _expoSpin->setValue(s.value("exposure_us", 7000).toInt());
    _gainSpin->setValue(s.value("gain_db", 0).toInt());
    connect(_jiebaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_max", v); });
    connect(_dongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongba_max", v); });
    connect(_heibaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("heiba_max", v); });
    connect(_dongbanAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_area_pct", v); });
    connect(_quebianAreaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("quebian_area_pct", v); });
    connect(_jiebaDongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_dongba_max", v); });
    connect(_dongbanQuebianSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [](double v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_quebian_area_pct", v); });
    connect(_lenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("min_len_mm", v); });
    connect(_widSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("min_wid_mm", v); });
    connect(_expoSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("exposure_us", v); });
    connect(_gainSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("gain_db", v); });
    // 存图开关：每次启动强制关（不持久化）；开启需密码，错则退回关；关闭随时可关
    connect(_saveChk, &QCheckBox::toggled, this, [this](bool on) {
        if (on && !verifySavePassword()) {
            _saveChk->setChecked(false);
            return;
        }
        _rawRow->setVisible(_saveChk->isChecked());      // 密码对才显示两行比例
        _resultRow->setVisible(_saveChk->isChecked());
    });

    // 操作按钮：四个并成一行。
    // 原先是两行两列(拍照/退出、关机/重启)，两行约 100px；并成一行约 42px，
    // 省下的 ~58px 正好盖住整列超屏的那点(1080 屏估算差 30px 上下)。
    // 宽度交给 stretch 按字数分配(4字:2字:2字:4字 = 3:2:2:3)，不写死 min-width ——
    // 写死 130px 的话四个要 520px，360px 面板根本放不下。
    // 实测每键约 90/60/60/90px 宽、42px 高，鼠标点是够用的。
    // 不用图标: emoji 在 Jetson 上没装 Noto Color Emoji 会显示成方框，
    // 而「关机/重启」画成电源/回转箭头是全界面最不能猜错的两个键 —— 猜错就是直接断电。
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    auto* snap = new QPushButton(QString::fromUtf8("手动拍照"), panel);
    snap->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:white;"
                          "background:#2e8b57; border-radius:6px;"));
    auto* exit = new QPushButton(QString::fromUtf8("退出"), panel);
    exit->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:white;"
                          "background:#c0392b; border-radius:6px;"));
    btnRow->addWidget(snap, 3);
    btnRow->addWidget(exit, 2);
    // 拍照/退出 与 关机/重启 之间留一道空档：同处一行后全靠这点间距分组，
    // 没有它「退出」和「关机」会挨着，误触代价不小。
    btnRow->addSpacing(16);
    auto* shutdownBtn = new QPushButton(QString::fromUtf8("关机"), panel);
    shutdownBtn->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:#ffd2d2;"
                          "background:#7a1f1f; border-radius:6px;"));
    auto* rebootBtn = new QPushButton(QString::fromUtf8("重启电脑"), panel);
    rebootBtn->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:#ffd2d2;"
                          "background:#6b4a1f; border-radius:6px;"));
    btnRow->addWidget(shutdownBtn, 2);
    btnRow->addWidget(rebootBtn, 3);
    v->addLayout(btnRow);
    v->addStretch(1);

    scroll->setWidget(panel);
    root->addWidget(scroll, 0);

    // 深色工业风主题
    setStyleSheet(QString::fromUtf8(R"(
        QWidget           { background:#14171c; color:#e0e0e0; font-size:15px; }
        QLabel            { background:transparent; }
        QGroupBox         { border:1px solid #2f353d; border-radius:8px;
                            margin-top:14px; padding:8px 6px 6px 6px; }
        QGroupBox::title  { subcontrol-origin:margin; left:10px; padding:0 4px;
                            color:#4da6ff; font-weight:bold; }
        QSpinBox, QDoubleSpinBox { background:#1d2128; border:1px solid #3a414b;
                            border-radius:4px; padding:4px; min-width:70px; }
        QPushButton       { background:#3a414b; border:none; border-radius:6px; padding:8px; }
        QPushButton:hover { background:#4a5360; }
    )"));

    connect(snap, &QPushButton::clicked, this, [this] { _manual = true; });
    connect(exit, &QPushButton::clicked, this, [this] { _exit = true; });
    connect(shutdownBtn, &QPushButton::clicked, this, [this] { doShutdown(); });
    connect(rebootBtn,   &QPushButton::clicked, this, [this] { doReboot(); });
}

// ============================================================
// 图像 / 结果刷新
// ============================================================
void MainWindow::setImage(const cv::Mat& bgr) {
    QImage img = cvMatToQImage(bgr);
    if (img.isNull()) return;
    _lastImage = std::move(img);
    updateImageDisplay();
}

void MainWindow::updateImageDisplay() {
    if (_lastImage.isNull()) return;
    // FastTransformation: 2448x2048 缩放到窗口，比 Smooth 快很多，Nano 上节省每帧开销
    _image->setPixmap(QPixmap::fromImage(_lastImage).scaled(
        _image->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    updateImageDisplay();
}

void MainWindow::setResult(bool ng, const QString& reason) {
    if (ng) {
        _resultBlock->setText("NG");
        _resultBlock->setStyleSheet(
            "font-size:26px; font-weight:bold; color:white;"
            "background:#c0392b; border-radius:8px; padding:6px;");
        _reasonLabel->setText(reason);
    } else {
        _resultBlock->setText("OK");
        _resultBlock->setStyleSheet(
            "font-size:26px; font-weight:bold; color:white;"
            "background:#2e8b57; border-radius:8px; padding:6px;");
        _reasonLabel->setText(QString::fromUtf8("正常"));
    }
}

void MainWindow::setStats(quint64 total, quint64 ng) {
    _statTotal->setText(QString::number(total));
    _statNg->setText(QString::number(ng));
    double rate = total > 0 ? 100.0 * (double)(total - ng) / (double)total : 0.0;
    _statRate->setText(QString::number(rate, 'f', 1) + "%");
}

void MainWindow::setGpuTemp(double gpu_c) {
    _statGpuTemp->setText(gpu_c < 0 ? QString::fromUtf8("--")
                                    : QString::number(gpu_c, 'f', 1) + QString::fromUtf8("°C"));
}

void MainWindow::setCpuTemp(double cpu_c) {
    _statCpuTemp->setText(cpu_c < 0 ? QString::fromUtf8("--")
                                    : QString::number(cpu_c, 'f', 1) + QString::fromUtf8("°C"));
}

void MainWindow::setMemoryPct(double pct) {
    _statMem->setText(pct < 0 ? QString::fromUtf8("--")
                              : QString::number(pct, 'f', 0) + "%");
}

void MainWindow::setDiskPct(double pct) {
    _statDisk->setText(pct < 0 ? QString::fromUtf8("--")
                               : QString::number(pct, 'f', 0) + "%");
}

void MainWindow::setMeasure(double long_mm, double short_mm) {
    _statDims->setText(QString::number(long_mm, 'f', 1) + QString::fromUtf8(" × ")
                       + QString::number(short_mm, 'f', 1) + QString::fromUtf8(" mm"));
}

void MainWindow::setCycleMs(double ms) {
    _statCycle->setText(QString::number(ms, 'f', 0) + " ms");
}

// ============================================================
// 状态灯
// ============================================================
void MainWindow::setPlcConnected(bool on) { setLed(_ledPlc, on); }
void MainWindow::setCamRunning(bool on) {
    _camRunning = on;
    renderCamLed();
}

void MainWindow::setCamFault(bool on) {
    _camFault = on;
    renderCamLed();
}

void MainWindow::renderCamLed() {
    // 三态：故障红 > 运行绿 > 未连接灰（故障优先级最高，恢复后自动回到运行/灰）
    const char* color = _camFault   ? "#e74c3c"
                      : _camRunning ? "#2ecc71"
                      :               "#666";
    _ledCam->setStyleSheet(QString("color:%1; font-size:16px;").arg(color));
}
void MainWindow::setEngineReady(bool on)  { setLed(_ledEngine, on); }
void MainWindow::setSaveBlocked(bool blocked) {
    if (_saveBlocked) _saveBlocked->setVisible(blocked);
}

// ============================================================
// 工人设置读取
// ============================================================
int MainWindow::jiebaMaxCount() const          { return _jiebaSpin->value(); }
int MainWindow::dongbaMaxCount() const         { return _dongbaSpin->value(); }
int MainWindow::heibaMaxCount() const          { return _heibaSpin->value(); }
double MainWindow::dongbanAreaPct() const         { return _dongbanAreaSpin->value(); }
double MainWindow::quebianAreaPct() const         { return _quebianAreaSpin->value(); }
int MainWindow::jiebaDongbaMaxCount() const    { return _jiebaDongbaSpin->value(); }
double MainWindow::dongbanQuebianAreaPct() const  { return _dongbanQuebianSpin->value(); }
int MainWindow::minLengthMm() const            { return _lenSpin->value(); }
int MainWindow::minWidthMm() const             { return _widSpin->value(); }
int MainWindow::rawSaveRatioPct() const        { return _rawSpin->value(); }
int MainWindow::resultSaveRatioPct() const     { return _resultSpin->value(); }
int MainWindow::exposureUs() const             { return _expoSpin->value(); }
int MainWindow::gainDb() const                 { return _gainSpin->value(); }
bool MainWindow::saveEnabled() const           { return _saveChk->isChecked(); }

// ============================================================
// 存图开关密码校验
// ============================================================
bool MainWindow::verifySavePassword() {
    bool ok = false;
    QString pwd = QInputDialog::getText(this,
                    QString::fromUtf8("开启存图"),
                    QString::fromUtf8("请输入存图密码："),
                    QLineEdit::Password, QString(), &ok);
    if (!ok) return false;
    return pwd == QString::fromUtf8(Config::SAVE_ENABLE_PASSWORD);
}

// ============================================================
// 按钮标志
// ============================================================
bool MainWindow::takeManualTrigger() {
    if (_manual) { _manual = false; return true; }
    return false;
}

bool MainWindow::exitRequested() const { return _exit; }

// ============================================================
// 一键关机：确认后调用 systemctl poweroff
// systemctl poweroff 走 logind，桌面登录用户即可，无需 sudo
// ============================================================
void MainWindow::doShutdown() {
    QMessageBox box(QMessageBox::Warning,
                    QString::fromUtf8("确认关机"),
                    QString::fromUtf8("确定要关闭整个系统吗？\n正在进行的检测将立即中断。"),
                    QMessageBox::NoButton, this);
    auto* yes = box.addButton(QString::fromUtf8("关机"), QMessageBox::AcceptRole);
    box.addButton(QString::fromUtf8("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != yes) return;

    if (!runPowerCommand(QStringLiteral("poweroff")))
        QMessageBox::critical(this, QString::fromUtf8("关机失败"),
                              QString::fromUtf8("关机命令未执行成功，系统不会关机。\n"
                                                "程序由 systemd 托管时进程不在登录会话，需要 polkit 授权。\n"
                                                "请重跑：sudo ./install-systemd.sh（或 bash fix-systemd.sh）"));
}

// 一键重启：确认后调用 systemctl reboot
// systemctl reboot 走 logind，桌面登录用户即可，无需 sudo
void MainWindow::doReboot() {
    QMessageBox box(QMessageBox::Warning,
                    QString::fromUtf8("确认重启"),
                    QString::fromUtf8("确定要重启整个系统吗？\n正在进行的检测将立即中断。"),
                    QMessageBox::NoButton, this);
    auto* yes = box.addButton(QString::fromUtf8("重启电脑"), QMessageBox::AcceptRole);
    box.addButton(QString::fromUtf8("取消"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != yes) return;

    if (!runPowerCommand(QStringLiteral("reboot")))
        QMessageBox::critical(this, QString::fromUtf8("重启失败"),
                              QString::fromUtf8("重启命令未执行成功，系统不会重启。\n"
                                                "程序由 systemd 托管时进程不在登录会话，需要 polkit 授权。\n"
                                                "请重跑：sudo ./install-systemd.sh（或 bash fix-systemd.sh）"));
}
