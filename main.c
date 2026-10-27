#include <asm/bootparam.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

static bool verbose;

#define log_verbose(...)                  \
    do {                                  \
        if (verbose)                      \
            fprintf(stderr, "[roro-vmm] " __VA_ARGS__); \
    } while (0)

struct options
{
    const char *kernel_path;
    const char *initramfs_path;
    bool verbose;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "usage: %s -k KERNEL -i INITRAMFS [-v]\n"
                    "  -k, --kernel PATH     Kernel image (required)\n"
                    "  -i, --initramfs PATH  Initramfs image (required)\n"
                    "  -v, --verbose        Print VMM diagnostics\n"
                    "  -h, --help           Show this help\n", program);
}

static struct options parse_options(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"kernel", required_argument, NULL, 'k'},
        {"initramfs", required_argument, NULL, 'i'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct options options = {0};
    int option;

    while ((option = getopt_long(argc, argv, "k:i:vh", long_options, NULL)) != -1)
    {
        switch (option)
        {
        case 'k':
            options.kernel_path = optarg;
            break;
        case 'i':
            options.initramfs_path = optarg;
            break;
        case 'v':
            options.verbose = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(stderr, argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (!options.kernel_path || !options.kernel_path[0] ||
        !options.initramfs_path || !options.initramfs_path[0] || optind != argc)
    {
        usage(stderr, argv[0]);
        exit(EXIT_FAILURE);
    }

    return options;
}

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
#define UART_IER_RDI 0x01
#define UART_IER_THRI 0x02
#define UART_IIR_NO_INT 0x01
#define UART_IIR_THRI 0x02
#define UART_IIR_RDI 0x04

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
    .iir = UART_IIR_NO_INT,
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
        if ((uart.ier & UART_IER_RDI) && (uart.lsr & UART_DR))
            uart.iir = UART_IIR_RDI;
        else if ((uart.ier & UART_IER_THRI) && (uart.lsr & UART_THRE))
            uart.iir = UART_IIR_THRI;
        else
            uart.iir = UART_IIR_NO_INT;

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

static struct termios saved_termios;
static int termios_changed;
static volatile sig_atomic_t stop_requested;

static void restore_terminal(void)
{
    if (termios_changed)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        termios_changed = 0;
    }
}

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void configure_terminal(void)
{
    struct sigaction action = {
        .sa_handler = request_stop,
    };
    struct termios guest_termios;

    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1)
        err(1, "tcgetattr stdin");

    guest_termios = saved_termios;
    guest_termios.c_lflag &= ~(ICANON | ECHO);
    guest_termios.c_cc[VMIN] = 0;
    guest_termios.c_cc[VTIME] = 0;

    if (atexit(restore_terminal) != 0)
        errx(1, "atexit");
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &guest_termios) == -1)
        err(1, "tcsetattr stdin");
    termios_changed = 1;

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1)
        err(1, "sigaction SIGINT");
}

int read_char(void)
{
    unsigned char c;

    ssize_t n = read(STDIN_FILENO, &c, 1);

    if (n == 1)
        return c;

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -1; // nothing available

    return -1;
}

struct vm
{
    uint8_t *mem;
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    struct kvm_run *run;
    size_t run_mmap_size;
};

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

struct initramfs_meta
{
    uint64_t gpa;
    size_t size;
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

    log_verbose("raw setup_sects: %u\n", raw_setup_sects);
    log_verbose("total setup sectors: %u\n", (unsigned int)setup_sects + 1);
    log_verbose("setup header end: 0x%zx\n", setup_header_end);
    log_verbose("setup header size: %zu\n", setup_header_size);
    log_verbose("protocol version: %u.%u (0x%04" PRIx16 ")\n",
           setup_header.version >> 8, setup_header.version & 0xff,
           (uint16_t)setup_header.version);
    log_verbose("kernel payload offset: 0x%" PRIx64 "\n", payload_offset);
    log_verbose("kernel payload size: %" PRIu64 "\n", payload_size);
    log_verbose("code32_start: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.code32_start);
    log_verbose("kernel_alignment: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.kernel_alignment);
    log_verbose("relocatable_kernel: %s\n",
           setup_header.relocatable_kernel ? "yes" : "no");
    log_verbose("initrd_addr_max: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.initrd_addr_max);
    log_verbose("cmdline_size: %" PRIu32 "\n",
           (uint32_t)setup_header.cmdline_size);
    log_verbose("preferred_address: 0x%016" PRIx64 "\n",
           (uint64_t)setup_header.pref_address);
    log_verbose("init_size: 0x%08" PRIx32 "\n",
           (uint32_t)setup_header.init_size);
}

static struct kern_meta load_kernel(struct vm *vm, const char *path)
{
    struct mapped_file kernel = map_file_readonly(path);
    struct kern_meta meta;

    log_verbose("kernel: %s (%zu bytes)\n", kernel.path, kernel.size);
    inspect_kernel(&kernel, &meta);

    if (meta.payload_size > GUEST_MEM_SIZE - KERNEL_LOAD_GPA)
        errx(1, "kernel payload does not fit in guest RAM");
    memcpy(vm->mem + KERNEL_LOAD_GPA,
           kernel.data + (size_t)meta.payload_offset,
           (size_t)meta.payload_size);
    log_verbose("kernel payload: [0x%08x, 0x%08" PRIx64 ")\n",
                KERNEL_LOAD_GPA, KERNEL_LOAD_GPA + meta.payload_size);

    unmap_file(&kernel);
    return meta;
}

static struct initramfs_meta load_initramfs(struct vm *vm, const char *path,
                                          const struct kern_meta *kernel)
{
    struct mapped_file initramfs = map_file_readonly(path);
    struct initramfs_meta meta;
    uint64_t initramfs_end, kernel_workspace_end;

    log_verbose("initramfs: %s (%zu bytes)\n", initramfs.path, initramfs.size);

    if (initramfs.size > GUEST_MEM_SIZE)
        errx(1, "initramfs does not fit in guest RAM");
    meta.gpa = align_down(GUEST_MEM_SIZE - initramfs.size, 4096);
    meta.size = initramfs.size;
    initramfs_end = meta.gpa + meta.size;
    kernel_workspace_end = KERNEL_LOAD_GPA + kernel->setup_header.init_size;
    if (meta.gpa < kernel_workspace_end)
        errx(1, "initramfs overlaps the kernel initialization workspace");
    if (initramfs_end - 1 > kernel->setup_header.initrd_addr_max)
        errx(1, "initramfs exceeds the kernel's maximum initrd address");

    memcpy(vm->mem + (size_t)meta.gpa, initramfs.data, meta.size);
    log_verbose("initramfs: [0x%08" PRIx64 ", 0x%08" PRIx64 ")\n",
                meta.gpa, initramfs_end);

    unmap_file(&initramfs);
    return meta;
}

static void init_virtual_hw(struct vm *vm)
{
    int ret;

    vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vm->kvm_fd == -1)
        err(1, "/dev/kvm");

    ret = ioctl(vm->kvm_fd, KVM_GET_API_VERSION, NULL);
    if (ret == -1)
        err(1, "KVM_GET_API_VERSION");
    if (ret != 12)
        errx(1, "KVM_GET_API_VERSION %d, expected 12", ret);

    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, (unsigned long)0);
    if (vm->vm_fd == -1)
        err(1, "KVM_CREATE_VM");

    uint64_t identity_map_gpa = KVM_IDENTITY_MAP_GPA;
    ret = ioctl(vm->vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &identity_map_gpa);
    if (ret == -1)
        err(1, "KVM_SET_IDENTITY_MAP_ADDR");
    ret = ioctl(vm->vm_fd, KVM_SET_TSS_ADDR, KVM_TSS_GPA);
    if (ret == -1)
        err(1, "KVM_SET_TSS_ADDR");
    ret = ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP, NULL);
    if (ret == -1)
        err(1, "KVM_CREATE_IRQCHIP");

    struct kvm_pit_config pit_config = {
        .flags = KVM_PIT_SPEAKER_DUMMY,
    };
    ret = ioctl(vm->vm_fd, KVM_CREATE_PIT2, &pit_config);
    if (ret == -1)
        err(1, "KVM_CREATE_PIT2");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = GUEST_MEM_SIZE,
        .userspace_addr = (uint64_t)vm->mem,
    };
    ret = ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1)
        err(1, "KVM_SET_USER_MEMORY_REGION");
    vm->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, (unsigned long)0);
    if (vm->vcpu_fd == -1)
        err(1, "KVM_CREATE_VCPU");

    ret = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (ret == -1)
        err(1, "KVM_GET_VCPU_MMAP_SIZE");
    vm->run_mmap_size = ret;
    if (vm->run_mmap_size < sizeof(*vm->run))
        errx(1, "KVM_GET_VCPU_MMAP_SIZE unexpectedly small");
    vm->run = mmap(NULL, vm->run_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vm->vcpu_fd, 0);
    if (vm->run == MAP_FAILED)
        err(1, "mmap vcpu");

    enum { CPUID_NENT = 100 };

    size_t size =
        sizeof(struct kvm_cpuid2) +
        CPUID_NENT * sizeof(struct kvm_cpuid_entry2);

    struct kvm_cpuid2 *cpuid = calloc(1, size);
    if (!cpuid)
        err(1, "calloc");

    cpuid->nent = CPUID_NENT;

    if (ioctl(vm->kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) == -1)
        err(1, "KVM_GET_SUPPORTED_CPUID");
    if (ioctl(vm->vcpu_fd, KVM_SET_CPUID2, cpuid) == -1)
        err(1, "KVM_SET_CPUID2");
    free(cpuid);
}

static void prepare_linux_32bit_boot_protocol(struct vm *vm,
                                             const struct kern_meta *kernel,
                                             const struct initramfs_meta *initramfs)
{
    static const char cmdline[] =
        "console=ttyS0,115200 earlycon=uart8250,io,0x3f8 "
        "rdinit=/init nokaslr";
    struct boot_params *boot_params;
    struct kvm_sregs sregs;
    int ret;

    boot_params = (struct boot_params *)(vm->mem + BOOT_PARAMS_GPA);
    memset(boot_params, 0, sizeof(*boot_params));
    memcpy(&boot_params->hdr, &kernel->setup_header, sizeof(kernel->setup_header));
    boot_params->hdr.type_of_loader = LOADER_TYPE_UNDEFINED;
    boot_params->hdr.code32_start = KERNEL_LOAD_GPA;
    log_verbose("boot_params: [0x%08x, 0x%08zx)\n",
           BOOT_PARAMS_GPA, BOOT_PARAMS_GPA + sizeof(*boot_params));

    if (strlen(cmdline) > boot_params->hdr.cmdline_size)
        errx(1, "kernel command line exceeds the supported size");
    if (sizeof(cmdline) > LOW_MEMORY_END - CMDLINE_GPA)
        errx(1, "kernel command line does not fit below low-memory limit");
    memcpy(vm->mem + CMDLINE_GPA, cmdline, sizeof(cmdline));
    boot_params->hdr.cmd_line_ptr = CMDLINE_GPA;
    log_verbose("command line: GPA 0x%08x (%zu bytes including NUL)\n",
           CMDLINE_GPA, sizeof(cmdline));

    boot_params->hdr.ramdisk_image = (uint32_t)initramfs->gpa;
    boot_params->hdr.ramdisk_size = (uint32_t)initramfs->size;

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

    log_verbose("E820 RAM:      [0x%08x, 0x%08x)\n", 0, E820_LOW_RAM_END);
    log_verbose("E820 RESERVED: [0x%08x, 0x%08x)\n",
           E820_LOW_RAM_END, KERNEL_LOAD_GPA);
    log_verbose("E820 RAM:      [0x%08x, 0x%08x)\n",
           KERNEL_LOAD_GPA, GUEST_MEM_SIZE);

    /* Selectors 0x10 and 0x18 refer to GDT entries 2 and 3. */
    const uint64_t gdt[] = {
        0,
        0,
        0x00cf9b000000ffffULL,
        0x00cf93000000ffffULL,
    };
    memcpy(vm->mem + GDT_GPA, gdt, sizeof(gdt));

    ret = ioctl(vm->vcpu_fd, KVM_GET_SREGS, &sregs);
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
    ret = ioctl(vm->vcpu_fd, KVM_SET_SREGS, &sregs);
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
    ret = ioctl(vm->vcpu_fd, KVM_SET_REGS, &regs);
    if (ret == -1)
        err(1, "KVM_SET_REGS");
}

static void handle_vm_io(struct vm *vm)
{
    uint8_t *data = (uint8_t *)vm->run + vm->run->io.data_offset;

    bool is_uart = vm->run->io.port >= UART_BASE &&
                   vm->run->io.port <= UART_BASE + 7;
    bool is_rtc = (vm->run->io.direction == KVM_EXIT_IO_OUT &&
                   vm->run->io.port == 0x70) ||
                  (vm->run->io.direction == KVM_EXIT_IO_IN &&
                   vm->run->io.port == 0x71);

    if (vm->run->io.count != 1)
        errx(1, "unhandled KVM_EXIT_IO: port=0x%x size=%u count=%u",
             vm->run->io.port, vm->run->io.size, vm->run->io.count);

    if ((is_uart || is_rtc) && vm->run->io.size != 1)
        errx(1, "unhandled KVM_EXIT_IO: direction=%s port=0x%x size=%u",
             vm->run->io.direction == KVM_EXIT_IO_IN ? "in" : "out",
             vm->run->io.port, vm->run->io.size);

    if (vm->run->io.direction == KVM_EXIT_IO_OUT && is_uart)
    {
        uart_write(vm->run->io.port, data[0]);
    }
    else if (vm->run->io.direction == KVM_EXIT_IO_IN && is_uart)
    {
        data[0] = uart_read(vm->run->io.port);
    }
    else if (vm->run->io.direction == KVM_EXIT_IO_OUT && vm->run->io.port == 0x70)
    {
        rtc.index = data[0];
    }
    else if (vm->run->io.direction == KVM_EXIT_IO_IN && vm->run->io.port == 0x71)
    {
        data[0] = 0; // Return 0 for all RTC reads
    }
    else if (vm->run->io.direction == KVM_EXIT_IO_IN)
    {
        memset(data, 0xff, vm->run->io.size * vm->run->io.count);
    }
    else if (vm->run->io.direction == KVM_EXIT_IO_OUT)
    {
        log_verbose("KVM_EXIT_IO_OUT: port=0x%x size=%u count=%u\n",
               vm->run->io.port, vm->run->io.size, vm->run->io.count);
    }
    else
    {
        errx(1, "unhandled KVM_EXIT_IO: direction=%s port=0x%x",
             vm->run->io.direction == KVM_EXIT_IO_IN ? "in" : "out",
             vm->run->io.port);
    }
}

int main(int argc, char **argv)
{
    struct options options = parse_options(argc, argv);
    struct vm vm = {
        .kvm_fd = -1,
        .vm_fd = -1,
        .vcpu_fd = -1,
    };
    int ret;
    struct initramfs_meta initramfs;
    struct kern_meta meta;

    verbose = options.verbose;

    vm.mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm.mem == MAP_FAILED)
        err(1, "allocating guest memory");

    meta = load_kernel(&vm, options.kernel_path);

    initramfs = load_initramfs(&vm, options.initramfs_path, &meta);

    init_virtual_hw(&vm);
    prepare_linux_32bit_boot_protocol(&vm, &meta, &initramfs);

    configure_terminal();
    while (1)
    {
        if (stop_requested)
            goto cleanup;

        if (!(uart.lsr & UART_DR))
        {
            int c = read_char();
            if (c != -1)
                uart_rx((uint8_t)c);
        }
        ret = ioctl(vm.vcpu_fd, KVM_RUN, NULL);
        if (ret == -1)
        {
            if (errno == EINTR && stop_requested)
                goto cleanup;
            err(1, "KVM_RUN");
        }
        switch (vm.run->exit_reason)
        {
        case KVM_EXIT_HLT:
            log_verbose("KVM_EXIT_HLT\n");
            goto cleanup;
        case KVM_EXIT_IO:
            handle_vm_io(&vm);
            break;
        case KVM_EXIT_FAIL_ENTRY:
            errx(1, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx",
                 (unsigned long long)vm.run->fail_entry.hardware_entry_failure_reason);
        case KVM_EXIT_INTERNAL_ERROR:
            errx(1, "KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x", vm.run->internal.suberror);
        case KVM_EXIT_MMIO:
            errx(1, "KVM_EXIT_MMIO: %s address=0x%llx length=%u",
                 vm.run->mmio.is_write ? "write" : "read",
                 (unsigned long long)vm.run->mmio.phys_addr,
                 vm.run->mmio.len);
        default:
            errx(1, "exit_reason = 0x%x", vm.run->exit_reason);
        }
    }

cleanup:
    restore_terminal();
    if (munmap(vm.mem, GUEST_MEM_SIZE) == -1)
        err(1, "munmap guest memory");
    return 0;
}
