#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include "trtyolo.hpp"
#include "config.h"

struct Defect {
    int cls_id;
    float conf;
    cv::Rect box;
    std::string name;
};

class Postprocessor {
public:
    Postprocessor(float thresh, const std::vector<std::string>& classes)
        : _thresh(thresh), _classes(classes) {}

    std::vector<Defect> process(const trtyolo::DetectRes& res,
                                cv::Mat& frame, const cv::Size& size);

    /** 整体 NG 判定。全部按【数量】判：只有 jieba 不管大小全算，dongba/dongban/heiba/
     *  shupi/fabai/quebian 先按各自尺寸门槛过滤掉小的再数（dongban/fabai/shupi 还多一道
     *  置信度门槛，见 dongbanMinConf/fabaiMinConf/shupiMinConf）；dongban 和 heiba 各有
     *  第二道更严的数量规则（大破洞 / 大油疤，门槛/上限见
     *  dongbanBigMinLenMm / dongbanBigMaxCount 和 heibaBigMinDiagMm / heibaBigMaxCount）；
     *  板长/板宽按测得尺寸；其余类默认 OK。
     *  reason 输出 NG 原因。
     *  ⚠ 2026-09-23 起本项目【没有面积规则了】—— dongban/quebian/shupi/fabai 原本都按
     *    「面积和占整图比例」判，现场逐个改成了数量 + 尺寸门槛（为什么改见 postprocessor.cpp
     *    的函数头注释）。所以这里不再需要图像尺寸。
     *  @param len_mm / wid_mm  测量出的板长/板宽（0=未测到，不判尺寸 NG）
     *  @param size_only  出参，传了才填：NG 是否【只】由尺寸引起（缺陷规则一条都没触发）。
     *                    存图那边靠它把纯尺寸 NG 排除掉，见 main.cpp 存图段。 */
    bool isNG(const std::vector<Defect>& defects,
              float len_mm, float wid_mm, std::string& reason,
              bool* size_only = nullptr) const;
    void draw(cv::Mat& frame, const std::vector<Defect>& defects);

    /** 画左上角统计面板：类别数 + 各类框数。有尺寸门槛的类写成「算数/全部」
     *  （dongba 2/4 = 画了 4 个框、其中 2 个过了门槛），没门槛的写 xN。
     *  （没有面积行了——见 isNG 上面那段） */
    void drawSummary(cv::Mat& frame, const std::vector<Defect>& defects);

    // ---- 工人可调阈值（运行时可改，界面输入框控制；每块板由 main.cpp 下发）----
    // 判定用的 7 个类全是「数量」规则：MAX_COUNT 是数量上限，MIN_* 是尺寸门槛
    // （检测框量出来的毫米数短于此值的不计数，0 = 不过滤）。7 个里只有 jieba 没有尺寸门槛
    // 这一半 —— 其余六个里 dongba/dongban/quebian/shupi/fabai 量【最长边】，heiba 量
    // 【对角线】（全项目唯一一个，两条 heiba 规则共用，见 gateUsesDiagonal）。
    // 尺寸门槛的毫米换算是标定值（config.h MM_PER_PX），相机装高装低它就偏 —— 见那里的注释。
    // *_MIN_CONF 是另一种门槛（模型置信度，不是尺寸）：dongban / shupi / fabai 各有一道 ——
    // 这三道是全项目仅有的不看框大小、只看模型把握的门槛。

    void setJiebaMaxCount(int n) { _jieba_max_count = n; }
    int  jiebaMaxCount() const   { return _jieba_max_count; }

    void setDongbaMaxCount(int n) { _dongba_max_count = n; }
    int  dongbaMaxCount() const   { return _dongba_max_count; }
    void setDongbaMinLenMm(int mm) { _dongba_min_len_mm = mm; }
    int  dongbaMinLenMm() const    { return _dongba_min_len_mm; }

    void setDongbanMaxCount(int n) { _dongban_max_count = n; }
    int  dongbanMaxCount() const   { return _dongban_max_count; }
    void setDongbanMinLenMm(int mm) { _dongban_min_len_mm = mm; }
    int  dongbanMinLenMm() const    { return _dongban_min_len_mm; }
    /** 破洞的第二道数量规则，现场叫【一票否决】（行名/原因串：大破洞）：
     *  最长边超过此值(mm)的才算数，块数超过 dongbanBigMaxCount 判 NG。跟上面那条
     *  (dongbanMaxCount/dongbanMinLenMm) 是同一个类的两道门槛：那条管「小的多」，
     *  这条管「单块太大」。形状完全一样，没有单开一套逻辑 ——「一票否决」说的是用意
     *  （一个 40mm 的大洞比两个 30mm 的严重），不是实现。
     *  ⚠ 2026-10-03 之前这条叫「大破洞或大油疤」，因为大油疤当时并进 dongban 一起标。
     *    那天标注挪到了 heiba，这条就只剩破洞；大油疤的那一半是下面的 heibaBig*。 */
    void setDongbanBigMaxCount(int n) { _dongban_big_max_count = n; }
    int  dongbanBigMaxCount() const   { return _dongban_big_max_count; }
    void setDongbanBigMinLenMm(int mm) { _dongban_big_min_len_mm = mm; }
    int  dongbanBigMinLenMm() const    { return _dongban_big_min_len_mm; }
    /** dongban 破洞的【置信度门槛】(2026-10-02 加的)：形状跟下面 shupi/fabai 那两道
     *  完全一样（同一张表 minConfFor、同一处口径 countsTowardRule），只是换了个类。
     *  ⚠ 它只卡上面那条「破洞」(dongbanMaxCount)。「一票否决」(dongbanBigMaxCount) 在
     *    isNG 里是自己一条 if，不走 countsTowardRule，所以【不】应用这道门槛 ——
     *    一个概率不够的 40mm 洞照样能触发一票否决。（加它的那天注释写的是「两条都不数」，
     *    2026-10-03 照代码改了过来：不是行为变了，是注释本来就写反了。）
     *  0 = 关掉这道门槛。出厂那个数（现场定的）见 config.h 的 DONGBAN_MIN_CONF。 */
    void setDongbanMinConf(double c) { _dongban_min_conf = c; }
    double dongbanMinConf() const    { return _dongban_min_conf; }

    void setHeibaMaxCount(int n) { _heiba_max_count = n; }
    int  heibaMaxCount() const   { return _heiba_max_count; }
    /** heiba 小油疤的尺寸门槛（2026-10-03 加的）：【对角线】不过此值(mm)的不算数。
     *  0 = 不过滤 = 全数进 heibaMaxCount（= 加这道门槛之前的行为）。
     *  ⚠ 量的是对角线，跟大油疤那条是【同一把尺子】（别的类都是最长边）——
     *    代码里只有 gateUsesDiagonal 一处决定量法。理由见 config.h 的 HEIBA_BIG_MIN_DIAG_MM。
     *  ⚠ 界面上不提「对角线」（2026-10-03 现场定的），输入框后缀照通用的 " mm" 写。 */
    void setHeibaMinDiagMm(int mm) { _heiba_min_diag_mm = mm; }
    int  heibaMinDiagMm() const    { return _heiba_min_diag_mm; }
    /** heiba 的第二道数量规则，现场叫【大油疤】(2026-10-03 加的)：
     *  【对角线】超过此值(mm)的 heiba 才算数，块数超过 heibaBigMaxCount 判 NG。跟上面那条
     *  (heibaMaxCount) 是同一个类的两道门槛，形状跟 dongbanBig* 一模一样：那条管
     *  「小的多」（全部 heiba，够不够大都算），这条管「单块太大」。
     *  ⚠ 量的是对角线，不是最长边 —— 全项目唯一一处，理由见 config.h 的
     *    HEIBA_BIG_MIN_DIAG_MM（长条/方形都有，最长边会跟着缺陷朝向变）。
     *  ⚠ 两条各数各的：一块过门槛的大油疤【既】算进 heibaMaxCount 的 24【也】算进这条。
     *    这是 2026-10-03 定的（跟 dongban 那一对一致），不是漏了去重。
     *  ⚠ 门槛和上限填 0 的约定跟别处一样：上限 0 = 超过 0 个 = 至少 1 个就判；
     *    门槛 0 = 这条规则整个关掉（那时 isBigHeiba 对谁都是 false，大油疤照旧只算进
     *    小油疤那条 —— 不会出现「两条都不数」的空洞）。 */
    void setHeibaBigMaxCount(int n) { _heiba_big_max_count = n; }
    int  heibaBigMaxCount() const   { return _heiba_big_max_count; }
    void setHeibaBigMinDiagMm(int mm) { _heiba_big_min_diag_mm = mm; }
    int  heibaBigMinDiagMm() const    { return _heiba_big_min_diag_mm; }

    void setShupiMaxCount(int n) { _shupi_max_count = n; }
    int  shupiMaxCount() const   { return _shupi_max_count; }
    void setShupiMinLenMm(int mm) { _shupi_min_len_mm = mm; }
    int  shupiMinLenMm() const    { return _shupi_min_len_mm; }
    /** shupi 的【置信度门槛】(2026-09-30 加的)：形状跟下面 fabai 那道完全一样
     *  （同一张表 minConfFor、同一处口径 countsTowardRule），只是换了个类。
     *  0 = 关掉这道门槛。出厂那个数（现场定的）见 config.h 的 SHUPI_MIN_CONF。 */
    void setShupiMinConf(double c) { _shupi_min_conf = c; }
    double shupiMinConf() const    { return _shupi_min_conf; }

    void setFabaiMaxCount(int n) { _fabai_max_count = n; }
    int  fabaiMaxCount() const   { return _fabai_max_count; }
    void setFabaiMinLenMm(int mm) { _fabai_min_len_mm = mm; }
    int  fabaiMinLenMm() const    { return _fabai_min_len_mm; }
    /** fabai 的【置信度门槛】：模型的 fabai 概率不高于此值的不算数（0 = 关掉这道门槛，
     *  跟尺寸门槛的 0 一个意思）。界面上是「发白概率(置信度)」那个框。
     *  ⚠ 它跟 MIN_LEN_MM 那类尺寸门槛不是一回事：尺寸是从框上量出来的、跟着相机标定走；
     *    置信度是模型自己给的分、跟标定无关。两道门槛是「且」——都过了才算数。
     *  为什么最早给 fabai 开这一道：见 config.h 的 FABAI_MIN_CONF。 */
    void setFabaiMinConf(double c) { _fabai_min_conf = c; }
    double fabaiMinConf() const    { return _fabai_min_conf; }

    void setQuebianMaxCount(int n) { _quebian_max_count = n; }
    int  quebianMaxCount() const   { return _quebian_max_count; }
    void setQuebianMinLenMm(int mm) { _quebian_min_len_mm = mm; }
    int  quebianMinLenMm() const    { return _quebian_min_len_mm; }

    /** 测得板长/板宽小于此值(mm)判 NG */
    void setMinLengthMm(int n) { _min_length_mm = n; }
    int  minLengthMm() const   { return _min_length_mm; }
    void setMinWidthMm(int n) { _min_width_mm = n; }
    int  minWidthMm() const    { return _min_width_mm; }

private:
    /** 某个类的【检测下限】：低于它的检测根本不存在（不画框、不进统计、不参与判定）。
     *  默认就是构造时传进来的全局门槛（Config::CONF_THRESHOLD），只有 heiba 用自己那个
     *  更低的数（Config::HEIBA_MIN_CONF）。
     *  ⚠ 跟下面那两个门槛表【不是一层的东西】，别混：
     *      minDetectConf（这一道）——「这算不算一次检测」，不过的连画都不画；
     *      sizeGateMm / minConfFor ——「这次检测算不算数」，不过的照画、只是框线压暗。
     *    所以这个数只可能比全局【低】（放宽），那两个只可能比全局【严】。 */
    float minDetectConf(const std::string& name) const;

    /** 某个类的尺寸门槛(mm)。没这道门槛的类返回 0，也就是「全都算」。 */
    int sizeGateMm(const std::string& name) const;

    /** 某个类的置信度门槛。没这道门槛的类返回 0，也就是「全都算」。
     *  跟 sizeGateMm 是一对：那个管「多大才算」，这个管「多确定才算」。 */
    double minConfFor(const std::string& name) const;

    /** 这个类有没有【任何】门槛（尺寸或置信度都有份）。给 drawSummary 用：它靠这个决定
     *  写成「算数/全部」还是「xN」——只问 sizeGateMm 的话，一个类把尺寸门槛调成 0
     *  （不过滤）、只留置信度门槛时会被当成没门槛，面板写 x5 而实际只有 3 个算数。 */
    bool hasGate(const std::string& name) const;

    /** 这一块缺陷算不算数：它那个类的门槛（尺寸 + 置信度）全过了才算。
     *  isNG 拿它筛数量、draw 拿它挑框色、drawSummary 拿它算「算数/全部」——
     *  判定和显示必须同一个口径，所以「怎么算过门槛」只留这一处，三个调用点不可能走偏。 */
    bool countsTowardRule(const Defect& d) const;

    /** 这一块 heiba 算不算【大油疤】：对角线超过 _heiba_big_min_diag_mm 才算
     *  （量对角线，不是最长边 —— 见 config.h 的 HEIBA_BIG_MIN_DIAG_MM）。
     *  门槛 0 = 这条规则关掉，那时对谁都是 false —— 全落回「小油疤」那条，不会两块都不数。
     *  isNG（数大油疤）和 draw（给大油疤换框色）都用它，跟 countsTowardRule 一个道理：
     *  「怎么算大」只留这一处，判定和显示不可能走偏。 */
    bool isBigHeiba(const Defect& d) const;

    float _thresh;
    std::vector<std::string> _classes;
    // 下面这些初值只在「第一块板之前」有效：每块板都会把界面上的工人设置 setXxx 下来覆盖。
    // 数值的出处统一在 config.h，这里不再复述数字（以前写死在注释里，改了常量注释就成假的）。
    int   _jieba_max_count        = Config::JIEBA_MAX_COUNT;
    int   _dongba_max_count       = Config::DONGBA_MAX_COUNT;
    int   _dongba_min_len_mm      = Config::DONGBA_MIN_LEN_MM;
    int   _dongban_max_count      = Config::DONGBAN_MAX_COUNT;
    int   _dongban_min_len_mm     = Config::DONGBAN_MIN_LEN_MM;
    int   _dongban_big_max_count  = Config::DONGBAN_BIG_MAX_COUNT;
    int   _dongban_big_min_len_mm = Config::DONGBAN_BIG_MIN_LEN_MM;
    double _dongban_min_conf      = Config::DONGBAN_MIN_CONF;
    int   _heiba_max_count        = Config::HEIBA_MAX_COUNT;
    int   _heiba_min_diag_mm      = Config::HEIBA_MIN_DIAG_MM;
    int   _heiba_big_max_count    = Config::HEIBA_BIG_MAX_COUNT;
    int   _heiba_big_min_diag_mm  = Config::HEIBA_BIG_MIN_DIAG_MM;
    int   _shupi_max_count        = Config::SHUPI_MAX_COUNT;
    int   _shupi_min_len_mm       = Config::SHUPI_MIN_LEN_MM;
    double _shupi_min_conf        = Config::SHUPI_MIN_CONF;
    int   _fabai_max_count        = Config::FABAI_MAX_COUNT;
    int   _fabai_min_len_mm       = Config::FABAI_MIN_LEN_MM;
    double _fabai_min_conf        = Config::FABAI_MIN_CONF;
    int   _quebian_max_count      = Config::QUEBIAN_MAX_COUNT;
    int   _quebian_min_len_mm     = Config::QUEBIAN_MIN_LEN_MM;
    int   _min_length_mm          = Config::MIN_LENGTH_MM;
    int   _min_width_mm           = Config::MIN_WIDTH_MM;
};
