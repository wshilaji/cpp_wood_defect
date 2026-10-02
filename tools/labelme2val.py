#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一个装满 labelme json + 原图的目录，转成 YOLO 检测格式的 val 集。

产物（<dst>/ 下）:
    images/*.jpg     原图（拷贝一份，跟 json 目录脱钩）
    labels/*.txt     YOLO 标注： class_id cx cy w h（都归一化到 0~1）
    data.yaml        ultralytics 直接吃的描述文件
    report.txt       每类实例数/图片数 —— 转完先看这个

为什么不用 labelme2yolo：
    它的类别顺序是**每次运行现攒的**（set 迭代 + 目录遍历顺序，PYTHONHASHSEED 一变就变），
    同一个目录跑两次可能给出不同的 names 顺序。而本项目的 `include/config.h` 的 CLASSES
    必须跟模型的 class_id 严格对齐 —— 顺序错一次就是整体错位，判定跟着错，而且不报错。
    所以这里把顺序从 config.h 直接读出来钉死，并且**碰到不认识的类名直接停机**。

用法:
    python3 tools/labelme2val.py --src ~/data/20261002val --dst ~/datasets/frozen_val_v1

    # config.h 不在默认位置时:
    python3 tools/labelme2val.py --src ... --dst ... --config-h /path/to/include/config.h
"""
import argparse
import json
import re
import shutil
import sys
from collections import Counter
from pathlib import Path

IMG_EXT = {".jpg", ".jpeg", ".png", ".bmp"}

# config.h 读不到时的兜底（2026-10-02 抄自 include/config.h 的 CLASSES）。
# 正常路径是从 config.h 现读，这里只是防止脚本被单独拷走。
FALLBACK_CLASSES = [
    "dongban", "dongba", "heiba", "shuwen", "liefeng", "quebian", "shupi",
    "jieba", "piwenba", "baowen", "fabai", "heiban", "banwen", "banwenba",
]


def classes_from_config_h(path: Path):
    """从 config.h 里抠出 CLASSES = { "a", "b", ... } 的顺序。"""
    if not path.is_file():
        return None
    m = re.search(r"CLASSES\s*=\s*\{(.*?)\}", path.read_text(encoding="utf-8"), re.S)
    if not m:
        return None
    names = re.findall(r'"([^"]+)"', m.group(1))
    return names or None


def shape_bbox(shape):
    """labelme 的 shape → (x1, y1, x2, y2)。矩形直接用；多边形取外接框。"""
    pts = shape.get("points") or []
    if not pts:
        return None
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="放 json + 原图的目录")
    ap.add_argument("--dst", required=True, help="输出 val 集的目录（会被创建）")
    ap.add_argument("--config-h", default=str(Path(__file__).resolve().parent.parent
                                             / "include" / "config.h"))
    args = ap.parse_args()

    names = classes_from_config_h(Path(args.config_h))
    origin = args.config_h
    if not names:
        names = FALLBACK_CLASSES
        origin = "脚本内兜底常量（config.h 没读到）"
    cls2id = {c: i for i, c in enumerate(names)}

    src = Path(args.src).expanduser().resolve()
    dst = Path(args.dst).expanduser().resolve()
    (dst / "images").mkdir(parents=True, exist_ok=True)
    (dst / "labels").mkdir(parents=True, exist_ok=True)

    jsons = sorted(src.glob("*.json"))
    if not jsons:
        sys.exit(f"{src} 下没找到 *.json")

    print(f"类顺序来源: {origin}")
    print("顺序: " + ", ".join(f"{i}:{c}" for i, c in enumerate(names)))

    inst = Counter()        # 类 → 实例数
    img_of = Counter()      # 类 → 出现该类的图片数
    errors = []
    n_ok = 0
    n_background = 0        # 有 json 但一个框都没有的图（纯背景，val 里是好事）

    for jf in jsons:
        try:
            data = json.loads(jf.read_text(encoding="utf-8"))
        except Exception as e:
            errors.append(f"{jf.name}: json 解析失败: {e}")
            continue

        # 找原图：先信 json 里的 imagePath，找不到就在同目录按同名试
        img = jf.parent / (data.get("imagePath") or jf.stem)
        if not img.is_file():
            cand = [p for p in jf.parent.glob(jf.stem + ".*")
                    if p.suffix.lower() in IMG_EXT]
            if not cand:
                errors.append(f"{jf.name}: 找不到原图（imagePath={data.get('imagePath')!r}）")
                continue
            img = cand[0]
        if img.suffix.lower() not in IMG_EXT:
            errors.append(f"{jf.name}: 原图后缀不认识: {img.name}")
            continue

        # 标注是照着 json 里记的尺寸画的，优先用它（没有才回退到 PIL）
        W, H = data.get("imageWidth"), data.get("imageHeight")
        if not W or not H:
            from PIL import Image
            with Image.open(img) as im:
                W, H = im.size

        lines, hit = [], set()
        for sh in data.get("shapes", []):
            label = sh.get("label", "")
            if label not in cls2id:
                errors.append(f"{jf.name}: 未知类名 {label!r}（config.h CLASSES 里没有）")
                continue
            bb = shape_bbox(sh)
            if bb is None:
                continue
            x1 = max(0.0, min(float(bb[0]), W)); x2 = max(0.0, min(float(bb[2]), W))
            y1 = max(0.0, min(float(bb[1]), H)); y2 = max(0.0, min(float(bb[3]), H))
            w, h = x2 - x1, y2 - y1
            if w <= 1 or h <= 1:
                errors.append(f"{jf.name}: 框退化/越界到看不见（{label} {w:.1f}x{h:.1f}px），已丢")
                continue
            lines.append(f"{cls2id[label]} {(x1+x2)/2/W:.6f} {(y1+y2)/2/H:.6f} "
                         f"{w/W:.6f} {h/H:.6f}")
            inst[label] += 1
            hit.add(label)

        for c in hit:
            img_of[c] += 1
        if lines:
            n_ok += 1
        else:
            n_background += 1

        shutil.copy2(img, dst / "images" / img.name)
        (dst / "labels" / (img.stem + ".txt")).write_text(
            "\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    # data.yaml —— 写绝对 path，搬目录的话自己改这一行
    y = ["# 由 tools/labelme2val.py 生成。别手改 names 的顺序：它必须跟 include/config.h 的",
         "# CLASSES 逐字逐序一致，否则历史模型的 class_id 全部错位。",
         f"path: {dst}",
         "train:",                       # 空着，这份只当 val 用，别拿它训
         "val: images",
         "names:"]
    y += [f"  {i}: {c}" for i, c in enumerate(names)]
    (dst / "data.yaml").write_text("\n".join(y) + "\n", encoding="utf-8")

    rep = [f"json 总数: {len(jsons)}   转成功: {n_ok}   纯背景图(无框): {n_background}",
           f"输出: {dst}",
           "",
           f"{'class':<12}{'inst':>6}{'imgs':>6}"]
    rep += [f"{c:<12}{inst[c]:>6}{img_of[c]:>6}" for c in names]
    rep += [f"{'TOTAL':<12}{sum(inst.values()):>6}{n_ok:>6}"]
    if errors:
        rep += ["", f"⚠ {len(errors)} 个问题（每条都可能导致这张图少框/漏图）:"] + errors
    text = "\n".join(rep) + "\n"
    (dst / "report.txt").write_text(text, encoding="utf-8")
    print("\n" + text)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
