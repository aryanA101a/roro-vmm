#include <asm/bootparam.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define GUEST_MEM_SIZE 0x08000000
#define BOOT_PARAMS_GPA 0x00007000
#define GDT_GPA 0x00005000
#define CMDLINE_GPA 0x00020000
#define LOW_MEMORY_END 0x000a0000
#define KERNEL_LOAD_GPA 0x00100000
#define E820_LOW_RAM_END 0x0009fc00
#define E820_ENTRY_TYPE_RAM 1
#define E820_ENTRY_TYPE_RESERVED 2
#define KVM_IDENTITY_MAP_GPA 0xfffbc000ULL
#define KVM_TSS_GPA 0xfffbd000UL
#define MIN_KERNEL_FILE_SIZE 0x208
#define SETUP_HEADER_OFFSET 0x1f1
#define SETUP_HEADER_END_BASE 0x202
#define SETUP_SECTOR_SIZE 512
#define X86_BOOT_FLAG 0xaa55
#define X86_HEADER_MAGIC 0x53726448
#define XLF_LOADED_HIGH 0x01
#define LOADER_TYPE_UNDEFINED 0xff
#define CR0_PE_BIT (1ULL << 0)
#define CR0_PG_BIT (1ULL << 31)

#define UART_BASE 0x3f8

#define UART_DR 0x01
#define UART_THRE 0x20
#define UART_TEMT 0x40

struct uart
{
    uint8_t rbr;
    uint8_t thr;
    uint8_t ier;
    uint8_t iir;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
};

static struct uart uart = {
    .iir = 0x01,                 // no interrupt pending
    .lsr = UART_THRE | UART_TEMT // transmitter ready/empty
};

static void uart_tx(uint8_t byte)
{
    putchar(byte);
    fflush(stdout);
}

void uart_rx(uint8_t byte)
{
    /*
     * Don't overwrite an unread byte.
     */
    if (!(uart.lsr & UART_DR))
    {
        uart.rbr = byte;
        uart.lsr |= UART_DR;
    }
}

void uart_write(uint16_t port, uint8_t value)
{
    uint16_t off = port - UART_BASE;
    int dlab = uart.lcr & 0x80;

    switch (off)
    {
    case 0:
        if (dlab)
        {
            uart.dll = value;
        }
        else
        {
            uart.thr = value;
            uart_tx(uart.thr);
        }
        break;

    case 1:
        if (dlab)
        {
            uart.dlm = value;
        }
        else
        {
            uart.ier = value;
        }
        break;

    case 2:
        /*
         * FCR write.
         *
         * We aren't implementing FIFOs yet.
         * Bit 1 clears receive state.
         */
        if (value & 0x02)
            uart.lsr &= ~UART_DR;

        break;

    case 3:
        uart.lcr = value;
        break;

    case 4:
        uart.mcr = value;
        break;

    case 7:
        uart.scr = value;
        break;

    default:
        break;
    }
}

uint8_t uart_read(uint16_t port)
{
    uint16_t off = port - UART_BASE;
    int dlab = uart.lcr & 0x80;

    switch (off)
    {
    case 0:
        if (dlab)
        {
            return uart.dll;
        }
        else
        {
            uint8_t value = uart.rbr;

            uart.lsr &= ~UART_DR;

            return value;
        }

    case 1:
        if (dlab)
            return uart.dlm;

        return uart.ier;

    case 2:
        return uart.iir;

    case 3:
        return uart.lcr;

    case 4:
        return uart.mcr;

    case 5:
        return uart.lsr;

    case 6:
        return uart.msr;

    case 7:
        return uart.scr;

    default:
        return 0xff;
    }
}

struct rtc
{
    uint8_t index;
};

static struct rtc rtc;

struct mapped_file
{
    const char *path;
    uint8_t *data;
    size_t size;
};

struct kern_meta
{
    struct setup_header setup_header;
    uint64_t payload_offset;
    uint64_t payload_size;
};

uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

static struct mapped_file map_file_readonly(const char *path)
{
    struct mapped_file file = {.path = path};
    struct stat stat;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        err(1, "%s", path);
    if (fstat(fd, &stat) == -1)
        err(1, "fstat %s", path);
    if (stat.st_size <= 0)
        errx(1, "%s: file is empty", path);
    if ((uintmax_t)stat.st_size > SIZE_MAX)
        errx(1, "%s: file is too large to map", path);

    file.size = (size_t)stat.st_size;
    file.data = mmap(NULL, file.size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file.data == MAP_FAILED)
        err(1, "mmap %s", path);
    if (close(fd) == -1)
        err(1, "close %s", path);

    return file;
}

static void unmap_file(struct mapped_file *file)
{
    if (munmap(file->data, file->size) == -1)
        err(1, "munmap %s", file->path);
    file->data = NULL;
    file->size = 0;
}

static void inspect_kernel(const struct mapped_file *kernel, struct kern_meta *meta)
{
    struct setup_header setup_header;
    uint8_t raw_setup_sects, setup_sects, jump_offset;
    size_t setup_header_end, setup_header_size;
    uint64_t payload_offset, payload_size;

    if (kernel->size < MIN_KERNEL_FILE_SIZE)
        errx(1, "%s: too small to contain an x86 kernel header",
             kernel->path);

    raw_setup_sects = kernel->data[SETUP_HEADER_OFFSET];
    setup_sects = raw_setup_sects == 0 ? 4 : raw_setup_sects;
    jump_offset = kernel->data[0x201];
    setup_header_end = SETUP_HEADER_END_BASE + jump_offset;
    setup_header_size = setup_header_end - SETUP_HEADER_OFFSET;

    if (setup_header_end > kernel->size)
        errx(1, "%s: setup header extends past end of file", kernel->path);
    if (setup_header_size < sizeof(setup_header))
        errx(1, "%s: setup header is too short (%zu bytes, need %zu)",
             kernel->path, setup_header_size, sizeof(setup_header));

    memcpy(&setup_header, kernel->data + SETUP_HEADER_OFFSET,
           sizeof(setup_header));

    if (setup_header.boot_flag != X86_BOOT_FLAG)
        errx(1, "%s: invalid x86 boot flag 0x%04" PRIx16, kernel->path,
             (uint16_t)setup_header.boot_flag);
    if (setup_header.header != X86_HEADER_MAGIC)
        errx(1, "%s: missing HdrS signature", kernel->path);
    if (setup_header.version < 0x020a)
        errx(1, "%s: boot protocol 0x%04" PRIx16 " is older than the required 2.10",
             kernel->path, (uint16_t)setup_header.version);
    if ((setup_header.loadflags & XLF_LOADED_HIGH) == 0)
        errx(1, "%s: kernel is not a bzImage loaded-high image", kernel->path);

    payload_offset = ((uint64_t)setup_sects + 1) * SETUP_SECTOR_SIZE;
    if (payload_offset > kernel->size)
        errx(1, "%s: kernel payload offset 0x%" PRIx64 " extends past end of file",
             kernel->path, payload_offset);
    payload_size = kernel->size - payload_offset;

    meta->payload_offset = payload_offset;
    meta->payload_size = payload_size;
    meta->setup_header = setup_header;

    printf("raw setup_sects: %u\n", raw_setup_sects);
    printf("total setup sectors: %u\n", (unsigned int)setup_sects + 1);
    printf("setup header end: 0x%zx\n", setup_header_end);
    printf("setup header size: %zu\n", setup_header_size);
    printf("protocol version: %u.%u (0x%04" PRIx16 ")\n",
           setup_header.version >> 8, setup_header.version & 0xff,
           (uint16_t)setup_header.version);
    printf("kernel payload offset: 0x%" PRIx64 "\n", payload_offset);
    printf("kernel payload size: %" PRIu64 "\n", payload_size);
    printf("code32_start: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.code32_start);
    printf("kernel_alignment: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.kernel_alignment);
    printf("relocatable_kernel: %s\n",
           setup_header.relocatable_kernel ? "yes" : "no");
    printf("initrd_addr_max: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.initrd_addr_max);
    printf("cmdline_size: %" PRIu32 "\n",
           (uint32_t)setup_header.cmdline_size);
    printf("preferred_address: 0x%016" PRIx64 "\n",
           (uint64_t)setup_header.pref_address);
    printf("init_size: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.init_size);
}

int main(int argc, char **argv)
{
    static const char cmdline[] =
        "console=ttyS0,115200 earlycon=uart8250,io,0x3f8 "
        "rdinit=/init nokaslr";
    const char *kernel_path, *initramfs_path;
    int kvm, vmfd, vcpufd, ret;
    struct mapped_file initramfs, kernel;
    struct kern_meta meta;
    struct boot_params *boot_params;
    uint8_t *mem;
    struct kvm_sregs sregs;
    size_t mmap_size;
    struct kvm_run *run;
    uint64_t initramfs_end, initramfs_gpa, kernel_workspace_end;
    uint32_t pci_config_address = 0;

    if (argc != 3)
        errx(1, "usage: %s KERNEL INITRAMFS", argv[0]);

    kernel_path = argv[1];
    initramfs_path = argv[2];

    kernel = map_file_readonly(kernel_path);
    initramfs = map_file_readonly(initramfs_path);

    printf("kernel: %s (%zu bytes)\n", kernel.path, kernel.size);
    printf("initramfs: %s (%zu bytes)\n", initramfs.path, initramfs.size);
    inspect_kernel(&kernel, &meta);

    mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        err(1, "allocating guest memory");

    if (meta.payload_size > GUEST_MEM_SIZE - KERNEL_LOAD_GPA)
        errx(1, "kernel payload does not fit in guest RAM");
    memcpy(mem + KERNEL_LOAD_GPA,
           kernel.data + (size_t)meta.payload_offset,
           (size_t)meta.payload_size);
    printf("kernel payload: [0x%08x, 0x%08" PRIx64 ")\n",
           KERNEL_LOAD_GPA, KERNEL_LOAD_GPA + meta.payload_size);

    boot_params = (struct boot_params *)(mem + BOOT_PARAMS_GPA);
    memset(boot_params, 0, sizeof(*boot_params));
    memcpy(&boot_params->hdr, &meta.setup_header, sizeof(meta.setup_header));
    boot_params->hdr.type_of_loader = LOADER_TYPE_UNDEFINED;
    boot_params->hdr.code32_start = KERNEL_LOAD_GPA;
    printf("boot_params: [0x%08x, 0x%08zx)\n",
           BOOT_PARAMS_GPA, BOOT_PARAMS_GPA + sizeof(*boot_params));

    if (strlen(cmdline) > boot_params->hdr.cmdline_size)
        errx(1, "kernel command line exceeds the supported size");
    if (sizeof(cmdline) > LOW_MEMORY_END - CMDLINE_GPA)
        errx(1, "kernel command line does not fit below low-memory limit");
    memcpy(mem + CMDLINE_GPA, cmdline, sizeof(cmdline));
    boot_params->hdr.cmd_line_ptr = CMDLINE_GPA;
    printf("command line: GPA 0x%08x (%zu bytes including NUL)\n",
           CMDLINE_GPA, sizeof(cmdline));

    if (initramfs.size > GUEST_MEM_SIZE)
        errx(1, "initramfs does not fit in guest RAM");
    initramfs_gpa = align_down(GUEST_MEM_SIZE - initramfs.size, 4096);
    initramfs_end = initramfs_gpa + initramfs.size;
    kernel_workspace_end = KERNEL_LOAD_GPA + meta.setup_header.init_size;
    if (initramfs_gpa < kernel_workspace_end)
        errx(1, "initramfs overlaps the kernel initialization workspace");
    if (initramfs_end - 1 > meta.setup_header.initrd_addr_max)
        errx(1, "initramfs exceeds the kernel's maximum initrd address");

    memcpy(mem + (size_t)initramfs_gpa, initramfs.data, initramfs.size);
    boot_params->hdr.ramdisk_image = (uint32_t)initramfs_gpa;
    boot_params->hdr.ramdisk_size = (uint32_t)initramfs.size;
    printf("initramfs: [0x%08" PRIx64 ", 0x%08" PRIx64 ")\n",
           initramfs_gpa, initramfs_end);

    boot_params->e820_table[0] = (struct boot_e820_entry){
        .addr = 0,
        .size = E820_LOW_RAM_END,
        .type = E820_ENTRY_TYPE_RAM,
    };
    boot_params->e820_table[1] = (struct boot_e820_entry){
        .addr = E820_LOW_RAM_END,
        .size = KERNEL_LOAD_GPA - E820_LOW_RAM_END,
        .type = E820_ENTRY_TYPE_RESERVED,
    };
    boot_params->e820_table[2] = (struct boot_e820_entry){
        .addr = KERNEL_LOAD_GPA,
        .size = GUEST_MEM_SIZE - KERNEL_LOAD_GPA,
        .type = E820_ENTRY_TYPE_RAM,
    };
    boot_params->e820_entries = 3;

    printf("E820 RAM:      [0x%08x, 0x%08x)\n", 0, E820_LOW_RAM_END);
    printf("E820 RESERVED: [0x%08x, 0x%08x)\n",
           E820_LOW_RAM_END, KERNEL_LOAD_GPA);
    printf("E820 RAM:      [0x%08x, 0x%08x)\n",
           KERNEL_LOAD_GPA, GUEST_MEM_SIZE);

    /* Steps 10-12 will continue from this KVM scaffold. */
    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm == -1)
        err(1, "/dev/kvm");

    ret = ioctl(kvm, KVM_GET_API_VERSION, NULL);
    if (ret == -1)
        err(1, "KVM_GET_API_VERSION");
    if (ret != 12)
        errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);

    vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);
    if (vmfd == -1)
        err(1, "KVM_CREATE_VM");

    uint64_t identity_map_gpa = KVM_IDENTITY_MAP_GPA;
    ret = ioctl(vmfd, KVM_SET_IDENTITY_MAP_ADDR, &identity_map_gpa);
    if (ret == -1)
        err(1, "KVM_SET_IDENTITY_MAP_ADDR");
    ret = ioctl(vmfd, KVM_SET_TSS_ADDR, KVM_TSS_GPA);
    if (ret == -1)
        err(1, "KVM_SET_TSS_ADDR");
    ret = ioctl(vmfd, KVM_CREATE_IRQCHIP, NULL);
    if (ret == -1)
        err(1, "KVM_CREATE_IRQCHIP");

    struct kvm_pit_config pit_config = {
        .flags = KVM_PIT_SPEAKER_DUMMY,
    };
    ret = ioctl(vmfd, KVM_CREATE_PIT2, &pit_config);
    if (ret == -1)
        err(1, "KVM_CREATE_PIT2");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = GUEST_MEM_SIZE,
        .userspace_addr = (uint64_t)mem,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1)
        err(1, "KVM_SET_USER_MEMORY_REGION");
    vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)0);
    if (vcpufd == -1)
        err(1, "KVM_CREATE_VCPU");

    ret = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (ret == -1)
        err(1, "KVM_GET_VCPU_MMAP_SIZE");
    mmap_size = ret;
    if (mmap_size < sizeof(*run))
        errx(1, "KVM_GET_VCPU_MMAP_SIZE unexpectedly small");
    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    if (run == MAP_FAILED)
        err(1, "mmap vcpu");

#define NENT 100

    size_t size =
        sizeof(struct kvm_cpuid2) +
        NENT * sizeof(struct kvm_cpuid_entry2);

    struct kvm_cpuid2 *cpuid = calloc(1, size);
    if (!cpuid)
        err(1, "calloc");

    cpuid->nent = NENT;

    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) == -1)
        err(1, "KVM_GET_SUPPORTED_CPUID");
    if (ioctl(vcpufd, KVM_SET_CPUID2, cpuid) == -1)
        err(1, "KVM_SET_CPUID2");
    free(cpuid);

    /* Selectors 0x10 and 0x18 refer to GDT entries 2 and 3. */
    const uint64_t gdt[] = {
        0,
        0,
        0x00cf9b000000ffffULL,
        0x00cf93000000ffffULL,
    };
    memcpy(mem + GDT_GPA, gdt, sizeof(gdt));

    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    if (ret == -1)
        err(1, "KVM_GET_SREGS");
    sregs.gdt.base = GDT_GPA;
    sregs.gdt.limit = sizeof(gdt) - 1;

    sregs.cs.base = 0;
    sregs.cs.selector = 0x10;
    sregs.cs.limit = 0xffffffff;
    sregs.cs.type = 0xb;
    sregs.cs.dpl = 0;
    sregs.cs.present = 1;
    sregs.cs.s = 1;
    sregs.cs.g = 1;
    sregs.cs.db = 1;
    sregs.cs.l = 0;
    sregs.cs.avl = 0;
    sregs.cs.unusable = 0;

    sregs.ds.base = 0;
    sregs.ds.selector = 0x18;
    sregs.ds.limit = 0xffffffff;
    sregs.ds.type = 0x3;
    sregs.ds.dpl = 0;
    sregs.ds.present = 1;
    sregs.ds.s = 1;
    sregs.ds.g = 1;
    sregs.ds.db = 1;
    sregs.ds.l = 0;
    sregs.ds.avl = 0;
    sregs.ds.unusable = 0;
    sregs.es = sregs.ds;
    sregs.ss = sregs.ds;

    sregs.cr0 |= CR0_PE_BIT;
    sregs.cr0 &= ~CR0_PG_BIT;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1)
        err(1, "KVM_SET_SREGS");

    struct kvm_regs regs = {
        .rip = KERNEL_LOAD_GPA,
        .rsi = BOOT_PARAMS_GPA,
        .rbx = 0,
        .rbp = 0,
        .rdi = 0,
        .rflags = 0x2,
    };
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if (ret == -1)
        err(1, "KVM_SET_REGS");

    while (1)
    {
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret == -1)
            err(1, "KVM_RUN");
        switch (run->exit_reason)
        {
        case KVM_EXIT_HLT:
            puts("KVM_EXIT_HLT");
            return 0;
        case KVM_EXIT_IO:
        {
            uint8_t *data = (uint8_t *)run + run->io.data_offset;

            if (run->io.count != 1)
                errx(1, "unhandled KVM_EXIT_IO: port=0x%x size=%u count=%u",
                     run->io.port, run->io.size, run->io.count);

            if (run->io.port >= 0xcf8 && run->io.port <= 0xcff)
            {
                if (run->io.direction == KVM_EXIT_IO_OUT)
                {
                    // ignore PCI config writes for now
                }
                else
                {
                    // indicate "no device present"
                    data[0] = 0xff;
                }

            }

            else if (run->io.size != 1)
            {
                errx(1, "unhandled KVM_EXIT_IO: direction=%s port=0x%x size=%u",
                     run->io.direction == KVM_EXIT_IO_IN ? "in" : "out",
                     run->io.port, run->io.size);
            }
            else if (run->io.direction == KVM_EXIT_IO_OUT && (run->io.port >= 0x3f8 && run->io.port <= 0x3ff))
            {
                uart_write(run->io.port, data[0]);
            }
            else if (run->io.direction == KVM_EXIT_IO_IN && (run->io.port >= 0x3f8 && run->io.port <= 0x3ff))
            {
                data[0] = uart_read(run->io.port);
            }
            else if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x70)
            {
                rtc.index = data[0];
            }
            else if (run->io.direction == KVM_EXIT_IO_IN && run->io.port == 0x71)
            {
                data[0] = 0; // Return 0 for all RTC reads
            }
            else if (run->io.direction == KVM_EXIT_IO_IN) {
                memset(data, 0xff, run->io.size * run->io.count);
            }
             else if (run->io.direction == KVM_EXIT_IO_OUT) {
                printf("KVM_EXIT_IO_OUT: port=0x%x size=%u count=%u data=",
                       run->io.port, run->io.size, run->io.count);
            }
            else
            {
                errx(1, "unhandled KVM_EXIT_IO: direction=%s port=0x%x",
                     run->io.direction == KVM_EXIT_IO_IN ? "in" : "out",
                     run->io.port);
            }
            break;
        }
        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
                 (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x", run->internal.suberror);
        case KVM_EXIT_MMIO:
            errx(1, "KVM_EXIT_MMIO: %s address=0x%llx length=%u",
                 run->mmio.is_write ? "write" : "read",
                 (unsigned long long)run->mmio.phys_addr,
                 run->mmio.len);
        default:
            errx(1, "exit_reason = 0x%x", run->exit_reason);
        }
    }

cleanup:
    if (munmap(mem, GUEST_MEM_SIZE) == -1)
        err(1, "munmap guest memory");
    unmap_file(&initramfs);
    unmap_file(&kernel);
    return 0;
}
