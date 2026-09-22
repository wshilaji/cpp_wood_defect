#!/bin/bash
# cleanup_images.sh — 存图目录容量看门狗（双保险里的【脚本那层】）
#
# 另一层是 SaveWorker 里的 C++ 检查（磁盘剩余 < 10G 或目录总量 ≥ 60G 就停写）。
# 两层分工：
#   C++ 层  → 保证「写的时候不会把盘写爆」，是硬闸，但只停不删；
#   本脚本  → 负责「删旧的」，把 raw/ + result/ 的总量压回水位以下。
# 只靠任何一层都不够：只有 C++ 那层，目录涨到 60G 就永远停写、得人工去删；
# 只有脚本这层，脚本没跑起来（没装 timer、进程被 kill）就没人拦。
#
# 口径（跟 C++ 层一致）：只算 output/raw/*.jpg 与 output/result/*.jpg 的字节和，
# 超过 CAP 就从最老的开始删，删到 KEEP 水位为止。
#
# 调用方式：
#   1) run.sh 启动时同步跑一次（在 exec 主程序【之前】）—— 这样主程序一起来看到的
#      目录就是已经降到水位以下的，C++ 那层不会在启动瞬间就因为「目录超 60G」停写。
#   2) systemd timer 每小时跑一次（见 install-systemd.sh）。
# 两处都会跑，所以用 mkdir 锁防并发；并发也只是白跑，不会删错。
#
# 参数：
#   --quiet   正常运行不打屏（给 run.sh / timer 用），出事的记录仍然写日志
# 环境变量（测试用，别在生产上乱设）：
#   OUTPUT_DIR        存图根目录，默认 <脚本所在目录>/output
#   CLEANUP_CAP_GB    上限，默认 60（与 SaveWorker::SAVE_CAP_BYTES 一致）
#   CLEANUP_KEEP_GB   删到的水位，默认 54（与 SaveWorker::SAVE_WATERMARK_BYTES 一致）
#
# 为什么删「最老」是安全的：主程序正在写的永远是最新的那几个文件，从最老端删
# 天然碰不到；再加一道「60 秒内动过的整组跳过」兜住时钟回拨之类的边角情况。
#
# 退出码：正常都返回 0。清理脚本失败不该拦住主程序启动（调用处另有 || true）。
#         参数错误返回 2。

set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUTPUT_DIR:-$DIR/output}"
CAP_GB="${CLEANUP_CAP_GB:-60}"
KEEP_GB="${CLEANUP_KEEP_GB:-54}"
LOG="$OUT/cleanup.log"
QUIET=0

case "${1:-}" in
    --quiet) QUIET=1 ;;
    "")      ;;
    *)       echo "用法: $0 [--quiet]" >&2; exit 2 ;;
esac

if ! [[ "$CAP_GB" =~ ^[0-9]+$ ]] || ! [[ "$KEEP_GB" =~ ^[0-9]+$ ]]; then
    echo "CLEANUP_CAP_GB / CLEANUP_KEEP_GB 必须是整数 GB" >&2
    exit 2
fi

CAP=$(( CAP_GB * 1024 * 1024 * 1024 ))
KEEP=$(( KEEP_GB * 1024 * 1024 * 1024 ))
MB() { echo $(( $1 / 1048576 )); }

log() {
    mkdir -p "$OUT" 2>/dev/null || return 0
    # 1MB 轮转：这脚本一小时跑一次、没事不写，日志长不快，但万一一直出事也别让它长成负担
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        mv -f "$LOG" "$LOG.1" 2>/dev/null || true
    fi
    printf '%s %s\n' "$(date '+%F %T')" "$*" >> "$LOG" 2>/dev/null || true
}

# 还没开始存图（新部署 / 目录被手工删了）：没事可做，静默退出
[ -d "$OUT/raw" ] && [ -d "$OUT/result" ] || exit 0

# ---- 防并发：run.sh 与 timer 有可能同一秒都进来 ----
# 用 mkdir 做原子锁，不用 flock(1)：mkdir 成功/失败本身就是原子的，不依赖任何外部命令，
# 也不用「exec 9>锁文件」那种写法 —— exec 只带重定向时若打不开文件，bash 会【直接退出脚本】，
# 而且那种写法里的 2>/dev/null 会把之后所有 stderr 一起吞掉，出问题连日志都没有。
# 锁目录是点开头的：主程序的 dirBytes() 跳过点开头项、本脚本 find 也只挑 raw/result 里的 *.jpg。
LOCK="$OUT/.cleanup.lock.d"
if ! mkdir "$LOCK" 2>/dev/null; then
    # 已经有锁。上次被 kill -9 会留下僵锁，超过 10 分钟就当僵锁清掉重来
    # （正常一次清理是秒级，10 分钟还挂着只能是僵的）。
    if [ -n "$(find "$LOCK" -maxdepth 0 -mmin +10 2>/dev/null)" ]; then
        rm -rf "$LOCK" 2>/dev/null || exit 0
        mkdir "$LOCK" 2>/dev/null || exit 0
    else
        [ "$QUIET" = 1 ] || echo "另一次清理正在跑，跳过"
        exit 0
    fi
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT

# ---- 一次 find 拿齐「mtime 字节数 路径」 ----
# 不再逐个 stat：几万个文件时逐个 fork/ext stat 是分钟级的，-printf 一趟就够。
# 用 %T@（mtime，含小数秒）排序而不是文件名：新旧两种命名（NG_/RAW_ 前缀 与 <id>_RAW/_NG 后缀）
# 混在一起时，只有 mtime 是两者都可靠的排序依据。
LIST="$(find "$OUT/raw" "$OUT/result" -maxdepth 1 -type f -name '*.jpg' -printf '%T@ %s %p\n' 2>/dev/null)"
[ -n "$LIST" ] || exit 0

# awk 汇总时用 printf "%.0f" 而不是 print：mawk 的默认 OFMT 是 %.6g，
# 5e10 这种量级会被打成 "5e+10"，bash 的 [ -gt ] 立刻报 integer expression expected。
TOTAL="$(printf '%s\n' "$LIST" | awk '{ s += $2 } END { printf "%.0f\n", s + 0 }')"

if [ "$TOTAL" -le "$CAP" ]; then
    [ "$QUIET" = 1 ] || echo "存图目录 $(MB "$TOTAL")MB / ${CAP_GB}G，无需清理"
    exit 0
fi

# ---- 按「板」分组 ----
# 一块板的两张图（<id>_RAW 与 <id>_NG|OK，旧格式 NG_<id> 与 RAW_<id>）必须同生共死：
# 只删掉一半，剩下那张既配不成对、又永远等不到另一半，纯属占着地方的垃圾。
# 组内最新 mtime 作为排序键 —— 一块板存完的时刻，也就是它「开始变老」的时刻。
#
# 输出【一行一个文件】（key|组最新mtime|组总字节|路径），不是一行一组再在 shell 里切分：
# 在 shell 里切一个空格分隔的字符串要么靠不加引号的 $var 分词（zsh 默认不拆！本机
# 开发用 zsh、目标机用 bash，这种差异本地不报错、上线才出错），要么靠 read -a
# （zsh 的 read 又没有 -a）。一行一个文件就完全没有切分这回事，两边行为一致。
GROUPS="$(printf '%s\n' "$LIST" | awk '
{
    mt = $1; sz = $2; path = $3
    base = path; sub(/.*\//, "", base)
    k = base; sub(/\.jpg$/, "", k)
    sub(/^(NG|RAW|OK)_/, "", k)      # 旧格式前缀
    sub(/_(NG|RAW|OK)$/, "", k)      # 新格式后缀
    if (!(k in bytes)) { bytes[k] = 0; newest[k] = mt }
    bytes[k] += sz
    if (mt + 0 > newest[k] + 0) newest[k] = mt
    files[k] = (k in files ? files[k] " " : "") path
}
END {
    for (k in bytes) {
        n = split(files[k], fa, " ")     # 到 END 才拆：此时 bytes[k] 才是全组的
        for (i = 1; i <= n; i++)
            printf "%s|%s|%.0f|%s\n", k, newest[k], bytes[k], fa[i]
    }
}
')"

NEED=$(( TOTAL - KEEP ))              # 要腾出来的字节数
CUTOFF=$(( $(date +%s) - 60 ))        # 60 秒内动过的组不算老
freed=0
ndel=0
nfail=0
cur=""
skip=0

# 从最老的组开始删，删够 NEED 就停（不是删到 54G 就多删，是刚好回到水位）
# 排序 -k2 (组最新 mtime) 在前，保证「老组在前」；同 mtime 的按 key 排，
# 保证同一组的文件必定相邻 —— 分组的完整性就靠这个。
while IFS='|' read -r key newest gsize path; do
    if [ "$key" != "$cur" ]; then          # 换到下一组
        cur="$key"
        [ "$freed" -ge "$NEED" ] && break  # 已腾够：整组整组地停，不拆散下一组
        if [ "${newest%.*}" -gt "$CUTOFF" ]; then
            skip=1                          # 太新（可能还在写）：整组留下
            continue
        fi
        skip=0
        freed=$(( freed + gsize ))          # 组开始就记账：本组整个都会被删掉
    fi
    [ "$skip" = 1 ] && continue
    if rm -f -- "$path" 2>/dev/null; then
        ndel=$(( ndel + 1 ))
    else
        # 删除失败通常是一整批一起失败（只读挂载/权限/盘满），逐条记的话几万条会把
        # 日志刷爆、把真正有用的信息埋掉，所以只记第一条，条数在循环后汇总。
        nfail=$(( nfail + 1 ))
        [ "$nfail" = 1 ] && log "删除失败(首条，其余同类只计数): $path"
    fi
done < <(printf '%s\n' "$GROUPS" | sort -t'|' -k2,2n -k1,1)

AFTER=$(( TOTAL - freed ))
if [ "$ndel" -gt 0 ]; then
    msg="清理 $ndel 个文件（$(MB "$freed")MB）：raw+result $(MB "$TOTAL")MB → $(MB "$AFTER")MB（上限 ${CAP_GB}G，水位 ${KEEP_GB}G）"
    log "$msg"
    [ "$QUIET" = 1 ] || echo "$msg"
fi
[ "$nfail" -gt 0 ] && log "删除失败共 $nfail 个（权限/只读挂载？）"

# 删到水位以下才算成功。够不到只有两种可能：要么剩余的全是 60 秒内的新文件
# （正常，说明存量本来就基本是新的），要么 rm 一直在失败（权限/只读挂载）。
# 后者必须在日志里留痕，否则就成了「脚本说跑过了但盘还是满的」。
if [ "$AFTER" -gt "$CAP" ]; then
    log "警告: 清理后仍 $(MB "$AFTER")MB > ${CAP_GB}G —— 可能是文件太新或删除失败，请查权限/挂载"
fi

exit 0
