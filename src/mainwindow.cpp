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

// 「左列 | 竖线 | 右列」骨架，返回两个列布局供调用方往里塞行。
//
// 为什么不是「每行插一小段竖线」：一是行间距（4px）会把线切成虚线，二是每行的
// 分界点取决于该行值标签的宽度（"1234" 与 "45.2°C" 不等宽），得额外把值列宽度
// 统一了才能对齐。改成整条通高的线 + 左右两列各 stretch=1，等宽是天然的，
// 竖线位置也就恒在正中间，不依赖任何宽度计算。
static void addStatBlock(QVBoxLayout* dst, QVBoxLayout** colL, QVBoxLayout** colR) {
    auto* blk = new QHBoxLayout;
    blk->setSpacing(10);

    // 竖线用普通 QWidget + 背景色，不用 QFrame::VLine —— QFrame 的线色走调色板，
    // 叠上全局深色样式表后各平台画出来的深浅不一致，给死颜色才可控。
    // setFixedWidth(1) 管住横向，纵向 Expanding 让它拉满整块高度。
    auto* sep = new QWidget;
    sep->setFixedWidth(1);
    sep->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    sep->setStyleSheet("background:#4a5360;");

    *colL = new QVBoxLayout;
    *colR = new QVBoxLayout;
    (*colL)->setSpacing(4);
    (*colR)->setSpacing(4);

    blk->addLayout(*colL, 1);
    blk->addWidget(sep);
    blk->addLayout(*colR, 1);
    dst->addLayout(blk);
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

// ---- 右侧面板宽度 ----
// 全文件所有「放不下 / 放得下」的宽度账都按这两个数算，改宽度只改这里，别去追注释里
// 的数字（以前是散在七八条注释里的 360/380，改一次宽度就得挨个改，漏一条就成假注释）。
// 2026-09-23 从 360/380 加宽到 420/440：从左边图像区挤 60px 过来，让「标签+两个输入框」
// 那几行松快些（死节那行原本只剩 ~30px 余量，再加一个字就贴边）。
// ⚠ 加宽【不省高度】：这些行本来就是一行一个 QHBoxLayout、不换行，宽度富余多少都不影响
//   纵向高度。想少滚动得把行排成两列（那是另一件事），不是靠这里。
// panel 比 scroll 窄 20px：scroll 里得给竖直滚动条留位置，一点不留会被压出横向滚动条。
static constexpr int PANEL_W  = 420;
static constexpr int SCROLL_W = 440;

// 一行里两个输入框【共用一个】标签 —— 死节那行专用：左边数量、右边尺寸门槛。
// 跟 addSpinRowPair 的区别是那个给两个框各配一个标签；这里第二个框不配标签，
// 靠 prefix/suffix 自己说明（显示成 ">30 mm"），省下的宽度留给主标签。
// 宽度账（面板 PANEL_W=420，分组框内宽 ~402px）：
//   "死节(dongba)数量大于" ~136px + "2 个" ~70px + ">30 mm" ~87px + 两道间距 16px ≈ 309px，
//   余 ~93px。这笔余量是留给以后改动的，不是让人往这行塞字的 —— 塞之前先重算。
static void addSpinRowTwoBoxes(const QString& name,
                               int lo1, int hi1, int def1, const QString& suffix1,
                               int lo2, int hi2, int def2,
                               const QString& prefix2, const QString& suffix2,
                               QSpinBox** out1, QSpinBox** out2, QVBoxLayout* lay) {
    auto* box = new QWidget;
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto* lbl = new QLabel(name);
    lbl->setStyleSheet("color:#c8c8c8;");
    auto* sp1 = new QSpinBox;
    sp1->setRange(lo1, hi1);
    sp1->setValue(def1);
    if (!suffix1.isEmpty()) sp1->setSuffix(suffix1);
    auto* sp2 = new QSpinBox;
    sp2->setRange(lo2, hi2);
    sp2->setValue(def2);
    if (!prefix2.isEmpty()) sp2->setPrefix(prefix2);
    if (!suffix2.isEmpty()) sp2->setSuffix(suffix2);

    row->addWidget(lbl, 1);
    row->addWidget(sp1);
    row->addWidget(sp2);
    lay->addWidget(box);
    if (out1) *out1 = sp1;
    if (out2) *out2 = sp2;
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

    // ---- 右: 操作面板 ----
    // 右列分两层：上面是滚动区（只装设置项），下面钉着按钮行。
    // 原先只有一层(全塞在滚动区里)，设置项一多关机/重启就被顶到可视区外面 —— 分层
    // 的理由见下面 btnRow 那段。结论：设置项多高都不该把关机/重启顶出屏幕。
    auto* rightCol = new QVBoxLayout;
    rightCol->setSpacing(10);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(SCROLL_W);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;}");
    auto* panel = new QWidget;
    panel->setFixedWidth(PANEL_W);
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

    // 统计 —— 一整块 4×2，中间一条通到底的竖线，木板尺寸单独占最后一行：
    //   总检数       1234  │  合格率     99.0%
    //   不合格数       12  │  耗时       210ms
    //   GPU 温度  45.2°C  │  CPU 温度  43.1°C
    //   内存          41%  │  硬盘         14%
    //   木板尺寸          1200.0 × 600.0 mm
    // 木板尺寸放最下面而不是中间：它跨满整行，夹在两列中间会把竖线顶成两截。
    auto* grpStt = new QGroupBox(QString::fromUtf8("统计"), panel);
    auto* lStt   = new QVBoxLayout(grpStt);
    lStt->setSpacing(6);

    QVBoxLayout* colL = nullptr;
    QVBoxLayout* colR = nullptr;
    addStatBlock(lStt, &colL, &colR);
    // 左列：计数 + 系统状态          右列：比率/耗时 + 系统状态
    _statTotal   = addStatRow(QString::fromUtf8("总检数"),   colL);
    _statNg      = addStatRow(QString::fromUtf8("不合格数"), colL);
    _statGpuTemp = addStatRow(QString::fromUtf8("GPU 温度"), colL);
    _statMem     = addStatRow(QString::fromUtf8("内存"),     colL);
    _statRate    = addStatRow(QString::fromUtf8("合格率"),   colR);
    _statCycle   = addStatRow(QString::fromUtf8("耗时"),     colR);
    _statCpuTemp = addStatRow(QString::fromUtf8("CPU 温度"), colR);
    _statDisk    = addStatRow(QString::fromUtf8("硬盘"),     colR);

    // 值长（1200.0 × 600.0 mm），不配对，占整行
    _statDims = addStatRow(QString::fromUtf8("木板尺寸"), lStt);
    v->addWidget(grpStt);

    // 工人设置
    auto* grpSet = new QGroupBox(QString::fromUtf8("工人设置"), panel);
    auto* lSet   = new QVBoxLayout(grpSet);
    // 七个类的数量阈值 —— 竖排成一列
    // (并排放不下: 面板宽 PANEL_W=420, 每个「标签+输入框」约 150px, 三个就要 466px)
    // 活节 = 节扣发白、按不掉, 不影响使用; 死节 = 节扣没掉但一按就掉
    // 括号里的拼音是模型/日志里那个类的名字(labels.txt、推理日志、图上画的都是它)，
    // 现场排查时不用再猜「活节对应哪个英文名」。
    _jiebaSpin  = addSpinRow(QString::fromUtf8("活节(jieba)数量大于"), 0, 50, 10, lSet);
    // 死节/破洞/缺边/树皮/发白 五行是同一个形状：左边数量阈值、右边尺寸门槛。
    // 2026-09-23 现场定的 —— 除了死节本来就是数量规则，另外四个原本走【面积和占比】，
    // 当天全改成了这个形状（为什么改见 postprocessor.cpp 的函数头注释）。
    // 两半是同一条规则的上下游：先按尺寸筛掉小的，再数个数，缺一个都没意义。
    // 右框显示成 ">30 mm" 是拿 prefix 拼的，不用再加一个「且大于」的标签占宽度
    // （宽度账见 addSpinRowTwoBoxes —— 面板加宽前这行只剩 ~30px 余量，加不下那 3 个字；
    //   现在 PANEL_W 给到 420 有余量了，但没必要加，符号比多一个标签清楚）。
    addSpinRowTwoBoxes(QString::fromUtf8("死节(dongba)数量大于"),
                       0, 50, 2, QString::fromUtf8(" 个"),
                       0, 500, 30, ">", " mm",
                       &_dongbaSpin, &_dongbaMinLenSpin, lSet);
    // 小油疤(黑色油滴到板上, 板子不碎) 数量阈值 —— 数量 > 此值判 NG，没有尺寸门槛
    _heibaSpin = addSpinRow(QString::fromUtf8("小油疤(heiba)数量大于"), 0, 500, 24, lSet);
    addSpinRowTwoBoxes(QString::fromUtf8("破洞(dongban)数量大于"),
                       0, 500, 2, QString::fromUtf8(" 个"),
                       0, 500, 30, ">", " mm",
                       &_dongbanSpin, &_dongbanMinLenSpin, lSet);
    // 标注备注 —— 紧跟在「破洞」下面: 说的是这条规则收哪些缺陷, 放远了就对不上号
    // (大油疤在 labelme 里也标成 dongban, 所以跟着破洞一起判)
    auto* holeHint = new QLabel(QString::fromUtf8("（大油疤归到破洞里面）"), grpSet);
    holeHint->setWordWrap(true);
    holeHint->setStyleSheet("color:#909090; font-size:12px;");
    lSet->addWidget(holeHint);
    addSpinRowTwoBoxes(QString::fromUtf8("缺边(quebian)数量大于"),
                       0, 500, 2, QString::fromUtf8(" 个"),
                       0, 500, 30, ">", " mm",
                       &_quebianSpin, &_quebianMinLenSpin, lSet);
    addSpinRowTwoBoxes(QString::fromUtf8("树皮(shupi)数量大于"),
                       0, 500, 99, QString::fromUtf8(" 个"),
                       0, 500, 30, ">", " mm",
                       &_shupiSpin, &_shupiMinLenSpin, lSet);
    addSpinRowTwoBoxes(QString::fromUtf8("发白(fabai)数量大于"),
                       0, 500, 99, QString::fromUtf8(" 个"),
                       0, 500, 30, ">", " mm",
                       &_fabaiSpin, &_fabaiMinLenSpin, lSet);
    // 一条提示罩住上面五行，不逐行重复 —— 五行的左边、右边语义完全一样。
    // ⚠ 这行必须短，一行就好：整个面板（含最底下的关机/重启）都塞在 QScrollArea 里，
    //   这里多占一行，底下那排就往下滚一行 —— 而滚出去的偏偏是关机/重启，
    //   全界面最不能猜错、也最不该要人找的两个键。
    // 所以面板上只留这两条；「最长边怎么算」「没过门槛照样画框、只是框线暗一档」
    // 这些解释不再占面板高度。
    auto* gateHint = new QLabel(
        QString::fromUtf8("（左边=数量，右边=直径：小于该直径的过滤掉，不算数）"), grpSet);
    gateHint->setWordWrap(true);
    gateHint->setStyleSheet("color:#909090; font-size:12px;");
    lSet->addWidget(gateHint);
    // 组合规则: 单类都没超、但两类加起来超了也判 NG（跟上面单类同一个「大于」口径）
    _jiebaDongbaSpin = addSpinRow(QString::fromUtf8("活节+死节数量大于"), 0, 100, 6, lSet);
    // 板长/板宽最小尺寸（横排省空间）：测出长/宽低于此值判 NG
    // 默认 1200/600 = 整板尺寸本身，即「比整板小就判 NG」（不再是原先的整板一半）
    addSpinRowPair(QString::fromUtf8("板长小于"), 0, 2000, 1200, " mm", &_lenSpin,
                   QString::fromUtf8("板宽小于"), 0, 2000, 600, " mm", &_widSpin, lSet);
    // 原始图/结果图保存 %：默认隐藏，开发者模式开关开启（密码正确）后才显示。
    // 两个初值都是 0 —— 也就是「解锁之后默认也不存 OK 板」，要抽样得工程师自己往里填。
    // 注意：这两个值【没有】持久化（下面那 17 个键里没它俩），所以每次启动都回到 0，
    // 现场调过也不留（ini 里那个 raw_save_pct 是死键，跟这行没关系）——
    // 要让它记住得另加 load/save + connect。
    _rawSpin    = addSpinRow(QString::fromUtf8("原始图保存 %"), 0, 100, 0, lSet, &_rawRow);
    _resultSpin = addSpinRow(QString::fromUtf8("结果图保存 %"), 0, 100, 0, lSet, &_resultRow);
    _rawRow->setVisible(false);
    _resultRow->setVisible(false);
    // 存图总开关：默认关，开启需密码（防止工人误开把硬盘写满）
    _saveChk = new QCheckBox(QString::fromUtf8("开发者模式（存图开关）"), grpSet);
    _saveChk->setStyleSheet(
        QString::fromUtf8("QCheckBox{color:#e0e0e0;} QCheckBox::indicator{width:18px;height:18px;}"));
    lSet->addWidget(_saveChk);
    // 存图保护提示：磁盘不足 / 目录超 60G 停存后显示，文案带具体原因（setSaveBlocked 填）
    _saveBlocked = new QLabel(QString::fromUtf8("⚠ 存图已暂停"), grpSet);
    _saveBlocked->setStyleSheet(QString::fromUtf8("color:#ff8080; font-size:14px;"));
    _saveBlocked->setVisible(false);
    lSet->addWidget(_saveBlocked);
    v->addWidget(grpSet);

    // 相机调参（工程师）
    auto* grpCam = new QGroupBox(QString::fromUtf8("相机调参"), panel);
    auto* lCam   = new QVBoxLayout(grpCam);
    // 曝光/增益并成一行省纵向空间。单位写死在标签里（不用 suffix）：两个框各带后缀
    // 会把宽度撑开，面板宽 PANEL_W 放不下两列。
    // 上限 24dB 是 MV-CS050-60GC 的标称增益范围(0~24dB, V5 高满阱模式只有 12.8)。
    // 原先写死的 30 没有任何出处, 提示里那句 0-300 更离谱, 一起对齐到这里。
    addSpinRowPair(QString::fromUtf8("曝光(us)"), 0, 100000, 6000, "", &_expoSpin,
                   QString::fromUtf8("增益(dB)"), 0, 24, 0, "", &_gainSpin, lCam);
    // 增益提示单独占一行: 面板宽 PANEL_W, 这么长的说明塞进标签会把输入框挤没
    auto* gainHint = new QLabel(
        QString::fromUtf8("（增益范围0-24，0 是默认；除非太暗，否则不要动默认 0）"), grpCam);
    gainHint->setWordWrap(true);
    gainHint->setStyleSheet("color:#909090; font-size:12px;");
    lCam->addWidget(gainHint);
    v->addWidget(grpCam);

    // ---- 设置持久化: 存到当前目录 config.ini（可见文件，重启后保留） ----
    // 下面这些 value(key, 默认值) 里的默认值就是「出厂值」：没有 config.ini 时用它，
    // 而上面各框构造时给的那个数其实永远被这里覆盖（两个数保持一致只是为了别读混）。
    // 数值与 include/config.h 里的同名常量对齐（config.h 是给没走界面那条路的地方用的）。
    QSettings s(QStringLiteral("config.ini"), QSettings::IniFormat);
    _jiebaSpin->setValue(s.value("jieba_max", 10).toInt());
    _dongbaSpin->setValue(s.value("dongba_max", 2).toInt());
    _dongbaMinLenSpin->setValue(s.value("dongba_min_len_mm", 30).toInt());
    _heibaSpin->setValue(s.value("heiba_max", 24).toInt());
    _dongbanSpin->setValue(s.value("dongban_max_count", 2).toInt());
    _dongbanMinLenSpin->setValue(s.value("dongban_min_len_mm", 30).toInt());
    _quebianSpin->setValue(s.value("quebian_max_count", 2).toInt());
    _quebianMinLenSpin->setValue(s.value("quebian_min_len_mm", 30).toInt());
    _shupiSpin->setValue(s.value("shupi_max_count", 99).toInt());
    _shupiMinLenSpin->setValue(s.value("shupi_min_len_mm", 30).toInt());
    _fabaiSpin->setValue(s.value("fabai_max_count", 99).toInt());
    _fabaiMinLenSpin->setValue(s.value("fabai_min_len_mm", 30).toInt());
    _jiebaDongbaSpin->setValue(s.value("jieba_dongba_max", 6).toInt());
    _lenSpin->setValue(s.value("min_len_mm", 1200).toInt());
    _widSpin->setValue(s.value("min_wid_mm", 600).toInt());
    _expoSpin->setValue(s.value("exposure_us", 6000).toInt());
    _gainSpin->setValue(s.value("gain_db", 0).toInt());
    connect(_jiebaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_max", v); });
    connect(_dongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongba_max", v); });
    connect(_dongbaMinLenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongba_min_len_mm", v); });
    connect(_heibaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("heiba_max", v); });
    connect(_dongbanSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_max_count", v); });
    connect(_dongbanMinLenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("dongban_min_len_mm", v); });
    connect(_quebianSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("quebian_max_count", v); });
    connect(_quebianMinLenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("quebian_min_len_mm", v); });
    connect(_shupiSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("shupi_max_count", v); });
    connect(_shupiMinLenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("shupi_min_len_mm", v); });
    connect(_fabaiSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("fabai_max_count", v); });
    connect(_fabaiMinLenSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("fabai_min_len_mm", v); });
    connect(_jiebaDongbaSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [](int v) { QSettings(QStringLiteral("config.ini"), QSettings::IniFormat).setValue("jieba_dongba_max", v); });
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

    // 操作按钮：四个并成一行，钉在右列最底下 —— 注意是在【滚动区外面】。
    // 原先这行是加进 panel 的，也就是在 QScrollArea 里面：设置项一多、整列超过屏幕高，
    // 关机/重启就滑到可视区底下去了，得先拖滚动条才够得着(注释里说的「不裁掉」只是
    // 「滚得到」，不等于「看得见」)。挪到滚动区外面之后，上面内容多高都跟它无关，
    // 永远贴在屏幕底 —— 而这两个键恰恰是全界面最不能猜错、最不该要人找的。
    // 并成一行(而不是两行两列)是上一版为省高度做的，跟这件事无关，保留：
    // 一行约 42px、两行约 100px，省下的 ~58px 还给了滚动区，设置项能少滚一点。
    // 宽度交给 stretch 按字数分配(4字:2字:2字:4字 = 3:2:2:3)，不写死 min-width ——
    // 写死 130px 的话四个要 520px，右列宽 SCROLL_W 根本放不下。
    // 实测每键约 90/60/60/90px 宽、42px 高，鼠标点是够用的。
    // 不用图标: emoji 在 Jetson 上没装 Noto Color Emoji 会显示成方框，
    // 而「关机/重启」画成电源/回转箭头是全界面最不能猜错的两个键 —— 猜错就是直接断电。
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    // 父窗口给 this 而不是 panel：它们马上要被放进 rightCol(挂在 this 上)，
    // 再挂在 panel 底下只会被 Qt 重新认领一次，写清楚省得看的人以为按钮还在面板里。
    auto* snap = new QPushButton(QString::fromUtf8("手动拍照"), this);
    snap->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:white;"
                          "background:#2e8b57; border-radius:6px;"));
    auto* exit = new QPushButton(QString::fromUtf8("退出"), this);
    exit->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:white;"
                          "background:#c0392b; border-radius:6px;"));
    btnRow->addWidget(snap, 3);
    btnRow->addWidget(exit, 2);
    // 拍照/退出 与 关机/重启 之间留一道空档：同处一行后全靠这点间距分组，
    // 没有它「退出」和「关机」会挨着，误触代价不小。
    btnRow->addSpacing(16);
    auto* shutdownBtn = new QPushButton(QString::fromUtf8("关机"), this);
    shutdownBtn->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:#ffd2d2;"
                          "background:#7a1f1f; border-radius:6px;"));
    auto* rebootBtn = new QPushButton(QString::fromUtf8("重启电脑"), this);
    rebootBtn->setStyleSheet(
        QString::fromUtf8("font-size:16px; font-weight:bold; padding:9px 6px; color:#ffd2d2;"
                          "background:#6b4a1f; border-radius:6px;"));
    btnRow->addWidget(shutdownBtn, 2);
    btnRow->addWidget(rebootBtn, 3);
    // panel 里留一条弹簧：设置项比滚动区矮时把内容顶到上边，不居中飘着
    v->addStretch(1);

    // 右列组装：上=滚动区(吃掉全部余高)，下=按钮行(固定高度，永远可见)。
    // 顺序就是上下顺序，addWidget 的第二个参数是伸缩比例：滚动区 1、按钮行 0。
    // ⚠ 以后往 panel 里加东西不用再担心把关机键顶下去 —— 加多少都在滚动区里面。
    scroll->setWidget(panel);
    rightCol->addWidget(scroll, 1);
    rightCol->addLayout(btnRow, 0);
    root->addLayout(rightCol, 0);

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
void MainWindow::setSaveBlocked(bool blocked, const QString& why) {
    if (!_saveBlocked) return;
    if (blocked) {
        // 原因写进提示里：现场看到「存图已暂停」得知道是盘满了还是目录到 60G 了，
        // 前者要清别的目录、后者等清理脚本或调大上限，处置方式不一样
        _saveBlocked->setText(why.isEmpty() ? QString::fromUtf8("⚠ 存图已暂停")
                                            : QString::fromUtf8("⚠ 存图已暂停：") + why);
    }
    _saveBlocked->setVisible(blocked);
}

// ============================================================
// 工人设置读取
// ============================================================
int MainWindow::jiebaMaxCount() const          { return _jiebaSpin->value(); }
int MainWindow::dongbaMaxCount() const         { return _dongbaSpin->value(); }
int MainWindow::dongbaMinLenMm() const         { return _dongbaMinLenSpin->value(); }
int MainWindow::heibaMaxCount() const          { return _heibaSpin->value(); }
int MainWindow::dongbanMaxCount() const        { return _dongbanSpin->value(); }
int MainWindow::dongbanMinLenMm() const        { return _dongbanMinLenSpin->value(); }
int MainWindow::quebianMaxCount() const        { return _quebianSpin->value(); }
int MainWindow::quebianMinLenMm() const        { return _quebianMinLenSpin->value(); }
int MainWindow::shupiMaxCount() const          { return _shupiSpin->value(); }
int MainWindow::shupiMinLenMm() const          { return _shupiMinLenSpin->value(); }
int MainWindow::fabaiMaxCount() const          { return _fabaiSpin->value(); }
int MainWindow::fabaiMinLenMm() const          { return _fabaiMinLenSpin->value(); }
int MainWindow::jiebaDongbaMaxCount() const    { return _jiebaDongbaSpin->value(); }
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
