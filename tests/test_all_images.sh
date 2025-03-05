#!/usr/bin/env bash
#set -x

IMAGES_REPO="https://github.com/fatprogs/fat32_bad_images.git"
REPO_NAME="fat32_bad_images"
PWD=`pwd`
FSCK_PATH=$PWD/../src/
CHECKOUT_BR=${1:-"main"}

FATPROGS_FSCK="${FSCK_PATH}/dosfsck"
DOSFSTOOLS_FSCK="/usr/sbin/fsck.fat"
TEST_DIR="."

echo "Download corrupted images..."

if [ -d "${REPO_NAME}" ]; then
	cd ${REPO_NAME}
	git fetch
	if [ $? -ne 0 ]; then
		echo "git fetch FAILED. exit"
		exit
	fi
	cd ..
else
	git clone ${IMAGES_REPO} ${REPO_NAME}
fi

if [ ${CHECKOUT_BR} != "main" ]; then
	echo "Checking out ${CHECKOUT_BR}... "

	cd ${REPO_NAME} && git checkout origin/${CHECKOUT_BR} -b ${CHECKOUT_BR}
	RET=$?
	cd ..
else
	echo "Rebasing latest origin/main... "
	cd ${REPO_NAME} && git checkout main && git rebase origin/main
	RET=$?
	cd ..
fi

if [ $RET -ne 0 ]; then
	echo "Failed to checkout or rebase ${CHECKOUT_BR}"
	exit 1
fi

# test created_manually directory
echo "====================="
echo "Test corrupted images"
echo "====================="
sleep 2
/bin/bash -c "cd ${REPO_NAME} && ./script/test_fsck_verification.py ${FATPROGS_FSCK} ${DOSFSTOOLS_FSCK} ${TEST_DIR}"
RET=$?
if [ $RET -ne 0 ]; then
	echo "Failed to test for repairing corrupted images"
	exit 1
fi

if [ ${CHECKOUT_BR} != "main" ]; then
	echo "Delete working branch ${CHECKOUT_BR}... "
	/bin/bash -c "cd ${REPO_NAME} && git branch -D ${CHECKOUT_BR}"
fi

echo "Sucess to test corrupted images"
#rm -rf ${REPO_NAME}
