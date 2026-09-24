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

    /** 整体 NG 判定。全部按【数量】判：jieba/heiba 不管大小全算，dongba/dongban/
     *  shupi/fabai/quebian 先按各自尺寸门槛过滤掉小的再数；dongban 另有第二道更严的
     *  数量规则（大破洞或大油疤，门槛/上限见 dongbanBigMinLenMm / dongbanBigMaxCount）；
     *  板长/板宽按测得尺寸；其余类默认 OK。reason 输出 NG 原因。
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
    // 判定用的 7 个类全是「数量」规则：MAX_COUNT 是数量上限，MIN_LEN_MM 是尺寸门槛
    // （检测框最长边换算成毫米，短于此值的不计数，0 = 不过滤）。jieba/heiba 没有门槛这一半。
    // 尺寸门槛的毫米换算是标定值（config.h MM_PER_PX），相机装高装低它就偏 —— 见那里的注释。

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
    /** 破洞的第二道数量规则，现场叫【一票否决】（行名/原因串：大破洞或大油疤）：
     *  最长边超过此值(mm)的才算数，块数超过 dongbanBigMaxCount 判 NG。跟上面那条
     *  (dongbanMaxCount/dongbanMinLenMm) 是同一个类的两道门槛：那条管「小的多」，
     *  这条管「单块太大」。形状完全一样，没有单开一套逻辑 ——「一票否决」说的是用意
     *  （一个 40mm 的大洞比两个 30mm 的严重），不是实现。
     *  名字里带「大油疤」不是笔误：模型没有大油疤这个类，它并进 dongban 一起标，
     *  这条实际管的是破洞 + 大油疤两样。 */
    void setDongbanBigMaxCount(int n) { _dongban_big_max_count = n; }
    int  dongbanBigMaxCount() const   { return _dongban_big_max_count; }
    void setDongbanBigMinLenMm(int mm) { _dongban_big_min_len_mm = mm; }
    int  dongbanBigMinLenMm() const    { return _dongban_big_min_len_mm; }

    void setHeibaMaxCount(int n) { _heiba_max_count = n; }
    int  heibaMaxCount() const   { return _heiba_max_count; }

    void setShupiMaxCount(int n) { _shupi_max_count = n; }
    int  shupiMaxCount() const   { return _shupi_max_count; }
    void setShupiMinLenMm(int mm) { _shupi_min_len_mm = mm; }
    int  shupiMinLenMm() const    { return _shupi_min_len_mm; }

    void setFabaiMaxCount(int n) { _fabai_max_count = n; }
    int  fabaiMaxCount() const   { return _fabai_max_count; }
    void setFabaiMinLenMm(int mm) { _fabai_min_len_mm = mm; }
    int  fabaiMinLenMm() const    { return _fabai_min_len_mm; }

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
    /** 某个类的尺寸门槛(mm)。没这道门槛的类返回 0，也就是「全都算」。 */
    int sizeGateMm(const std::string& name) const;

    /** 这一块缺陷算不算数：有没有过它那个类的尺寸门槛（没门槛的类一律算）。
     *  isNG 拿它筛数量、draw 拿它挑框色 —— 判定和显示必须同一个口径，
     *  所以「怎么算过门槛」只留这一处，两个调用点不可能走偏。 */
    bool countsTowardRule(const Defect& d) const;

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
    int   _heiba_max_count        = Config::HEIBA_MAX_COUNT;
    int   _shupi_max_count        = Config::SHUPI_MAX_COUNT;
    int   _shupi_min_len_mm       = Config::SHUPI_MIN_LEN_MM;
    int   _fabai_max_count        = Config::FABAI_MAX_COUNT;
    int   _fabai_min_len_mm       = Config::FABAI_MIN_LEN_MM;
    int   _quebian_max_count      = Config::QUEBIAN_MAX_COUNT;
    int   _quebian_min_len_mm     = Config::QUEBIAN_MIN_LEN_MM;
    int   _min_length_mm          = Config::MIN_LENGTH_MM;
    int   _min_width_mm           = Config::MIN_WIDTH_MM;
};
