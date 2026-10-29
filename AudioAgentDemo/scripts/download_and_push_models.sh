#!/usr/bin/env bash
# ==============================================================================
# download_and_push_models.sh
#
# 自动从国内高速镜像源（ModelScope 魔搭 / hf-mirror）下载端侧大模型与语音模型，
# 并通过 ADB 自动推送到 Android 设备的目标存储路径。
#
# 目标路径:
#   LLM: /sdcard/Android/data/com.ai.audioagent/files/models/llm/
#   ASR: /sdcard/Android/data/com.ai.audioagent/files/models/asr/
# ==============================================================================

set -eo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODELS_DIR="${ROOT_DIR}/models"
LLM_LOCAL_DIR="${MODELS_DIR}/llm"
ASR_LOCAL_DIR="${MODELS_DIR}/asr"

PACKAGE_NAME="com.ai.audioagent"
REMOTE_BASE="/sdcard/Android/data/${PACKAGE_NAME}/files/models"
REMOTE_LLM_DIR="${REMOTE_BASE}/llm"
REMOTE_ASR_DIR="${REMOTE_BASE}/asr"

DO_LLM=true
DO_ASR=true
DOWNLOAD_ONLY=false
PUSH_ONLY=false

usage() {
    echo -e "${BOLD}使用指南:${NC}"
    echo -e "  bash scripts/download_and_push_models.sh [选项]"
    echo -e ""
    echo -e "${BOLD}选项:${NC}"
    echo -e "  --all            同时下载并推送 LLM 大模型与 ASR 语音模型 (默认)"
    echo -e "  --llm-only       仅处理 Qwen2.5-0.5B-Instruct-MNN 端侧大模型 (约 400MB)"
    echo -e "  --asr-only       仅处理 Sherpa-onnx Zipformer 端侧 ASR 模型 (约 31MB)"
    echo -e "  --download-only  仅下载模型到本地 models/ 目录，不执行 adb push"
    echo -e "  --push-only      跳过下载，直接将本地 models/ 目录推送到已连接的 Android 设备"
    echo -e "  -h, --help       显示此帮助信息"
    echo -e ""
    echo -e "${BOLD}示例:${NC}"
    echo -e "  bash scripts/download_and_push_models.sh"
    echo -e "  bash scripts/download_and_push_models.sh --llm-only"
    echo -e "  bash scripts/download_and_push_models.sh --push-only"
    exit 0
}

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)
            DO_LLM=true
            DO_ASR=true
            shift
            ;;
        --llm-only)
            DO_LLM=true
            DO_ASR=false
            shift
            ;;
        --asr-only)
            DO_LLM=false
            DO_ASR=true
            shift
            ;;
        --download-only)
            DOWNLOAD_ONLY=true
            shift
            ;;
        --push-only)
            PUSH_ONLY=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo -e "${RED}未知参数: $1${NC}"
            usage
            ;;
    esac
done

# 模型清单与镜像源定义
# 格式: 文件名 | 期望最小字节数 | 候选 URL 列表 (空格分隔)
declare -a LLM_FILES=(
    "config.json|50|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/config.json https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/config.json https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/config.json"
    "llm_config.json|100|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/llm_config.json https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm_config.json https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm_config.json"
    "tokenizer.txt|1000000|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/tokenizer.txt https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/tokenizer.txt https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/tokenizer.txt"
    "embeddings_bf16.bin|100000000|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/embeddings_bf16.bin https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/embeddings_bf16.bin https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/embeddings_bf16.bin"
    "llm.mnn|400000|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/llm.mnn https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn"
    "llm.mnn.weight|200000000|https://www.modelscope.cn/models/MNN/Qwen2.5-0.5B-Instruct-MNN/resolve/master/llm.mnn.weight https://hf-mirror.com/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn.weight https://huggingface.co/taobao-mnn/Qwen2.5-0.5B-Instruct-MNN/resolve/main/llm.mnn.weight"
)

declare -a ASR_FILES=(
    "encoder-epoch-99-avg-1.int8.onnx|15000000|https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/encoder-epoch-99-avg-1.int8.onnx https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/encoder-epoch-99-avg-1.int8.onnx"
    "decoder-epoch-99-avg-1.onnx|5000000|https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/decoder-epoch-99-avg-1.onnx https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/decoder-epoch-99-avg-1.onnx"
    "joiner-epoch-99-avg-1.int8.onnx|1000000|https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/joiner-epoch-99-avg-1.int8.onnx https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/joiner-epoch-99-avg-1.int8.onnx"
    "tokens.txt|30000|https://hf-mirror.com/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/tokens.txt https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/main/tokens.txt"
)

# 文件大小获取 (兼容 Linux 与 macOS stat)
get_file_size() {
    local f="$1"
    if [[ ! -f "$f" ]]; then
        echo 0
        return
    fi
    if stat -f%z "$f" >/dev/null 2>&1; then
        stat -f%z "$f"
    else
        stat -c%s "$f"
    fi
}

download_file_with_fallback() {
    local target_dir="$1"
    local filename="$2"
    local min_bytes="$3"
    shift 3
    local urls=("$@")

    local dest="${target_dir}/${filename}"
    local current_size
    current_size=$(get_file_size "$dest")

    if [[ "$current_size" -ge "$min_bytes" ]]; then
        echo -e "  [✔ 已存在] ${CYAN}${filename}${NC} (${current_size} 字节，跳过下载)"
        return 0
    fi

    echo -e "  [⬇ 开始下载] ${BOLD}${filename}${NC}..."
    local temp_dest="${dest}.tmp"

    for url in "${urls[@]}"; do
        echo -e "    ${YELLOW}➔ 尝试下载源:${NC} ${url}"
        if curl -L --fail --retry 2 --retry-delay 1 --connect-timeout 15 -C - \
             -o "$temp_dest" "$url"; then
            local downloaded_size
            downloaded_size=$(get_file_size "$temp_dest")
            if [[ "$downloaded_size" -ge "$min_bytes" ]]; then
                mv "$temp_dest" "$dest"
                echo -e "    ${GREEN}✔ 下载成功:${NC} ${filename} (${downloaded_size} 字节)"
                return 0
            else
                echo -e "    ${RED}✘ 文件大小校验不达标 (${downloaded_size} < ${min_bytes})${NC}"
                rm -f "$temp_dest"
            fi
        else
            echo -e "    ${YELLOW}✘ 当前源连接失败，自动切换下一个镜像源...${NC}"
            rm -f "$temp_dest"
        fi
    done

    echo -e "${RED}错误: 所有镜像源均下载失败: ${filename}${NC}"
    return 1
}

echo -e "================================================================="
echo -e "     ${BOLD}${CYAN}端侧全双工 AI Audio Agent 模型自动下载与 ADB 部署${NC}"
echo -e "================================================================="

# 1. 下载阶段
if [[ "$PUSH_ONLY" == false ]]; then
    mkdir -p "$LLM_LOCAL_DIR"
    mkdir -p "$ASR_LOCAL_DIR"

    if [[ "$DO_LLM" == true ]]; then
        echo -e "\n${BOLD}${BLUE}【步骤 1/2】下载 Qwen2.5-0.5B-Instruct-MNN 大模型文件 (优先 ModelScope 极速源)...${NC}"
        for item in "${LLM_FILES[@]}"; do
            IFS='|' read -r fname min_b urls_str <<< "$item"
            read -r -a url_arr <<< "$urls_str"
            download_file_with_fallback "$LLM_LOCAL_DIR" "$fname" "$min_b" "${url_arr[@]}"
        done
        echo -e "${GREEN}✔ Qwen2.5-0.5B-Instruct-MNN 大模型权重全部就绪！${NC}"
    fi

    if [[ "$DO_ASR" == true ]]; then
        echo -e "\n${BOLD}${BLUE}【步骤 2/2】下载 Sherpa-onnx Zipformer ASR 语音模型文件...${NC}"
        for item in "${ASR_FILES[@]}"; do
            IFS='|' read -r fname min_b urls_str <<< "$item"
            read -r -a url_arr <<< "$urls_str"
            download_file_with_fallback "$ASR_LOCAL_DIR" "$fname" "$min_b" "${url_arr[@]}"
        done
        echo -e "${GREEN}✔ Sherpa-onnx Zipformer ASR 模型全部就绪！${NC}"
    fi
else
    echo -e "\n${YELLOW}跳过下载阶段 (--push-only 模式)${NC}"
fi

if [[ "$DOWNLOAD_ONLY" == true ]]; then
    echo -e "\n${GREEN}✔ 下载完成 (--download-only 模式，已跳过 ADB 推送)。${NC}"
    echo -e "本地模型存储于: ${BOLD}${MODELS_DIR}${NC}"
    exit 0
fi

# 2. ADB 推送阶段
echo -e "\n================================================================="
echo -e "                 ${BOLD}${BLUE}开始 ADB 推送到 Android 设备${NC}"
echo -e "================================================================="

if ! command -v adb >/dev/null 2>&1; then
    echo -e "${RED}错误: 未在 PATH 中找到 'adb' 命令。${NC}"
    echo -e "请安装 Android SDK platform-tools 或将 adb 添加到环境变量后重试。"
    echo -e "模型已保存在本地: ${BOLD}${MODELS_DIR}${NC}"
    exit 1
fi

DEVICES=$(adb devices | grep -v "List of devices" | grep "device$" || true)
if [[ -z "$DEVICES" ]]; then
    echo -e "${RED}错误: 未检测到已连接的 Android 设备 (adb devices 为空)。${NC}"
    echo -e "请通过 USB 或 Wi-Fi 调试连接手机，并确保开启了「USB 调试」权限。"
    echo -e "本地模型已就绪，可在连接设备后执行: ${CYAN}bash scripts/download_and_push_models.sh --push-only${NC}"
    exit 1
fi

DEVICE_MODEL=$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r\n')
echo -e "检测到目标设备: ${GREEN}${DEVICE_MODEL}${NC}"

# 创建远程目录
echo -e "正在设备创建目录: ${REMOTE_BASE}..."
adb shell "mkdir -p ${REMOTE_LLM_DIR} ${REMOTE_ASR_DIR}"

if [[ "$DO_LLM" == true ]]; then
    echo -e "\n${BOLD}正在推送 Qwen2.5-0.5B MNN 大模型文件 (约 400MB)...${NC}"
    for item in "${LLM_FILES[@]}"; do
        IFS='|' read -r fname _ _ <<< "$item"
        local_path="${LLM_LOCAL_DIR}/${fname}"
        if [[ -f "$local_path" ]]; then
            echo -e "  ➔ 推送 ${fname}..."
            adb push "$local_path" "${REMOTE_LLM_DIR}/${fname}"
        else
            echo -e "  ${YELLOW}⚠ 本地文件缺失: ${local_path}，跳过${NC}"
        fi
    done
fi

if [[ "$DO_ASR" == true ]]; then
    echo -e "\n${BOLD}正在推送 Sherpa-onnx Zipformer ASR 语音模型文件 (约 31MB)...${NC}"
    for item in "${ASR_FILES[@]}"; do
        IFS='|' read -r fname _ _ <<< "$item"
        local_path="${ASR_LOCAL_DIR}/${fname}"
        if [[ -f "$local_path" ]]; then
            echo -e "  ➔ 推送 ${fname}..."
            adb push "$local_path" "${REMOTE_ASR_DIR}/${fname}"
        else
            echo -e "  ${YELLOW}⚠ 本地文件缺失: ${local_path}，跳过${NC}"
        fi
    done
fi

echo -e "\n================================================================="
echo -e "                   ${BOLD}${GREEN}设备端模型部署校验${NC}"
echo -e "================================================================="
echo -e "${CYAN}【LLM 目录】${REMOTE_LLM_DIR}${NC}:"
adb shell "ls -lh ${REMOTE_LLM_DIR}" 2>/dev/null || true
echo -e "\n${CYAN}【ASR 目录】${REMOTE_ASR_DIR}${NC}:"
adb shell "ls -lh ${REMOTE_ASR_DIR}" 2>/dev/null || true

echo -e "\n${GREEN}${BOLD}🎉 模型部署完成！${NC}"
echo -e "请在手机打开 AudioAgentDemo，即可直接加载真实的端侧大模型！"
