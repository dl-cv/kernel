#!/bin/bash
# DLCVCAM RK3576 内核自动编译脚本
# - 自动判断初次配置 / 增量编译
# - 若 DLCVCAM_BUILD_VERSION 对应提交与当前 HEAD 不同，则按当天日期自动 bump
# - 产出带构建编号与短提交的 boot 镜像：boot-rk3576-6.1.99-YYYYMMDDNN-<gitsha>.img
# - 同时生成整包完整性 sidecar（不使用签名，仅 .sha256）：
#     <img>.sha256          整包 SHA-256（防传错包/截断；FIT 内嵌 hash 另由 verify 检查）
#   板端烧录前：python3 scripts/dlcvcam_verify_bootimg.py verify <img> --require-sidecar
#
# 用法:
#   ./build_bootimg.sh              # 自动判断配置并编译
#   ./build_bootimg.sh --force-config   # 强制重新 defconfig + merge
#   ./build_bootimg.sh --no-bump        # 不自动更新 DLCVCAM_BUILD_VERSION
#   ./build_bootimg.sh -j4              # 指定并行度
#   ./build_bootimg.sh --clean-config   # 删除 .config 后走初次配置

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
DEFCONFIG=lubancat_linux_rk3576_defconfig
CUT_CONFIG=arch/arm64/configs/dlcvcam_rk3576_kernel_cut.config
DTB_NAME=dlcvcam-rk3576.dtb
BUILD_VERSION_FILE=DLCVCAM_BUILD_VERSION
KERNEL_VER_PREFIX="6.1.99"
JOBS="$(nproc 2>/dev/null || echo 8)"
FORCE_CONFIG=0
NO_BUMP=0
CLEAN_CONFIG=0

log()  { printf '\033[1;34m[%s]\033[0m %s\n' "$(date '+%H:%M:%S')" "$*"; }
ok()   { printf '\033[1;32m[%s]\033[0m %s\n' "$(date '+%H:%M:%S')" "$*"; }
warn() { printf '\033[1;33m[%s]\033[0m %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
die()  { printf '\033[1;31m[%s]\033[0m %s\n' "$(date '+%H:%M:%S')" "$*" >&2; exit 1; }

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \?//'
	exit 0
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h|--help) usage ;;
		--force-config) FORCE_CONFIG=1; shift ;;
		--clean-config) CLEAN_CONFIG=1; shift ;;
		--no-bump) NO_BUMP=1; shift ;;
		-j*)
			if [[ "$1" == "-j" ]]; then
				JOBS="${2:?-j 需要并行数}"
				shift 2
			else
				JOBS="${1#-j}"
				shift
			fi
			;;
		*)
			die "未知参数: $1 （见 --help）"
			;;
	esac
done

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

need_cmd make
need_cmd lz4
need_cmd git
need_cmd "${CROSS_COMPILE}gcc"

# ---------- 构建编号：相对上次提交该文件的 commit 是否变化 ----------
# 规则：
# 1. 若 DLCVCAM_BUILD_VERSION 相对 HEAD 有未提交修改，且内容已是合法编号，则沿用工作区值
# 2. 否则看「最后一次提交该文件的 commit」是否等于当前 HEAD：
#    - 相等：说明本提交就是 bump 提交，沿用文件内容
#    - 不等：说明 HEAD 相对上次 bump 又有新提交，按当天日期自动 bump（同日递增 NN）
bump_build_version_if_needed() {
	local today current last_bump_commit head_commit date_part seq new_ver

	[[ -f "${BUILD_VERSION_FILE}" ]] || die "缺少 ${BUILD_VERSION_FILE}"

	current="$(tr -d '[:space:]' < "${BUILD_VERSION_FILE}")"
	[[ "${current}" =~ ^[0-9]{8}(0[1-9]|[1-9][0-9])$ ]] \
		|| die "${BUILD_VERSION_FILE} 格式非法: '${current}'（需要 YYYYMMDDNN，NN=01..99）"

	if [[ "${NO_BUMP}" -eq 1 ]]; then
		warn "跳过版本自动 bump（--no-bump），使用: ${current}"
		echo "${current}"
		return
	fi

	# 工作区已改过该文件：尊重用户/上次脚本写好的值
	if ! git diff --quiet -- "${BUILD_VERSION_FILE}" 2>/dev/null \
		|| ! git diff --cached --quiet -- "${BUILD_VERSION_FILE}" 2>/dev/null; then
		log "${BUILD_VERSION_FILE} 工作区已修改，沿用: ${current}"
		echo "${current}"
		return
	fi

	head_commit="$(git rev-parse HEAD)"
	last_bump_commit="$(git log -1 --format='%H' -- "${BUILD_VERSION_FILE}" 2>/dev/null || true)"

	if [[ -n "${last_bump_commit}" && "${last_bump_commit}" == "${head_commit}" ]]; then
		log "${BUILD_VERSION_FILE} 已在当前 HEAD 提交，沿用: ${current}"
		echo "${current}"
		return
	fi

	today="$(date +%Y%m%d)"
	date_part="${current:0:8}"
	seq="${current:8:2}"
	# 去掉前导零做算术
	seq=$((10#${seq}))

	if [[ "${date_part}" == "${today}" ]]; then
		seq=$((seq + 1))
		if [[ "${seq}" -gt 99 ]]; then
			die "当天构建序号已超过 99，请手工处理 ${BUILD_VERSION_FILE}"
		fi
		new_ver="$(printf '%s%02d' "${today}" "${seq}")"
	else
		new_ver="${today}01"
	fi

	printf '%s\n' "${new_ver}" > "${BUILD_VERSION_FILE}"
	ok "HEAD 相对上次 ${BUILD_VERSION_FILE} 提交已变化，自动 bump: ${current} -> ${new_ver}"
	echo "${new_ver}"
}

# ---------- 初次配置 vs 增量 ----------
need_initial_config() {
	[[ "${FORCE_CONFIG}" -eq 1 ]] && return 0
	[[ "${CLEAN_CONFIG}" -eq 1 ]] && return 0
	[[ ! -f .config ]] && return 0

	# 关键配置标记：本地版本与 ARM64，避免误用其它板级 .config
	if ! grep -q '^CONFIG_ARM64=y' .config 2>/dev/null; then
		warn ".config 不是 ARM64 配置，将重新配置"
		return 0
	fi
	if ! grep -q 'CONFIG_LOCALVERSION="-rk3576"' .config 2>/dev/null; then
		# 裁剪配置合并后通常带 -rk3576；没有也不强制失败，仅提示
		warn ".config 未包含 CONFIG_LOCALVERSION=\"-rk3576\"，仍按增量处理（可用 --force-config）"
	fi
	return 1
}

do_initial_config() {
	log "初次配置：${DEFCONFIG} + ${CUT_CONFIG}"
	if [[ "${CLEAN_CONFIG}" -eq 1 && -f .config ]]; then
		rm -f .config .config.old
	fi

	make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${DEFCONFIG}"
	[[ -f "${CUT_CONFIG}" ]] || die "缺少裁剪配置: ${CUT_CONFIG}"
	./scripts/kconfig/merge_config.sh -m .config "${CUT_CONFIG}"
	make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" olddefconfig
	ok "配置完成"
}

do_build() {
	log "编译 Image / dtbs / modules （-j${JOBS}）..."
	make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
		Image dtbs modules -j"${JOBS}"

	log "压缩 Image -> Image.lz4 ..."
	lz4 -f arch/arm64/boot/Image arch/arm64/boot/Image.lz4

	log "打包 boot.img （DTB=${DTB_NAME}）..."
	BOOT_ITS=boot.its ./scripts/mkimg --dtb "${DTB_NAME}"

	[[ -f boot.img ]] || die "未生成 boot.img"
}

is_worktree_dirty_except_build_version() {
	# 忽略 DLCVCAM_BUILD_VERSION 本身（脚本可能刚 bump 过）
	# 以及常见编译产物，避免误标 dirty
	local line path
	while IFS= read -r line; do
		[[ -z "${line}" ]] && continue
		path="${line:3}"
		path="${path#\"}"
		path="${path%\"}"
		# rename: "old -> new"
		if [[ "${path}" == *" -> "* ]]; then
			path="${path##* -> }"
		fi
		case "${path}" in
			"${BUILD_VERSION_FILE}") continue ;;
			boot.img|resource.img|zboot.img|out|out/*) continue ;;
			boot.img.sha256|boot.img.dlcvcam.json) continue ;;
			boot-rk3576-*.img|boot-rk3576-*.img.sha256|boot-rk3576-*.img.dlcvcam.json) continue ;;
			# legacy *.dlcvcam.json ignored if leftover from older builds
			arch/arm64/boot/Image|arch/arm64/boot/Image.lz4) continue ;;
			.config|.config.old) continue ;;
			bad_packages|bad_packages/*) continue ;;
		esac
		return 0
	done < <(git status --porcelain --untracked-files=no 2>/dev/null || true)
	return 1
}

write_bootimg_sha256_sidecar() {
	# 只生成整包 .sha256；板端烧录前用 scripts/dlcvcam_verify_bootimg.py 做
	# 整包 SHA + FIT 内嵌 hash（无签名）
	local img="$1"
	local verify_py="${SCRIPT_DIR}/scripts/dlcvcam_verify_bootimg.py"

	[[ -f "${img}" ]] || die "write_bootimg_sha256_sidecar: 缺少 ${img}"

	# 清理旧版 JSON 清单（若存在），避免误当交付物
	rm -f "${img}.dlcvcam.json"

	if [[ -f "${verify_py}" ]] && command -v python3 >/dev/null 2>&1; then
		python3 "${verify_py}" gen-sidecar "${img}" \
			|| die "生成 .sha256 失败: ${img}"
		# 自检：刚生成的包必须能通过 FIT 内嵌 hash + 整包 sha256
		python3 "${verify_py}" verify "${img}" --require-sidecar \
			|| die "打包后自检失败（不应发生）: ${img}"
		return
	fi

	# 兜底：无 python 时只写整包 sha256（无法做 FIT 自检）
	warn "python3 或 scripts/dlcvcam_verify_bootimg.py 不可用，仅写入 ${img}.sha256（跳过 FIT 自检）"
	need_cmd sha256sum
	sha256sum "${img}" | awk -v n="$(basename "${img}")" '{print $1 "  " n}' > "${img}.sha256"
}

package_named_image() {
	local build_ver short_sha out_name kernelrelease build_id git_desc

	build_ver="$(tr -d '[:space:]' < "${BUILD_VERSION_FILE}")"
	short_sha="$(git rev-parse --short=8 HEAD)"
	# 除版本文件/编译产物外还有改动时加 -dirty，避免与干净提交产物混淆
	if is_worktree_dirty_except_build_version; then
		short_sha="${short_sha}-dirty"
	fi

	out_name="boot-rk3576-${KERNEL_VER_PREFIX}-${build_ver}-${short_sha}.img"
	cp -f boot.img "${out_name}"

	if command -v mkimage >/dev/null 2>&1; then
		log "FIT 信息："
		mkimage -l boot.img || true
	fi

	kernelrelease="$(make -s ARCH="${ARCH}" kernelrelease 2>/dev/null || true)"
	build_id="$(make -s ARCH="${ARCH}" dlcvcam-build-version 2>/dev/null || echo "${build_ver}")"
	git_desc="$(git rev-parse --short HEAD) ($(git rev-parse --abbrev-ref HEAD))"

	log "生成整包 .sha256 并自检（FIT 内嵌 hash + 整包 sha256）..."
	write_bootimg_sha256_sidecar "boot.img"
	write_bootimg_sha256_sidecar "${out_name}"

	ok "完成"
	echo
	echo "  kernelrelease : ${kernelrelease:-n/a}"
	echo "  build version : ${build_id}"
	echo "  git           : ${git_desc}"
	echo "  boot.img      : $(ls -lh boot.img | awk '{print $5}')"
	echo "  交付镜像      : ${out_name}  ($(ls -lh "${out_name}" | awk '{print $5}'))"
	echo "  整包校验      : ${out_name}.sha256"
	echo
	echo "烧录前在板卡上校验（不验签，仅 hash）："
	echo "  python3 scripts/dlcvcam_verify_bootimg.py verify ${out_name} --require-sidecar"
	echo "  # 或只验 FIT 内嵌 sha256（无 .sha256 时）："
	echo "  python3 scripts/dlcvcam_verify_bootimg.py verify ${out_name}"
	echo
	echo "下一步：校验通过后再用 RKDevTool / rkdeveloptool 烧录 ${out_name}"
	echo "  （或直接烧录 boot.img，内容相同）"
}

# ---------- main ----------
log "工作目录: ${SCRIPT_DIR}"
log "交叉编译: ${CROSS_COMPILE}gcc ($(${CROSS_COMPILE}gcc -dumpmachine 2>/dev/null || echo '?'))"

BUILD_VER="$(bump_build_version_if_needed)"
log "本次 KBUILD_BUILD_VERSION / DLCVCAM_BUILD_VERSION = ${BUILD_VER}"

if need_initial_config; then
	do_initial_config
else
	log "检测到已有 .config，走增量编译"
fi

do_build
package_named_image
