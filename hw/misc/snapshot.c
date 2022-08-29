#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "exec/ramblock.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "qapi/visitor.h"
#include "io/channel-buffer.h"
#include "migration/savevm.h"

#define TYPE_PCI_SNAPSHOT_DEVICE "snapshot"
typedef struct SnapshotState SnapshotState;
DECLARE_INSTANCE_CHECKER(SnapshotState, SNAPSHOT,
                         TYPE_PCI_SNAPSHOT_DEVICE)

struct SnapshotState {
    PCIDevice pdev;
    MemoryRegion mmio;

    // track saved stated to prevent re-saving
    bool is_saved;

    uintptr_t shared_addr; // sync to shared memory file
    size_t shared_size;
    uint8_t *saved_shared; // saved memory previously at shared_addr

    uint8_t *guest_mem;
    size_t guest_size;

    // saved cpu and devices state
    QIOChannelBuffer *ioc;
};

// memory save location (for better performance, use tmpfs)
const char *filepath = "/dev/shm/snapshot0";
const char *shared_mem_file = "/dev/shm/snapshot_data";

// restore shared memory to previous memory state
static void snapshot_mem_restore_shared(struct SnapshotState *s) {
    if (s->shared_addr != -1 && s->saved_shared != NULL) {
        // remove the shared memory
        munmap(s->guest_mem + s->shared_addr, s->shared_size);
        // restore what was previously at the shared memory
        mremap(s->saved_shared, s->shared_size, s->shared_size,
               MREMAP_MAYMOVE | MREMAP_FIXED, s->guest_mem + s->shared_addr);
        s->shared_addr = -1;
        s->saved_shared = NULL;
    }
}

// overwrite map with shared memory
static void snapshot_mem_init_shared(struct SnapshotState *s) {
    int fd = -1;
    size_t page_size = 0x1000;

    if (s->shared_addr != -1 && s->shared_addr < s->guest_size && (s->shared_addr & (page_size - 1)) == 0) {
        fd = open(shared_mem_file, O_RDWR | O_CREAT | O_SYNC, 0666);
        if (fd < 0) {
            perror("shared memory file open");
            exit(1);
        }
        if (ftruncate(fd, s->shared_size) < 0) {
            perror("shared memory file expand to page size");
            exit(1);
        }
        // copy backup
        s->saved_shared = mmap(NULL, s->shared_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memcpy(s->saved_shared, s->guest_mem + s->shared_addr, s->shared_size);
        munmap(s->guest_mem + s->shared_addr, s->shared_size);
        mmap(s->guest_mem + s->shared_addr, s->shared_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);

        close(fd);
    } else {
        fprintf(stderr, "shared memory address (0x%zx) is out of bounds\n", s->shared_addr);
    }
}

// restore memory from file
static void snapshot_mem_restore(struct SnapshotState *s) {
    int fd = -1;

    // remove the shared mem backup
    munmap(s->saved_shared, s->shared_size);
    s->saved_shared = NULL;
    munmap(s->guest_mem + s->shared_addr, s->shared_size);

    // remove the entire old memory
    munmap(s->guest_mem, s->guest_size);

    // map as MAP_PRIVATE to avoid carrying writes back to the saved file
    fd = open(filepath, O_RDONLY);
    mmap(s->guest_mem, s->guest_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);

    snapshot_mem_init_shared(s);
}

static void save_snapshot(struct SnapshotState *s) {
    if (s->is_saved) {
        return;
    }
    s->is_saved = true;

    // save memory state to file
    int fd = -1;

    fd = open(filepath, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (ftruncate(fd, s->guest_size)) {
        fprintf(stderr, "Failed to expand file to %zu bytes\n", s->guest_size);
        close(fd);
        s->is_saved = false;
        return;
    }

    char *map = mmap(0, s->guest_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(map, s->guest_mem, s->guest_size);
    msync(map, s->guest_size, MS_SYNC);
    munmap(map, s->guest_size);
    close(fd);

    snapshot_mem_restore(s);

    // save cpu and device state
    s->ioc = qemu_snapshot_save_cpu_state();
}

static void restore_snapshot(struct SnapshotState *s) {
    if (!s->is_saved) {
        fprintf(stderr, "[QEMU] ERROR: attempting to restore but state has not been saved!\n");
        return;
    }

    snapshot_mem_restore(s);

    // restore cpu and device state
    qemu_snapshot_load_cpu_state(s->ioc);
}

static uint64_t snapshot_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void snapshot_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    SnapshotState *snapshot = opaque;

    snapshot->guest_mem = current_machine->ram->ram_block->host;
    snapshot->guest_size = current_machine->ram->ram_block->max_length;

    /* fprintf(stderr, "[QEMU] snapshot_mmio_write: addr=0x%lx, val=0x%lx, size=%u\n", addr, val, size); */

    switch (addr) {
    case 0x00:
        switch (val) {
            case 0x202:
                // release shared memory
                snapshot_mem_restore_shared(snapshot);
                break;
            case 0x101:
                save_snapshot(snapshot);
                break;
            case 0x102:
                restore_snapshot(snapshot);
                break;
        }
        break;
    case 0x10:
        // release previous shared memory first
        snapshot_mem_restore_shared(snapshot);

        // link address to shared memory
        snapshot->shared_addr = val;
        snapshot->shared_size = 0x1000;
        snapshot_mem_init_shared(snapshot);
        break;
    }
}

static const MemoryRegionOps snapshot_mmio_ops = {
    .read = snapshot_mmio_read,
    .write = snapshot_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};

static void pci_snapshot_realize(PCIDevice *pdev, Error **errp)
{
    SnapshotState *snapshot = SNAPSHOT(pdev);
    snapshot->is_saved = false;
    snapshot->ioc = NULL;
    snapshot->shared_addr = -1;
    snapshot->shared_size = 0;
    snapshot->saved_shared = NULL;
    snapshot->guest_mem = NULL;
    snapshot->guest_size = 0;

    memory_region_init_io(&snapshot->mmio, OBJECT(snapshot), &snapshot_mmio_ops, snapshot,
                    "snapshot-mmio", 1 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &snapshot->mmio);
}

static void snapshot_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_snapshot_realize;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0xf987;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static void pci_snapshot_register_types(void)
{
    static InterfaceInfo interfaces[] = {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    };
    static const TypeInfo snapshot_info = {
        .name          = TYPE_PCI_SNAPSHOT_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SnapshotState),
        .class_init    = snapshot_class_init,
        .interfaces = interfaces,
    };

    type_register_static(&snapshot_info);
}
type_init(pci_snapshot_register_types)
