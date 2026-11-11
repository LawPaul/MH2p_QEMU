#include "qemu/osdep.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/tegra30.h"
#include "hw/core/loader.h"
#include "system/reset.h"
#include "hw/misc/mmx_ioc.h"
#include "hw/misc/glbridge.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "hw/block/flash.h"
#include "system/blockdev.h"
#include "system/block-backend.h"

typedef struct MHI2MachineState {
    MachineState parent_obj;
    char *iram_filename;
    char *pmic_model;
    Tegra30State *soc;
} MHI2MachineState;

static char *mhi2_get_pmic(Object *obj, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;

    return g_strdup(s->pmic_model ? s->pmic_model : "legacy");
}

static void mhi2_set_pmic(Object *obj, const char *value, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;

    if (strcmp(value, "legacy") && strcmp(value, "tps65911") &&
        strcmp(value, "max20024")) {
        error_setg(errp, "pmic must be legacy, tps65911 or max20024");
        return;
    }
    g_free(s->pmic_model);
    s->pmic_model = g_strdup(value);
}

static char *mhi2_get_iram(Object *obj, Error **errp)
{
    return g_strdup(((MHI2MachineState *)obj)->iram_filename);
}

static void mhi2_set_iram(Object *obj, const char *value, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;
    g_free(s->iram_filename);
    s->iram_filename = g_strdup(value);
}

static void mhi2_reset(void *opaque)
{
    MHI2MachineState *s = opaque;
    CPUState *boot_cpu = CPU(&s->soc->avp);

    for (int i = 0; i < TEGRA30_NUM_CPUS; i++) {
        cpu_reset(CPU(&s->soc->cpus[i]));
    }
    cpu_reset(CPU(&s->soc->lp_cpu));
    cpu_reset(boot_cpu);
    /* IOC normal-power indication: Quickboot samples GPIO CC5 for lp=. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->soc->gpio),
                                 TEGRA30_GPIO_NR(28, 5)), 1);
    /* devg-nvgpio maps the active-low USB overcurrent input to C6.
     * A powered virtual connector with no electrical fault leaves it high. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->soc->gpio),
                                 TEGRA30_GPIO_NR(2, 6)), 1);
    cpu_set_pc(boot_cpu, 0x480e0000);
}

static void mhi2_harman_init(MachineState *machine)
{
    Tegra30State *soc;
    MHI2MachineState *ms = (MHI2MachineState *)machine;

    if (!machine->firmware) {
        error_report("MHI2 requires -bios <64 MiB NOR image>");
        exit(1);
    }
    info_report("Initializing MHI2 Harman");

    soc = TEGRA30(object_new(TYPE_TEGRA30));
    ms->soc = soc;
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    
    // TODO: SoC setup

    // TODO: Move emem init to tegra30.c
    memory_region_init_ram(&soc->emem, OBJECT(soc), "emem", machine->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x80000000, &soc->emem);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    glbridge_create(OBJECT(machine));


    /*
     * IOC companion controller (the V850 MCU).  It signals data-ready on GPIO
     * PDD0 (the OS's "mmx_ioc_irq", port DD pin 0 -- the only bank-7 pin the OS
     * configures for edge detection; it polls the GPIO INT_STA rather than
     * taking a CPU interrupt) and delivers frames to the I2C2 slave interface
     * (soc->i2c[1], the bus the OOC's IocI2c resmgr uses).
     */
    DeviceState *ioc = qdev_new(TYPE_MMX_IOC);
    /*
     * The IOC link uses two Tegra I2C controllers: QNX masters configuration
     * to the IOC (a slave at 0x4c) on i2c[4] (DVC), and the IOC masters data
     * frames back into the Tegra slave on i2c[1] (I2C2) -- which QNX's "I2C
     * slave thread" polls.  This link is the data (IOC->Tegra) channel.
     */
    object_property_set_link(OBJECT(ioc), "i2c", OBJECT(&soc->i2c[1]),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ioc), &error_fatal);
    /* IOC data-ready output -> GPIO PDD0 input (mmx_ioc_irq). */
    qdev_connect_gpio_out(ioc, 0,
                          qdev_get_gpio_in(DEVICE(&soc->gpio),
                                           TEGRA30_GPIO_NR(29, 0)));
    /* MMX request output GPIO PDD3 -> IOC request input. */
    qdev_connect_gpio_out(DEVICE(&soc->gpio), TEGRA30_GPIO_NR(29, 3),
                          qdev_get_gpio_in(ioc, 0));

    /*
     * PMIC / board-ID device on the DVC I2C bus (slave 0x2d).  The bootloader
     * reads the 6-byte board ID from registers 0x6a.. and matches the first
     * byte against a board-variant table; 0x20 selects a known board.
     */
    I2CSlave *pmic;
    if (!ms->pmic_model || !strcmp(ms->pmic_model, "legacy")) {
        pmic = i2c_slave_new("tegra30-i2c-dbg", 0x2d);
        qdev_prop_set_uint8(DEVICE(pmic), "fill", 0x20);
        i2c_slave_realize_and_unref(pmic, soc->i2c[4].bus, &error_fatal);
    } else {
        bool maxim = !strcmp(ms->pmic_model, "max20024");

        pmic = i2c_slave_new("mhi2-pmic", maxim ? 0x3c : 0x2d);
        qdev_prop_set_bit(DEVICE(pmic), "maxim", maxim);
        i2c_slave_realize_and_unref(pmic, soc->i2c[4].bus, &error_fatal);
        /* Emulator fixture wiring; production PMIC IRQ routing is unproven. */
        qdev_connect_gpio_out(DEVICE(pmic), 0,
                              qdev_get_gpio_in(DEVICE(&soc->a9mpcore), 86));
    }

    /*
     * IOC control interface: QNX's IocI2c resmgr masters to the IOC as an I2C
     * slave at 0x4c on the DVC bus (i2c[4]) to push configuration register
     * writes.  Modelled here as a register-file stub so those writes ACK (a
     * NACK makes the resmgr never start its slave thread); the IOC's data
     * channel back to the Tegra is the i2c[1] slave handled by mmx_ioc.
     */
    I2CSlave *ioc_cfg = i2c_slave_new("tegra30-i2c-dbg", 0x4c);
    qdev_prop_set_uint8(DEVICE(ioc_cfg), "fill", 0x00);
    /* devg-nvtmon accesses the local/remote diode sensor at DVC 0x4c. */
    qdev_prop_set_bit(DEVICE(ioc_cfg), "temperature-sensor", true);
    i2c_slave_realize_and_unref(ioc_cfg, soc->i2c[4].bus, &error_fatal);

    /*
     * eMMC on SDMMC4 (QNX mounts the app filesystem from it). Backed
     * by the IF_SD drive if the user supplies one, e.g.
     * -drive if=sd,format=raw,file=emmc.img
     */
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    DeviceState *emmc = qdev_new(TYPE_EMMC);
    qdev_prop_set_uint32(emmc, "ocr-power-delay-ns", 100000000);
    if (blk) {
        qdev_prop_set_drive(emmc, "drive", blk);
    }
    qdev_realize_and_unref(emmc, qdev_get_child_bus(soc->sdmmc[3], "sd-bus"),
                           &error_fatal);

    /* Removable readers: stock QNX identifies SDMMC1/3 with CD D3/D4 and
     * WP V2/V3. IF_SD index 0 remains the eMMC for existing launchers. */
    for (int slot = 0; slot < 2; slot++) {
        DeviceState *host = soc->sdmmc[slot ? 2 : 0];
        DriveInfo *card_di = drive_get(IF_SD, slot + 1, 0);

        qdev_connect_gpio_out_named(host, "card-inserted", 0,
            qemu_irq_invert(qdev_get_gpio_in(DEVICE(&soc->gpio),
                                            TEGRA30_GPIO_NR(3, 3 + slot))));
        qdev_connect_gpio_out_named(host, "card-readonly", 0,
            qdev_get_gpio_in(DEVICE(&soc->gpio), TEGRA30_GPIO_NR(21, 2 + slot)));
        if (card_di && blk_bs(blk_by_legacy_dinfo(card_di))) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive(card, "drive", blk_by_legacy_dinfo(card_di));
            qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                                  &error_fatal);
        }
    }

    uint8_t *nor;
    size_t nor_size;
    if (!g_file_get_contents(machine->firmware, (gchar **) &nor, (gsize *) &nor_size, NULL)) {
        error_report("Failed to load nor");
        exit(1);
    }

    if (nor_size != 64 * MiB) {
        error_report("MHI2 NOR image must be exactly 64 MiB");
        exit(1);
    }

    /* QuickBoot is shadowed to and executed from DRAM at 0x83F28000. */
    rom_add_blob_fixed("mhi2-quickboot-shadow", nor, nor_size, 0x83F28000);
    g_free(nor);

    /*
     * NOR boot flash window at 0x48000000.  The firmware probes the NOR as an
     * AMD/Spansion CFI flash (CFI query at word 0x55 returning "QRY", JEDEC
     * autoselect with unlock cycles at words 0x555/0x2aa) and then DMAs it
     * through the SNOR controller, so model it as a real CFI flash rather than
     * a plain RAM window.  64 MiB, x16, 512 x 128 KiB sectors.
     */
    /* Writable per-run overlay: QNX persists files without modifying -bios. */
    BlockBackend *nor_blk = blk_new_open(
        machine->firmware,
        NULL, NULL, BDRV_O_RDWR | BDRV_O_SNAPSHOT, &error_fatal);
    DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);
    qdev_prop_set_drive(flash, "drive", nor_blk);
    qdev_prop_set_uint32(flash, "num-blocks", 512);
    qdev_prop_set_uint32(flash, "sector-length", 0x20000);
    qdev_prop_set_uint8(flash, "width", 2);
    qdev_prop_set_uint16(flash, "unlock-addr0", 0x555);
    qdev_prop_set_uint16(flash, "unlock-addr1", 0x2aa);
    qdev_prop_set_uint16(flash, "id0", 0x0001);
    qdev_prop_set_string(flash, "name", "tegra3-nor");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, 0x48000000);

    /*
     * Tegra internal RAM (IRAM, 256 KiB at 0x40000000).  On real hardware the
     * boot ROM leaves a Boot Information Table here; the QuickBoot loader
     * validates it before reading the next stage.  Seed it from the captured
     * tegra3_iram.bin dump.
     */
    memory_region_init_ram(&soc->iram, OBJECT(soc), "iram", 0x40000,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x40000000, &soc->iram);

    if (ms->iram_filename) {
        if (load_image_targphys(ms->iram_filename, 0x40000000, 0x40000, &error_fatal) < 0) {
            error_report("Cannot load IRAM image %s", ms->iram_filename);
            exit(1);
        }
    } else {
        error_report("Quickboot needs -machine iram=<captured IRAM/BIT image>");
        exit(1);
    }

    qemu_register_reset(mhi2_reset, ms);

}

static void mhi2_harman_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a9"),
        NULL
    };

    object_class_property_add_str(OBJECT_CLASS(mc), "iram",
                                  mhi2_get_iram, mhi2_set_iram);
    object_class_property_add_str(OBJECT_CLASS(mc), "pmic",
                                  mhi2_get_pmic, mhi2_set_pmic);
    mc->desc = "MHI2 Harman";
    mc->init = mhi2_harman_init;
    mc->block_default_type = IF_SD;
    mc->units_per_default_bus = 1;
    mc->min_cpus = TEGRA30_NUM_PROCS;
    mc->max_cpus = TEGRA30_NUM_PROCS;
    mc->default_cpus = TEGRA30_NUM_PROCS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "mhi2-harman.ram";
}

DEFINE_MACHINE_EXTENDED("mhi2-harman", MACHINE, MHI2MachineState,
                        mhi2_harman_machine_init, false, false, NULL)
