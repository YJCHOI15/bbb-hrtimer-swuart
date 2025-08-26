#!/bin/bash

# BBB 접속 정보 (필요에 맞게 수정)
BBB_USER=debian
BBB_HOST=192.168.7.2
DIR_NAME=$(basename "$PWD")
BBB_DIR=/home/${BBB_USER}/${DIR_NAME}

# 파일 경로
MAIN_FILE=./main
KO_FILE=./drivers/*.ko

# 파일 복사
if scp ${MAIN_FILE} ${KO_FILE} ${BBB_USER}@${BBB_HOST}:${BBB_DIR}/; then
    echo "복사 완료: ${MAIN_FILE}, ${KO_FILE} → ${BBB_USER}@${BBB_HOST}:${BBB_DIR}"
else
    echo "복사 실패"
fi