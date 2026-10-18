#!/usr/bin/env bash
set -euo pipefail

# 获取脚本所在根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASSETS_DIR="${SCRIPT_DIR}/app/src/main/assets/models"

mkdir -p "${ASSETS_DIR}"

echo "================================================================="
echo "=== [Pixel 10 Edge AI] 自动下载 Google 官方 LiteRT 视觉检测模型 ==="
echo "================================================================="

# 1. 下载 EfficientDet-Lite0 INT8 (Google 官方 MediaPipe / LiteRT INT8 量化模型)
EFFICIENTDET_TARGET="${ASSETS_DIR}/efficientdet_lite0.tflite"
EFFICIENTDET_URL="https://storage.googleapis.com/mediapipe-models/object_detector/efficientdet_lite0/int8/latest/efficientdet_lite0.tflite"

echo ">> [1/2] 正在拉取 EfficientDet-Lite0 INT8..."
curl -L --retry 3 "${EFFICIENTDET_URL}" -o "${EFFICIENTDET_TARGET}"
echo "   已成功保存至: ${EFFICIENTDET_TARGET}"

# 2. 下载 SSD MobileNet V1 INT8 (全整型量化模型，专为 Tensor G5 Edge TPU 加速特化)
MOBILENET_TARGET="${ASSETS_DIR}/mobilenetv1_quant.tflite"
MOBILENET_URL="https://storage.googleapis.com/download.tensorflow.org/models/tflite/task_library/object_detection/android/lite-model_ssd_mobilenet_v1_1_metadata_2.tflite"

echo ">> [2/2] 正在拉取 SSD MobileNet V1 INT8 (TPU 特化)..."
curl -L --retry 3 "${MOBILENET_URL}" -o "${MOBILENET_TARGET}"
echo "   已成功保存至: ${MOBILENET_TARGET}"

echo "================================================================="
echo ">>> 模型就绪！已准备好直通 Pixel 10 Edge TPU 硬件推理！"
echo "================================================================="
ls -lh "${ASSETS_DIR}"
