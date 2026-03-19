#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define READ_SIZE_BYTES (1U << 20)                    /* 1 MiB */
#define TARGET_OFFSET_BYTES (12ULL * (1ULL << 40))    /* 12 TiB */
#define DEFAULT_DEVICE "/dev/nvme0"

static const char *errno_category(int err)
{
    switch (err) {
    case EACCES:
    case EPERM:
        return "权限不足（通常需要 root 或 CAP_SYS_ADMIN）";
    case ENOENT:
        return "设备节点不存在";
    case ENODEV:
        return "设备不存在或已离线";
    case ENOTTY:
        return "设备不支持该 ioctl（设备类型或内核接口不匹配）";
    case EINVAL:
        return "参数非法（可能是 nsid/LBA/长度不合法，或设备不支持该命令）";
    case EFAULT:
        return "用户态缓冲区地址不可访问";
    case ENOMEM:
        return "内存不足";
    case EIO:
        return "底层 I/O 错误";
    case ETIMEDOUT:
        return "命令超时";
    default:
        return "未知系统错误类型";
    }
}

static const char *nvme_sct_name(unsigned int sct)
{
    switch (sct) {
    case 0:
        return "Generic Command Status";
    case 1:
        return "Command Specific Status";
    case 2:
        return "Media and Data Integrity Errors";
    case 3:
        return "Path Related Status";
    case 7:
        return "Vendor Specific";
    default:
        return "Reserved/Unknown SCT";
    }
}

/* 常见状态码示例，不完整但便于快速诊断。 */
static const char *nvme_status_hint(unsigned int sct, unsigned int sc)
{
    if (sct == 0) {
        switch (sc) {
        case 0x01: return "Invalid Command Opcode";
        case 0x02: return "Invalid Field in Command";
        case 0x05: return "Commands Aborted due to Power Loss Notification";
        case 0x06: return "Internal Device Error";
        case 0x80: return "LBA Out of Range";
        default:   return "Generic Status（未在内置映射表中）";
        }
    }
    if (sct == 1) {
        switch (sc) {
        case 0x00: return "Completion Queue Invalid";
        case 0x0b: return "Invalid Namespace or Format";
        default:   return "Command Specific Status（未在内置映射表中）";
        }
    }
    if (sct == 2) {
        switch (sc) {
        case 0x80: return "Write Fault";
        case 0x81: return "Unrecovered Read Error";
        case 0x82: return "End-to-end Guard Check Error";
        default:   return "Media/Data Integrity Status（未在内置映射表中）";
        }
    }
    return "未内置该 SCT/SC 的提示文本";
}

static void report_errno(const char *where)
{
    int err = errno;
    fprintf(stderr, "[系统错误] %s 失败: errno=%d (%s)\n",
            where, err, strerror(err));
    fprintf(stderr, "          错误类型: %s\n", errno_category(err));
}

static void report_nvme_status(int status)
{
    unsigned int raw = (unsigned int)status & 0xffffU;
    unsigned int sct = (raw >> 8) & 0x7U;
    unsigned int sc = raw & 0xffU;

    fprintf(stderr, "[NVMe状态错误] ioctl 返回状态码: %d (0x%x)\n", status, raw);
    fprintf(stderr, "  SCT=%u (%s), SC=0x%02x, 提示=%s\n",
            sct, nvme_sct_name(sct), sc, nvme_status_hint(sct, sc));
}

static void dump_first_128_bytes(const uint8_t *buf)
{
    const size_t len = 128;
    size_t i, j;

    printf("读取成功，前 %zu 字节如下（HEX + ASCII）:\n", len);
    for (i = 0; i < len; i += 16) {
        printf("%04zx  ", i);
        for (j = 0; j < 16; ++j) {
            printf("%02x ", buf[i + j]);
        }
        printf(" |");
        for (j = 0; j < 16; ++j) {
            unsigned char c = buf[i + j];
            printf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        printf("|\n");
    }
}

static int submit_io_read(int fd, uint32_t nsid, uint64_t slba, uint32_t lba_size, void *buf)
{
    uint32_t nblocks = READ_SIZE_BYTES / lba_size;
    struct nvme_passthru_cmd64 cmd64;

    memset(&cmd64, 0, sizeof(cmd64));
    cmd64.opcode = 0x02; /* NVMe NVM Read */
    cmd64.nsid = nsid;
    cmd64.addr = (uint64_t)(uintptr_t)buf;
    cmd64.data_len = READ_SIZE_BYTES;
    cmd64.cdw10 = (uint32_t)(slba & 0xffffffffULL);
    cmd64.cdw11 = (uint32_t)(slba >> 32);
    cmd64.cdw12 = nblocks - 1; /* NLB is zero-based */
    cmd64.timeout_ms = 30000;
    return ioctl(fd, NVME_IOCTL_IO64_CMD, &cmd64);
}

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : DEFAULT_DEVICE;
    int fd = -1;
    void *buf = NULL;
    int lba_size = 0;
    int nsid = 0;
    uint64_t slba;
    int ret;

    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("目标设备: %s\n", dev);
    printf("目标偏移: 12 TiB, 读取长度: 1 MiB\n");
    fflush(stdout);

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        report_errno("open");
        return 1;
    }

    nsid = ioctl(fd, NVME_IOCTL_ID);
    if (nsid < 0) {
        report_errno("ioctl(NVME_IOCTL_ID)");
        close(fd);
        return 1;
    }

    if (ioctl(fd, BLKSSZGET, &lba_size) < 0) {
        report_errno("ioctl(BLKSSZGET)");
        close(fd);
        return 1;
    }

    if (lba_size <= 0 || (READ_SIZE_BYTES % (uint32_t)lba_size) != 0) {
        fprintf(stderr, "逻辑块大小非法或与 1MiB 不对齐: lba_size=%d\n", lba_size);
        close(fd);
        return 1;
    }

    if ((TARGET_OFFSET_BYTES % (uint64_t)lba_size) != 0) {
        fprintf(stderr, "12TiB 无法被当前 LBA 大小整除: lba_size=%d\n", lba_size);
        close(fd);
        return 1;
    }
    slba = TARGET_OFFSET_BYTES / (uint64_t)lba_size;

    if (posix_memalign(&buf, 4096, READ_SIZE_BYTES) != 0 || buf == NULL) {
        fprintf(stderr, "posix_memalign 失败，无法分配 1MiB 缓冲区。\n");
        close(fd);
        return 1;
    }
    memset(buf, 0, READ_SIZE_BYTES);

    printf("LBA大小: %d bytes, NSID: %d, 起始SLBA: %" PRIu64 "\n", lba_size, nsid, slba);
    ret = submit_io_read(fd, (uint32_t)nsid, slba, (uint32_t)lba_size, buf);
    if (ret < 0) {
        report_errno("ioctl(NVME_IOCTL_IO64_CMD)");
        free(buf);
        close(fd);
        return 2;
    }
    if (ret > 0) {
        report_nvme_status(ret);
        free(buf);
        close(fd);
        return 3;
    }

    dump_first_128_bytes((const uint8_t *)buf);

    free(buf);
    close(fd);
    return 0;
}
