#include "libhfuzz/libhfuzz.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "honggfuzz.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfuzz/instrument.h"

__attribute__((visibility("default"))) __attribute__((used))
const char* const LIBHFUZZ_module_persistent = "LIBHFUZZ_module_persistent";

__attribute__((weak)) int LLVMFuzzerInitialize(
    int* argc HF_ATTR_UNUSED, char*** argv HF_ATTR_UNUSED) {
    return 1;
}

__attribute__((weak)) size_t LLVMFuzzerMutate(
    uint8_t* Data HF_ATTR_UNUSED, size_t Size HF_ATTR_UNUSED, size_t MaxSize HF_ATTR_UNUSED) {
    LOG_F("LLVMFuzzerMutate() is not supported in honggfuzz yet");
    return 0;
}

__attribute__((weak)) int LLVMFuzzerTestOneInput(
    const uint8_t* buf HF_ATTR_UNUSED, size_t len HF_ATTR_UNUSED) {
    LOG_F(
        "Define 'int LLVMFuzzerTestOneInput(uint8_t * buf, size_t len)' in your "
        "code to make it work");
    return 0;
}

static const uint8_t* inputFile = NULL;
__attribute__((constructor)) static void initializePersistent(void) {
    if (fcntl(_HF_INPUT_FD, F_GETFD) == -1 && errno == EBADF) {
        return;
    }
    if ((inputFile = mmap(NULL, _HF_INPUT_MAX_SIZE, PROT_READ, MAP_SHARED, _HF_INPUT_FD, 0)) ==
        MAP_FAILED) {
        PLOG_F("mmap(fd=%d, size=%zu) of the input file failed", _HF_INPUT_FD,
            (size_t)_HF_INPUT_MAX_SIZE);
    }
}

void HonggfuzzFetchData(const uint8_t** buf_ptr, size_t* len_ptr) {
    static bool initialized = false;
    if (initialized) {
        if (!files_writeToFd(_HF_PERSISTENT_FD, &HFdoneTag, sizeof(HFdoneTag))) {
            LOG_F("writeToFd(size=%zu, doneTag) failed", sizeof(HFdoneTag));
        }
    } else {
        /*
         * Start coverage feedback from this point only (ignore coverage obtained during process
         * start-up)
         */
        instrumentClearNewCov();
        initialized = true;
    }

    uint64_t rcvLen;
    if (files_readFromFd(_HF_PERSISTENT_FD, (uint8_t*)&rcvLen, sizeof(rcvLen)) != sizeof(rcvLen)) {
        LOG_F("readFromFd(rcvLen, size=%zu) failed", sizeof(rcvLen));
    }

    *buf_ptr = inputFile;
    *len_ptr = (size_t)rcvLen;
}

void HF_ITER(const uint8_t** buf_ptr, size_t* len_ptr) {
    HonggfuzzFetchData(buf_ptr, len_ptr);
}

static void HonggfuzzRunOneInput(const uint8_t* buf, size_t len) {
    int ret = LLVMFuzzerTestOneInput(buf, len);
    if (ret != 0) {
        LOG_F("LLVMFuzzerTestOneInput() returned '%d' instead of '0'", ret);
    }
}

static void HonggfuzzPersistentLoop(void) {
    for (;;) {
        size_t len;
        const uint8_t* buf;

        HonggfuzzFetchData(&buf, &len);
        HonggfuzzRunOneInput(buf, len);
    }
}

static int HonggfuzzRunFromFile(int argc, char** argv) {
    int in_fd = STDIN_FILENO;
    const char* fname = "[STDIN]";
    if (argc > 1) {
        fname = argv[argc - 1];
        if ((in_fd = open(argv[argc - 1], O_RDONLY)) == -1) {
            PLOG_W("Cannot open '%s' as input, using stdin", argv[argc - 1]);
            in_fd = STDIN_FILENO;
            fname = "[STDIN]";
        }
    }

    LOG_I("Accepting input from '%s'", fname);
    LOG_I("Usage for fuzzing: honggfuzz -P [flags] -- %s", argv[0]);

    uint8_t* buf = (uint8_t*)util_Malloc(_HF_INPUT_MAX_SIZE);
    ssize_t len = files_readFromFd(in_fd, buf, _HF_INPUT_MAX_SIZE);
    if (len < 0) {
        LOG_E("Couldn't read data from stdin: %s", strerror(errno));
        free(buf);
        return -1;
    }

    HonggfuzzRunOneInput(buf, len);
    free(buf);
    return 0;
}

int HonggfuzzMain(int argc, char** argv) {
    LLVMFuzzerInitialize(&argc, &argv);

    if (inputFile) {
        HonggfuzzPersistentLoop();
    }

    return HonggfuzzRunFromFile(argc, argv);
}
