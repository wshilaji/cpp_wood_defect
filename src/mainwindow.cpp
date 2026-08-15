#include "mainwindow.h"
#include "config.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSpinBox>
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
    _statTotal = addStatRow(QString::fromUtf8("总检数"), lStt);
    _statNg    = addStatRow(QString::fromUtf8("NG 数"), lStt);
    _statRate  = addStatRow(QString::fromUtf8("合格率"), lStt);
    _statDims  = addStatRow(QString::fromUtf8("木板尺寸"), lStt);
    _statCycle = addStatRow(QString::fromUtf8("耗时"), lStt);
    _statGpu   = addStatRow(QString::fromUtf8("GPU 温度"), lStt);
    v->addWidget(grpStt);

    // 工人设置
    auto* grpSet = new QGroupBox(QString::fromUtf8("工人设置"), panel);
    auto* lSet   = new QVBoxLayout(grpSet);
    // 结疤/洞疤 NG 数量、洞坑/缺边面积 —— 各横排一行，省面板空间
    addSpinRowPair(QString::fromUtf8("结疤NG"), 0, 50, 8, "", &_jiebaSpin,
                   QString::fromUtf8("洞疤NG"), 0, 50, 8, "", &_dongbaSpin, lSet);
    addSpinRowPair(QString::fromUtf8("洞坑面积"), 0, 100, 1, " %", &_dongbanAreaSpin,
                   QString::fromUtf8("缺边面积"), 0, 100, 1, " %", &_quebianAreaSpin, lSet);
    _jiebaDongbaSpin    = addSpinRow(QString::fromUtf8("结疤+洞疤数量"), 0, 100, 12, lSet);
    _dongbanQuebianSpin = addSpinRow(QString::fromUtf8("洞坑+缺边面积 %"), 0, 100, 2, lSet);
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
    _expoSpin = addSpinRow(QString::fromUtf8("曝光 (us)"), 0, 100000, 7000, lCam);
    _gainSpin = addSpinRow(QString::fromUtf8("增益 (dB)"), 0, 30, 0, lCam);
    v->addWidget(grpCam);

    // ---- 设置持久化: 存到当前目录 config.ini（可见文件，重启后保留） ----
    QSettings s(QStringLiteral("config.ini"), QSettings::IniFormat);
    _jiebaSpin->setValue(s.value("jieba_max", 8).toInt());
    _dongbaSpin->setValue(s.value("dongba_max", 8).toInt());
    _dongbanAreaSpin->setValue(s.value("dongban_area_pct", 1).toInt());
    _quebianAreaSpin->setValue(s.value("quebian_area_pct", 1).toInt());
    _jiebaDongbaSpin->setValue(s.value("jieba_dongba_max", 12).toInt());
    _dongbanQuebianSpin->setValue(s.value("dongban_quebian_area_pct", 2).toInt());
    _lenSpin->setValue(s.value("min_len_mm", 600).toInt());
    _widSpin->setValue(s.value("min_wid_mm", 300).toInt());
    _expoSpin->setValue(s.value("exposure_us", 7000).toInt());
    _gainSpin->setValue(s.value("gain_db", 0).toInt());
    connect(_jiebaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_max", v); });
    connect(_dongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongba_max", v); });
    connect(_dongbanAreaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_area_pct", v); });
    connect(_quebianAreaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("quebian_area_pct", v); });
    connect(_jiebaDongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_dongba_max", v); });
    connect(_dongbanQuebianSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_quebian_area_pct", v); });
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

    // 操作按钮
    auto* btnRow = new QHBoxLayout;
    auto* snap = new QPushButton(QString::fromUtf8("手动拍照"), panel);
    snap->setStyleSheet(
        QString::fromUtf8("font-size:18px; font-weight:bold; padding:10px; color:white;"
                          "background:#2e8b57; border-radius:6px;"));
    auto* exit = new QPushButton(QString::fromUtf8("退出"), panel);
    exit->setStyleSheet(
        QString::fromUtf8("font-size:18px; font-weight:bold; padding:10px; color:white;"
                          "background:#c0392b; border-radius:6px;"));
    btnRow->addWidget(snap);
    btnRow->addWidget(exit);
    v->addLayout(btnRow);

    // 关机按钮（独立一行，防误触）
    auto* shutdownBtn = new QPushButton(QString::fromUtf8("关机"), panel);
    shutdownBtn->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:8px; color:#ffd2d2;"
                          "background:#7a1f1f; border-radius:6px;"));
    v->addWidget(shutdownBtn);
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
        QSpinBox          { background:#1d2128; border:1px solid #3a414b;
                            border-radius:4px; padding:4px; min-width:70px; }
        QPushButton       { background:#3a414b; border:none; border-radius:6px; padding:8px; }
        QPushButton:hover { background:#4a5360; }
    )"));

    connect(snap, &QPushButton::clicked, this, [this] { _manual = true; });
    connect(exit, &QPushButton::clicked, this, [this] { _exit = true; });
    connect(shutdownBtn, &QPushButton::clicked, this, [this] { doShutdown(); });
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

void MainWindow::setGpuTemp(double c) {
    _statGpu->setText(QString::number(c, 'f', 1) + QString::fromUtf8(" °C"));
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
void MainWindow::setCamRunning(bool on)   { setLed(_ledCam, on); }
void MainWindow::setEngineReady(bool on)  { setLed(_ledEngine, on); }
void MainWindow::setSaveBlocked(bool blocked) {
    if (_saveBlocked) _saveBlocked->setVisible(blocked);
}

// ============================================================
// 工人设置读取
// ============================================================
int MainWindow::jiebaMaxCount() const          { return _jiebaSpin->value(); }
int MainWindow::dongbaMaxCount() const         { return _dongbaSpin->value(); }
int MainWindow::dongbanAreaPct() const         { return _dongbanAreaSpin->value(); }
int MainWindow::quebianAreaPct() const         { return _quebianAreaSpin->value(); }
int MainWindow::jiebaDongbaMaxCount() const    { return _jiebaDongbaSpin->value(); }
int MainWindow::dongbanQuebianAreaPct() const  { return _dongbanQuebianSpin->value(); }
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

    QProcess::startDetached(QStringLiteral("systemctl"),
                            {QStringLiteral("poweroff")});
}
