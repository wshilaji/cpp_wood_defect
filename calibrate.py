import numpy as np
import cv2 as cv
import glob
import os

# ==========================================
# 1. 设置你的棋盘格参数（根据你的A4纸实际情况修改）
CHECKERBOARD = (10, 7)  # 内部角点的数量（列, 行），例如 7列5行
SQUARE_SIZE = 0.02    # 单个格子的实际边长（单位：米，例如 25mm 就是 0.025）

# 准备 3D 真实世界坐标点 (Object Points)
objp = np.zeros((CHECKERBOARD[0] * CHECKERBOARD[1], 3), np.float32)
objp[:, :2] = np.mgrid[0:CHECKERBOARD[0], 0:CHECKERBOARD[1]].T.reshape(-1, 2) * SQUARE_SIZE

objpoints = []  # 存储 3D 真实世界坐标点
imgpoints = []  # 存储 2D 图像像素坐标点

# ==========================================
# 2. 批量读取图片并提取角点
# ==========================================
images = glob.glob('calib_images/*.jpg')  # 确保图片放在 calib_images 文件夹下
if not images:
    print("未找到图片，请检查路径！")
else:
    for fname in images:
        img = cv.imread(fname)
        if img is None:
            print(f"[失败] 无法读取图片: {fname}")
            continue
            
        gray = cv.cvtColor(img, cv.COLOR_BGR2GRAY)
        
        # 寻找棋盘格角点
        ret, corners = cv.findChessboardCorners(gray, CHECKERBOARD, None)
        
        if ret:
            # 亚像素级角点优化（提高精度）
            criteria = (cv.TERM_CRITERIA_EPS + cv.TERM_CRITERIA_MAX_ITER, 30, 0.001)
            corners2 = cv.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
            
            objpoints.append(objp)
            imgpoints.append(corners2)
            
            # 【修改点】：将画好角点的图片直接保存到本地，不弹窗
            cv.drawChessboardCorners(img, CHECKERBOARD, corners2, ret)
            save_path = fname.replace('.jpg', '_corners.jpg')
            cv.imwrite(save_path, img)
            print(f"[成功] 检测到角点并保存: {save_path}")
        else:
            print(f"[失败] 未检测到角点: {fname}")

# ==========================================
# 3. 核心标定过程
# ==========================================
if len(objpoints) > 0:
    ret, mtx, dist, rvecs, tvecs = cv.calibrateCamera(objpoints, imgpoints, gray.shape[::-1], None, None)
    
    print("\n--- 标定成功 ---")
    print(f"重投影误差 (越小越好，通常 < 0.5): {ret}")
    print("相机内参矩阵 (包含 fx, fy):\n", mtx)
    print("畸变系数:\n", dist)
    
    # 保存参数，方便后续算长宽
    np.savez('camera_params.npz', mtx=mtx, dist=dist, rvecs=rvecs, tvecs=tvecs)
    print("参数已保存至 camera_params.npz")
else:
    print("没有有效的标定图片，请检查角点检测情况！")
