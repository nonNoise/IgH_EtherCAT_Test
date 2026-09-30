#!/usr/bin/env bash

set -euo pipefail

# ============================================================
# setup_github.sh
#
# Git / GitHub SSH setup script
#
# Features:
#   - Git user.name / user.email setup
#   - ED25519 SSH key generation
#   - GitHub Personal Access Token input
#   - GitHub account verification
#   - Register SSH public key to GitHub
#   - SSH connection test
#
# Notes:
#   - GitHub Token is NOT saved to disk.
#   - Existing SSH keys are NOT overwritten.
# ============================================================

GITHUB_HOST="github.com"
GITHUB_API="https://api.github.com"

SSH_DIR="${HOME}/.ssh"
SSH_KEY="${SSH_DIR}/id_ed25519"
SSH_CONFIG="${SSH_DIR}/config"
KNOWN_HOSTS="${SSH_DIR}/known_hosts"

# ------------------------------------------------------------
# Colors
# ------------------------------------------------------------

if [ -t 1 ]; then
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    RED='\033[0;31m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    GREEN=''
    YELLOW=''
    RED=''
    BLUE=''
    NC=''
fi

info()
{
    echo -e "${BLUE}[INFO]${NC} $*"
}

ok()
{
    echo -e "${GREEN}[ OK ]${NC} $*"
}

warn()
{
    echo -e "${YELLOW}[WARN]${NC} $*"
}

error()
{
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

# ------------------------------------------------------------
# Temporary files
# ------------------------------------------------------------

TEMP_FILES=()

make_temp()
{
    local tmp
    tmp="$(mktemp)"
    TEMP_FILES+=("${tmp}")
    echo "${tmp}"
}

cleanup()
{
    unset GITHUB_TOKEN 2>/dev/null || true
    unset PUBLIC_KEY 2>/dev/null || true
    unset DEVICE_NAME 2>/dev/null || true

    local f
    for f in "${TEMP_FILES[@]:-}"; do
        if [ -n "${f}" ] && [ -f "${f}" ]; then
            rm -f "${f}"
        fi
    done
}

trap cleanup EXIT INT TERM

# ------------------------------------------------------------
# Header
# ------------------------------------------------------------

echo
echo "============================================================"
echo " GitHub SSH Setup"
echo "============================================================"
echo

# ------------------------------------------------------------
# Check required commands
# ------------------------------------------------------------

REQUIRED_COMMANDS=(
    git
    curl
    ssh
    ssh-keygen
    ssh-keyscan
    python3
)

MISSING_COMMANDS=()

for cmd in "${REQUIRED_COMMANDS[@]}"; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        MISSING_COMMANDS+=("${cmd}")
    fi
done

if [ "${#MISSING_COMMANDS[@]}" -ne 0 ]; then
    error "必要なコマンドがインストールされていません。"
    echo

    for cmd in "${MISSING_COMMANDS[@]}"; do
        echo "  - ${cmd}"
    done

    echo
    echo "Debian / Ubuntu / Raspberry Pi OS の場合:"
    echo
    echo "  sudo apt update"
    echo "  sudo apt install -y git curl openssh-client python3"
    echo

    exit 1
fi

ok "Required commands are installed."

# ------------------------------------------------------------
# Device name
# ------------------------------------------------------------

DEFAULT_DEVICE_NAME="$(hostname)"

echo
read -r -p "Device name [${DEFAULT_DEVICE_NAME}]: " DEVICE_NAME

if [ -z "${DEVICE_NAME}" ]; then
    DEVICE_NAME="${DEFAULT_DEVICE_NAME}"
fi

# ------------------------------------------------------------
# Git user.name
# ------------------------------------------------------------

CURRENT_GIT_NAME="$(git config --global user.name 2>/dev/null || true)"

echo

if [ -n "${CURRENT_GIT_NAME}" ]; then

    read -r -p "Git user.name [${CURRENT_GIT_NAME}]: " GIT_NAME

    if [ -z "${GIT_NAME}" ]; then
        GIT_NAME="${CURRENT_GIT_NAME}"
    fi

else

    read -r -p "Git user.name: " GIT_NAME

fi

if [ -z "${GIT_NAME}" ]; then
    error "Git user.name は空にできません。"
    exit 1
fi

# ------------------------------------------------------------
# Git user.email
# ------------------------------------------------------------

CURRENT_GIT_EMAIL="$(git config --global user.email 2>/dev/null || true)"

if [ -n "${CURRENT_GIT_EMAIL}" ]; then

    read -r -p "Git user.email [${CURRENT_GIT_EMAIL}]: " GIT_EMAIL

    if [ -z "${GIT_EMAIL}" ]; then
        GIT_EMAIL="${CURRENT_GIT_EMAIL}"
    fi

else

    read -r -p "Git user.email: " GIT_EMAIL

fi

if [ -z "${GIT_EMAIL}" ]; then
    error "Git user.email は空にできません。"
    exit 1
fi

# ------------------------------------------------------------
# GitHub Personal Access Token
# ------------------------------------------------------------

echo
echo "GitHub Personal Access Token を入力してください。"
echo
echo "推奨:"
echo "  Fine-grained Personal Access Token"
echo
echo "必要な設定:"
echo
echo "  Repository access:"
echo "    Public repositories でOKです"
echo
echo "  Account permissions:"
echo "    Git SSH keys -> Read and write"
echo

read -r -s -p "GitHub Token: " GITHUB_TOKEN
echo

if [ -z "${GITHUB_TOKEN}" ]; then
    error "GitHub Token が入力されていません。"
    exit 1
fi

# ------------------------------------------------------------
# Configure Git
# ------------------------------------------------------------

echo
info "Configuring Git..."

git config --global user.name "${GIT_NAME}"
git config --global user.email "${GIT_EMAIL}"
git config --global init.defaultBranch main

ok "Git user.name  = ${GIT_NAME}"
ok "Git user.email = ${GIT_EMAIL}"
ok "Default branch = main"

# ------------------------------------------------------------
# Check GitHub Token
# ------------------------------------------------------------

echo
info "Checking GitHub token..."

TOKEN_RESPONSE="$(make_temp)"

HTTP_CODE="$(
    curl \
        --silent \
        --show-error \
        --output "${TOKEN_RESPONSE}" \
        --write-out "%{http_code}" \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer ${GITHUB_TOKEN}" \
        "${GITHUB_API}/user"
)"

case "${HTTP_CODE}" in

    200)

        GITHUB_USERNAME="$(
            python3 - "${TOKEN_RESPONSE}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)

print(data.get("login", "unknown"))
PY
        )"

        ok "GitHub authentication successful."
        ok "GitHub user = ${GITHUB_USERNAME}"
        ;;

    401)

        echo
        error "GitHub Token の認証に失敗しました。"
        echo
        echo "HTTP 401: Bad credentials"
        echo
        echo "入力した Personal Access Token が無効です。"
        echo
        echo "GitHub側で以下を確認してください。"
        echo
        echo "------------------------------------------------------------"
        echo " GitHub Token の作成方法"
        echo "------------------------------------------------------------"
        echo
        echo "GitHub:"
        echo
        echo "  Settings"
        echo "    -> Developer settings"
        echo "    -> Personal access tokens"
        echo "    -> Fine-grained tokens"
        echo "    -> Generate new token"
        echo
        echo "推奨設定:"
        echo
        echo "  Repository access:"
        echo
        echo "    Public repositories"
        echo
        echo "    ※ SSH鍵登録だけならこれでOKです。"
        echo
        echo "  Account permissions:"
        echo
        echo "    Git SSH keys"
        echo "        -> Read and write"
        echo
        echo "------------------------------------------------------------"
        echo
        echo "また、以下も確認してください:"
        echo
        echo "  - Tokenを途中までしかコピーしていない"
        echo "  - Tokenの有効期限が切れている"
        echo "  - TokenをRevoke / Deleteしている"
        echo "  - 別のGitHubアカウントのTokenを使用している"
        echo "  - Tokenの先頭や末尾に余計な文字が入っている"
        echo
        echo "Fine-grained Token は通常:"
        echo
        echo "  github_pat_xxxxxxxxxxxxxxxxx"
        echo
        echo "のような形式です。"
        echo

        exit 1
        ;;

    403)

        echo
        error "GitHub Token が拒否されました。"
        echo
        echo "HTTP 403: Forbidden"
        echo
        echo "Tokenの設定またはGitHub側のポリシーを確認してください。"
        echo
        echo "Fine-grained Personal Access Token の推奨設定:"
        echo
        echo "  Repository access:"
        echo "    Public repositories"
        echo
        echo "  Account permissions:"
        echo "    Git SSH keys -> Read and write"
        echo

        exit 1
        ;;

    *)

        echo
        error "GitHub APIとの通信に失敗しました。"
        echo
        echo "HTTP Status: ${HTTP_CODE}"
        echo

        if [ -s "${TOKEN_RESPONSE}" ]; then
            echo "GitHub Response:"
            echo
            cat "${TOKEN_RESPONSE}"
            echo
        fi

        exit 1
        ;;

esac

# ------------------------------------------------------------
# Create ~/.ssh
# ------------------------------------------------------------

echo
info "Preparing SSH directory..."

mkdir -p "${SSH_DIR}"
chmod 700 "${SSH_DIR}"

ok "SSH directory = ${SSH_DIR}"

# ------------------------------------------------------------
# SSH Key
# ------------------------------------------------------------

echo

if [ -f "${SSH_KEY}" ] && [ -f "${SSH_KEY}.pub" ]; then

    ok "Existing SSH key found."
    echo "     ${SSH_KEY}"

elif [ -f "${SSH_KEY}" ] || [ -f "${SSH_KEY}.pub" ]; then

    echo
    error "SSH秘密鍵と公開鍵の片方だけが存在しています。"
    echo
    echo "確認してください:"
    echo
    echo "  ${SSH_KEY}"
    echo "  ${SSH_KEY}.pub"
    echo
    echo "安全のため自動的な上書きは行いません。"
    echo

    exit 1

else

    info "Generating ED25519 SSH key..."

    ssh-keygen \
        -t ed25519 \
        -f "${SSH_KEY}" \
        -C "${DEVICE_NAME}@${GITHUB_USERNAME}" \
        -N ""

    ok "SSH key generated."

fi

chmod 600 "${SSH_KEY}"
chmod 644 "${SSH_KEY}.pub"

# ------------------------------------------------------------
# Register github.com known_hosts
# ------------------------------------------------------------

echo
info "Checking GitHub host key..."

touch "${KNOWN_HOSTS}"
chmod 600 "${KNOWN_HOSTS}"

if ssh-keygen -F "${GITHUB_HOST}" -f "${KNOWN_HOSTS}" >/dev/null 2>&1; then

    ok "github.com already exists in known_hosts."

else

    ssh-keyscan -H "${GITHUB_HOST}" >> "${KNOWN_HOSTS}" 2>/dev/null

    ok "github.com added to known_hosts."

fi

# ------------------------------------------------------------
# Read public key
# ------------------------------------------------------------

PUBLIC_KEY="$(cat "${SSH_KEY}.pub")"

# ------------------------------------------------------------
# Create API JSON
# ------------------------------------------------------------

export DEVICE_NAME
export PUBLIC_KEY

JSON_DATA="$(
    python3 <<'PY'
import json
import os

print(json.dumps({
    "title": os.environ["DEVICE_NAME"],
    "key": os.environ["PUBLIC_KEY"]
}))
PY
)"

unset PUBLIC_KEY

# ------------------------------------------------------------
# Register SSH Key
# ------------------------------------------------------------

echo
info "Registering SSH key to GitHub..."

API_RESPONSE="$(make_temp)"

HTTP_CODE="$(
    curl \
        --silent \
        --show-error \
        --output "${API_RESPONSE}" \
        --write-out "%{http_code}" \
        -X POST \
        -H "Accept: application/vnd.github+json" \
        -H "Authorization: Bearer ${GITHUB_TOKEN}" \
        "${GITHUB_API}/user/keys" \
        -d "${JSON_DATA}"
)"

case "${HTTP_CODE}" in

    201)

        ok "SSH key registered successfully."
        ;;

    401)

        echo
        error "GitHub Token の認証に失敗しました。"
        echo
        echo "HTTP 401: Bad credentials"
        echo
        echo "Tokenが無効、期限切れ、またはRevokeされています。"
        echo

        exit 1
        ;;

    403)

        echo
        error "SSH鍵をGitHubへ登録できませんでした。"
        echo
        echo "HTTP 403: Forbidden"
        echo
        echo "GitHub Token の権限が不足している可能性があります。"
        echo
        echo "GitHub側で以下を設定してください:"
        echo
        echo "  Settings"
        echo "    -> Developer settings"
        echo "    -> Personal access tokens"
        echo "    -> Fine-grained tokens"
        echo
        echo "Account permissions:"
        echo
        echo "  Git SSH keys"
        echo "      -> Read and write"
        echo
        echo "Repository access は:"
        echo
        echo "  Public repositories"
        echo
        echo "のままで問題ありません。"
        echo

        exit 1
        ;;

    422)

        echo
        warn "GitHub returned HTTP 422."
        echo

        MESSAGE="$(
            python3 - "${API_RESPONSE}" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as f:
        data = json.load(f)

    print(data.get("message", ""))

except Exception:
    pass
PY
        )"

        if [ -n "${MESSAGE}" ]; then
            echo "GitHub:"
            echo "  ${MESSAGE}"
            echo
        fi

        echo "このSSH公開鍵が既にGitHubへ登録されている可能性があります。"
        echo
        echo "既存鍵を使用してSSH接続テストを続行します。"
        ;;

    *)

        echo
        error "SSH鍵の登録に失敗しました。"
        echo
        echo "HTTP Status: ${HTTP_CODE}"
        echo

        if [ -s "${API_RESPONSE}" ]; then
            echo "GitHub Response:"
            echo
            cat "${API_RESPONSE}"
            echo
        fi

        exit 1
        ;;

esac

# ------------------------------------------------------------
# SSH config
# ------------------------------------------------------------

echo
info "Checking SSH config..."

touch "${SSH_CONFIG}"
chmod 600 "${SSH_CONFIG}"

if grep -qE '^[[:space:]]*Host[[:space:]]+github\.com([[:space:]]|$)' \
    "${SSH_CONFIG}" 2>/dev/null; then

    ok "github.com already exists in ~/.ssh/config."

else

    cat >> "${SSH_CONFIG}" <<EOF

Host github.com
    HostName github.com
    User git
    IdentityFile ${SSH_KEY}
    IdentitiesOnly yes
EOF

    ok "github.com added to ~/.ssh/config."

fi

# ------------------------------------------------------------
# SSH Test
# ------------------------------------------------------------

echo
info "Testing GitHub SSH connection..."

set +e

SSH_OUTPUT="$(
    ssh \
        -o BatchMode=yes \
        -o ConnectTimeout=10 \
        -T \
        git@github.com \
        2>&1
)"

SSH_EXIT=$?

set -e

echo
echo "${SSH_OUTPUT}"
echo

# GitHub normally returns exit status 1 even when authentication succeeds.

if echo "${SSH_OUTPUT}" | grep -qi "successfully authenticated"; then

    ok "GitHub SSH authentication successful."

else

    error "GitHub SSH authentication failed."

    echo
    echo "SSH exit code: ${SSH_EXIT}"
    echo
    echo "公開鍵:"
    echo
    cat "${SSH_KEY}.pub"
    echo
    echo "GitHub側のSSH Keysを確認してください:"
    echo
    echo "  Settings"
    echo "    -> SSH and GPG keys"
    echo

    exit 1

fi

# ------------------------------------------------------------
# Finished
# ------------------------------------------------------------

echo
echo "============================================================"
echo " GitHub setup completed"
echo "============================================================"
echo

echo "Device        : ${DEVICE_NAME}"
echo "GitHub user   : ${GITHUB_USERNAME}"
echo "Git user      : ${GIT_NAME}"
echo "Git email     : ${GIT_EMAIL}"
echo "SSH key       : ${SSH_KEY}"

echo
echo "SSH形式のGit URLを使用できます:"
echo
echo "  git clone git@github.com:${GITHUB_USERNAME}/REPOSITORY.git"
echo

echo "既存リポジトリがHTTPSになっている場合:"
echo
echo "  git remote -v"
echo
echo "  git remote set-url origin git@github.com:${GITHUB_USERNAME}/REPOSITORY.git"
echo

echo "接続確認:"
echo
echo "  ssh -T git@github.com"
echo

echo "GitHub Token は保存されていません。"
echo
