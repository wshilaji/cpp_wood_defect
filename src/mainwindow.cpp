#include "mainwindow.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QPushButton>
#include <QPixmap>
#include <QImage>
#include <QMessageBox>
#include <QProcess>

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
static QLabel* addLedRow(const QString& name, QVBoxLayout* lay) {
    auto* row = new QHBoxLayout;
    auto* led = new QLabel(QString::fromUtf8("●"));
    led->setStyleSheet("color:#666; font-size:16px;");
    led->setFixedWidth(20);
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
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

static QSpinBox* addSpinRow(const QString& name, int lo, int hi, int def, QVBoxLayout* lay) {
    auto* row = new QHBoxLayout;
    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    auto* sp = new QSpinBox;
    sp->setRange(lo, hi);
    sp->setValue(def);
    row->addWidget(lbl, 1);
    row->addWidget(sp);
    lay->addLayout(row);
    return sp;
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

    // ---- 右: 操作面板 ----
    auto* panel = new QWidget(this);
    panel->setFixedWidth(360);
    auto* v = new QVBoxLayout(panel);
    v->setSpacing(8);

    auto* title = new QLabel(QString::fromUtf8("旭森智造"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:22px; font-weight:bold; color:#4da6ff; padding:4px;");
    v->addWidget(title);

    // 系统状态
    auto* grpSt = new QGroupBox(QString::fromUtf8("系统状态"), panel);
    auto* lSt   = new QVBoxLayout(grpSt);
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
        "font-size:42px; font-weight:bold; color:#808080;"
        "background:#262a30; border-radius:10px; padding:16px;");
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
    _statCycle = addStatRow(QString::fromUtf8("节拍"), lStt);
    _statGpu   = addStatRow(QString::fromUtf8("GPU 温度"), lStt);
    v->addWidget(grpStt);

    // 工人设置
    auto* grpSet = new QGroupBox(QString::fromUtf8("工人设置"), panel);
    auto* lSet   = new QVBoxLayout(grpSet);
    _jiebaSpin = addSpinRow(QString::fromUtf8("jieba NG 数量"), 0, 50, 8, lSet);
    _rawSpin   = addSpinRow(QString::fromUtf8("原始图保存 %"), 0, 100, 50, lSet);
    v->addWidget(grpSet);

    // 相机调参（工程师）
    auto* grpCam = new QGroupBox(QString::fromUtf8("相机调参"), panel);
    auto* lCam   = new QVBoxLayout(grpCam);
    _expoSpin = addSpinRow(QString::fromUtf8("曝光 (us)"), 0, 100000, 7000, lCam);
    _gainSpin = addSpinRow(QString::fromUtf8("增益 (dB)"), 0, 30, 0, lCam);
    v->addWidget(grpCam);

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

    root->addWidget(panel, 0);

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
            "font-size:42px; font-weight:bold; color:white;"
            "background:#c0392b; border-radius:10px; padding:16px;");
        _reasonLabel->setText(reason);
    } else {
        _resultBlock->setText("OK");
        _resultBlock->setStyleSheet(
            "font-size:42px; font-weight:bold; color:white;"
            "background:#2e8b57; border-radius:10px; padding:16px;");
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

// ============================================================
// 工人设置读取
// ============================================================
int MainWindow::jiebaMaxCount() const  { return _jiebaSpin->value(); }
int MainWindow::rawSaveRatioPct() const{ return _rawSpin->value(); }
int MainWindow::exposureUs() const     { return _expoSpin->value(); }
int MainWindow::gainDb() const         { return _gainSpin->value(); }

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
