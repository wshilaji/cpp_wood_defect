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
     *  shupi/fabai/quebian 先按各自尺寸门槛过滤掉小的再数；另有 jieba+dongba 的组合数；
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

    /** 画左上角统计面板：类别数 + 各类框数（没有面积行了——见 isNG 上面那段） */
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

    /** jieba+dongba 数量之和超过此值判 NG（单类都没超也可能被这条拦住） */
    void setJiebaDongbaMaxCount(int n) { _jieba_dongba_max_count = n; }
    int  jiebaDongbaMaxCount() const   { return _jieba_dongba_max_count; }

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
    int   _heiba_max_count        = Config::HEIBA_MAX_COUNT;
    int   _shupi_max_count        = Config::SHUPI_MAX_COUNT;
    int   _shupi_min_len_mm       = Config::SHUPI_MIN_LEN_MM;
    int   _fabai_max_count        = Config::FABAI_MAX_COUNT;
    int   _fabai_min_len_mm       = Config::FABAI_MIN_LEN_MM;
    int   _quebian_max_count      = Config::QUEBIAN_MAX_COUNT;
    int   _quebian_min_len_mm     = Config::QUEBIAN_MIN_LEN_MM;
    int   _jieba_dongba_max_count = Config::JIEBA_DONGBA_MAX_COUNT;
    int   _min_length_mm          = Config::MIN_LENGTH_MM;
    int   _min_width_mm           = Config::MIN_WIDTH_MM;
};
