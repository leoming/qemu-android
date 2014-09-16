/* Copyright (C) 2007-2013 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include "framebuffer.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "trace.h"

#define BITS 8
#include "goldfish_fb_template.h"
#define BITS 15
#include "goldfish_fb_template.h"
#define BITS 16
#include "goldfish_fb_template.h"
#define BITS 24
#include "goldfish_fb_template.h"
#define BITS 32
#include "goldfish_fb_template.h"

#define TYPE_GOLDFISH_FB "goldfish_fb"
#define GOLDFISH_FB(obj) OBJECT_CHECK(struct goldfish_fb_state, (obj), TYPE_GOLDFISH_FB)

enum {
    FB_GET_WIDTH        = 0x00,
    FB_GET_HEIGHT       = 0x04,
    FB_INT_STATUS       = 0x08,
    FB_INT_ENABLE       = 0x0c,
    FB_SET_BASE         = 0x10,
    FB_SET_ROTATION     = 0x14,
    FB_SET_BLANK        = 0x18,
    FB_GET_PHYS_WIDTH   = 0x1c,
    FB_GET_PHYS_HEIGHT  = 0x20,

    FB_INT_VSYNC             = 1U << 0,
    FB_INT_BASE_UPDATE_DONE  = 1U << 1
};

struct goldfish_fb_state {
    SysBusDevice parent;

    QemuConsole *con;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t fb_base;
    uint32_t base_valid : 1;
    uint32_t need_update : 1;
    uint32_t need_int : 1;
    uint32_t set_rotation : 2;
    uint32_t blank : 1;
    uint32_t int_status;
    uint32_t int_enable;
    int      rotation;   /* 0, 1, 2 or 3 */
    int      dpi;
};

#define  GOLDFISH_FB_SAVE_VERSION  2

static void goldfish_fb_save(QEMUFile*  f, void*  opaque)
{
    struct goldfish_fb_state*  s = opaque;

    DisplaySurface *ds = qemu_console_surface(s->con);

    qemu_put_be32(f, surface_width(ds));
    qemu_put_be32(f, surface_height(ds));
    qemu_put_be32(f, surface_stride(ds));
    qemu_put_byte(f, 0);

    qemu_put_be32(f, s->fb_base);
    qemu_put_byte(f, s->base_valid);
    qemu_put_byte(f, s->need_update);
    qemu_put_byte(f, s->need_int);
    qemu_put_byte(f, s->set_rotation);
    qemu_put_byte(f, s->blank);
    qemu_put_be32(f, s->int_status);
    qemu_put_be32(f, s->int_enable);
    qemu_put_be32(f, s->rotation);
    qemu_put_be32(f, s->dpi);
}

static int  goldfish_fb_load(QEMUFile*  f, void*  opaque, int  version_id)
{
    struct goldfish_fb_state*  s   = opaque;
    int                        ret = -1;
    int                        ds_w, ds_h, ds_pitch, ds_rot;

    if (version_id != GOLDFISH_FB_SAVE_VERSION)
        goto Exit;

    ds_w     = qemu_get_be32(f);
    ds_h     = qemu_get_be32(f);
    ds_pitch = qemu_get_be32(f);
    ds_rot   = qemu_get_byte(f);

    DisplaySurface *ds = qemu_console_surface(s->con);

    if (surface_width(ds) != ds_w ||
        surface_height(ds) != ds_h ||
        surface_stride(ds) != ds_pitch ||
        ds_rot != 0)
    {
        /* XXX: We should be able to force a resize/rotation from here ? */
        fprintf(stderr, "%s: framebuffer dimensions mismatch\n", __FUNCTION__);
        goto Exit;
    }

    s->fb_base      = qemu_get_be32(f);
    s->base_valid   = qemu_get_byte(f);
    s->need_update  = qemu_get_byte(f);
    s->need_int     = qemu_get_byte(f);
    s->set_rotation = qemu_get_byte(f);
    s->blank        = qemu_get_byte(f);
    s->int_status   = qemu_get_be32(f);
    s->int_enable   = qemu_get_be32(f);
    s->rotation     = qemu_get_be32(f);
    s->dpi          = qemu_get_be32(f);

    /* force a refresh */
    s->need_update = 1;

    ret = 0;
Exit:
    return ret;
}

static int
pixels_to_mm(int  pixels, int dpi)
{
    /* dpi = dots / inch
    ** inch = dots / dpi
    ** mm / 25.4 = dots / dpi
    ** mm = (dots * 25.4)/dpi
    */
    return (int)(0.5 + 25.4 * pixels  / dpi);
}


#define  STATS  0

#if STATS
static int   stats_counter;
static long  stats_total;
static int   stats_full_updates;
static long  stats_total_full_updates;
#endif

static void goldfish_fb_update_display(void *opaque)
{
    struct goldfish_fb_state *s = (struct goldfish_fb_state *)opaque;
    DisplaySurface *ds = qemu_console_surface(s->con);
    int full_update = 0;
    int  width, height, pitch;

    if (!s || !s->con || surface_bits_per_pixel(ds) == 0 || !s->fb_base)
        return;

    if((s->int_enable & FB_INT_VSYNC) && !(s->int_status & FB_INT_VSYNC)) {
        s->int_status |= FB_INT_VSYNC;
        qemu_irq_raise(s->irq);
    }

    if(s->need_update) {
        full_update = 1;
        if(s->need_int) {
            s->int_status |= FB_INT_BASE_UPDATE_DONE;
            if(s->int_enable & FB_INT_BASE_UPDATE_DONE)
                qemu_irq_raise(s->irq);
        }
        s->need_int = 0;
        s->need_update = 0;
    }

    pitch     = surface_stride(ds);
    width     = surface_width(ds);
    height    = surface_height(ds);

    int ymin, ymax;

#if STATS
    if (full_update)
        stats_full_updates += 1;
    if (++stats_counter == 120) {
        stats_total               += stats_counter;
        stats_total_full_updates  += stats_full_updates;

        trace_goldfish_fb_update_stats(stats_full_updates*100.0/stats_counter,
                stats_total_full_updates*100.0/stats_total );

        stats_counter      = 0;
        stats_full_updates = 0;
    }
#endif /* STATS */

    if (s->blank)
    {
        void *dst_line = surface_data(ds);
        memset( dst_line, 0, height*pitch );
        ymin = 0;
        ymax = height-1;
    }
    else
    {
        SysBusDevice *dev = SYS_BUS_DEVICE(opaque);
        MemoryRegion *address_space = sysbus_address_space(dev);
        int src_width = width * 2;
        int dest_col_pitch = surface_bytes_per_pixel(ds);
        int dest_row_pitch = surface_stride(ds);
        drawfn fn;

        switch (surface_bits_per_pixel(ds)) {
        case 0:
            return;
        case 8:
            fn = draw_line_8;
            break;
        case 15:
            fn = draw_line_15;
            break;
        case 16:
            fn = draw_line_16;
            break;
        case 24:
            fn = draw_line_24;
            break;
        case 32:
            fn = draw_line_32;
            break;
        default:
            hw_error("goldfish_fb: bad color depth\n");
            return;
        }

        ymin = 0;
        framebuffer_update_display(ds, address_space, s->fb_base, width, height,
                src_width, dest_row_pitch, dest_col_pitch, full_update,
                fn, ds, &ymin, &ymax);
    }

    ymax += 1;
    if (ymin >= 0) {
        trace_goldfish_fb_update_display(ymin, ymax-ymin, 0, width);
        dpy_gfx_update(s->con, 0, ymin, width, ymax-ymin);
    }
}

static void goldfish_fb_invalidate_display(void * opaque)
{
    // is this called?
    struct goldfish_fb_state *s = (struct goldfish_fb_state *)opaque;
    s->need_update = 1;
}

static uint64_t goldfish_fb_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t ret = 0;
    struct goldfish_fb_state *s = opaque;
    DisplaySurface *ds = qemu_console_surface(s->con);

    switch(offset) {
        case FB_GET_WIDTH:
            ret = surface_width(ds);
            break;

        case FB_GET_HEIGHT:
            ret = surface_height(ds);
            break;

        case FB_INT_STATUS:
            ret = s->int_status & s->int_enable;
            if(ret) {
                s->int_status &= ~ret;
                qemu_irq_lower(s->irq);
            }
            break;

        case FB_GET_PHYS_WIDTH:
            ret = pixels_to_mm( surface_width(ds), s->dpi );
            break;

        case FB_GET_PHYS_HEIGHT:
            ret = pixels_to_mm( surface_height(ds), s->dpi );
            break;

        default:
            error_report("goldfish_fb_read: Bad offset 0x" TARGET_FMT_plx,
                    offset);
            break;
    }

    trace_goldfish_fb_memory_read(offset, ret);
    return ret;
}

static void goldfish_fb_write(void *opaque, hwaddr offset, uint64_t val,
        unsigned size)
{
    struct goldfish_fb_state *s = opaque;

    trace_goldfish_fb_memory_write(offset, val);

    switch(offset) {
        case FB_INT_ENABLE:
            s->int_enable = val;
            qemu_set_irq(s->irq, s->int_status & s->int_enable);
            break;
        case FB_SET_BASE:
            s->fb_base = val;
            s->int_status &= ~FB_INT_BASE_UPDATE_DONE;
            s->need_update = 1;
            s->need_int = 1;
            s->base_valid = 1;
            if(s->set_rotation != s->rotation) {
                //printf("FB_SET_BASE: rotation : %d => %d\n", s->rotation, s->set_rotation);
                s->rotation = s->set_rotation;
            }
            /* The guest is waiting for us to complete an update cycle
             * and notify it, so make sure we do a redraw immediately.
             */
            graphic_hw_update(s->con);
            qemu_set_irq(s->irq, s->int_status & s->int_enable);
            break;
        case FB_SET_ROTATION:
            s->set_rotation = val;
            break;
        case FB_SET_BLANK:
            s->blank = val;
            s->need_update = 1;
            break;
        default:
            error_report("goldfish_fb_write: Bad offset 0x" TARGET_FMT_plx,
                    offset);
    }
}

static const MemoryRegionOps goldfish_fb_iomem_ops = {
    .read = goldfish_fb_read,
    .write = goldfish_fb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const GraphicHwOps goldfish_fb_ops = {
    .invalidate = goldfish_fb_invalidate_display,
    .gfx_update = goldfish_fb_update_display,
};

static int goldfish_fb_init(SysBusDevice *sbdev)
{
    DeviceState *dev = DEVICE(sbdev);
    struct goldfish_fb_state *s = GOLDFISH_FB(dev);

    sysbus_init_irq(sbdev, &s->irq);

    s->con = graphic_console_init(dev, 0, &goldfish_fb_ops, s);

    s->dpi = 165;  /* XXX: Find better way to get actual value ! */

    memory_region_init_io(&s->iomem, OBJECT(s), &goldfish_fb_iomem_ops, s,
            "goldfish_fb", 0x100);
    sysbus_init_mmio(sbdev, &s->iomem);

    register_savevm(dev, "goldfish_fb", 0, GOLDFISH_FB_SAVE_VERSION,
                     goldfish_fb_save, goldfish_fb_load, s);

    return 0;
}

static void goldfish_fb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = goldfish_fb_init;
    dc->desc = "goldfish framebuffer";
}

static const TypeInfo goldfish_fb_info = {
    .name          = TYPE_GOLDFISH_FB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(struct goldfish_fb_state),
    .class_init    = goldfish_fb_class_init,
};

static void goldfish_fb_register(void)
{
    type_register_static(&goldfish_fb_info);
}

type_init(goldfish_fb_register);