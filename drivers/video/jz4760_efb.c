/*
 * Enhanced frame buffer driver for RS97 handheld.
 * Mose code are based on JZ4770 driver and this driver will create /dev/fb1 with IPU supported for user app.
 * Basically, for compatiable, keep /dev/fb0 node will be good idea instaed of replacing it.
 *
 * Copyright (C) 2012, Maarten ter Huurne <maarten@treewalker.org>
 * Copyright (C) 2014, Paul Cercueil <paul@crapouillou.net>
 * Copyright (C) 2017, Steward Fu <steward.fu@gmail.com>
 *
 * Based on the JZ4760 frame buffer driver:
 * Copyright (C) 2005-2008, Ingenic Semiconductor Inc.
 * Author: Wolfgang Wang, <lgwang@ingenic.cn>
 *
 * Includes code fragments from JZ4740 SoC LCD frame buffer driver:
 * Copyright (C) 2009-2010, Lars-Peter Clausen <lars@metafoo.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gcd.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/wait.h>

#include <asm/addrspace.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/processor.h>
#include <asm/jzsoc.h>
#include <asm/mach-jz4760/jz4760lcdc.h>

#define MAX_XRES 640
#define MAX_YRES 480
#define EFB_FBIO_WAITFORVSYNC  _IOWR(0, 0, int)
//#define DEVICE_ATTR(_name, _mode, _show, _stroe) \
//struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)

static struct resource jz_lcd_resources[] = { 
  [0] = { 
    .start          = LCD_BASE,
    .end            = LCD_BASE + 0x13F,
    .flags          = IORESOURCE_MEM,
  },  
  [1] = { 
    .start          = IPU_BASE,
    .end            = IPU_BASE + 0x9B,
    .flags          = IORESOURCE_MEM,
  }, 
};

struct jz4760lcd_panel_t {
  unsigned int cfg;  /* panel mode and pin usage etc. */
  unsigned int w;    /* Panel Width(in pixel) */
  unsigned int h;    /* Panel Height(in line) */
  unsigned int fclk;  /* frame clk */
  unsigned int hsw;  /* hsync width, in pclk */
  unsigned int vsw;  /* vsync width, in line count */
  unsigned int elw;  /* end of line, in pclk */
  unsigned int blw;  /* begin of line, in pclk */
  unsigned int efw;  /* end of frame, in line count */
  unsigned int bfw;  /* begin of frame, in line count */
};

static const struct jz4760lcd_panel_t jz4760_lcd_panel = {
  .cfg = LCD_CFG_LCDPIN_LCD | LCD_CFG_RECOVER | /* Underrun recover */
         LCD_CFG_MODE_GENERIC_TFT | /* General TFT panel */
         LCD_CFG_MODE_TFT_16BIT |   /* output 16bpp */
         LCD_CFG_PCP |  /* Pixel clock polarity: falling edge */
         LCD_CFG_HSP |   /* Hsync polarity: active low */
         LCD_CFG_VSP,  /* Vsync polarity: leading edge is falling edge */
  /* w, h, fclk, hsw, vsw, elw, blw, efw, bfw */
  320, 240, 60, 50, 1, 10, 70, 5, 5,
  /* Note: 432000000 / 72 = 60 * 400 * 250, so we get exactly 60 Hz. */
};

/* default output to lcd panel */
static const struct jz4760lcd_panel_t *jz_panel = &jz4760_lcd_panel;

struct jzfb {
  struct fb_info *fb;
  struct platform_device *pdev;
  void *panel;

  uint32_t pseudo_palette[16];
  unsigned int bpp;  /* Current 'bits per pixel' value (32 or 16) */

  uint32_t pan_offset;
  uint32_t vsync_count;
  wait_queue_head_t wait_vsync;

  struct clk *lpclk, *ipuclk;

  struct mutex lock;
  bool is_enabled;
  /*
   * Number of frames to wait until doing a forced foreground flush.
   * If it looks like we are double buffering, we can flush on vertical
   * panning instead.
   */
  unsigned int delay_flush;

  bool clear_fb;

  //void __iomem *base;
  //void __iomem *ipu_base;
};

static void *lcd_frame_fb;
static bool keep_aspect_ratio = true;
static bool allow_downscaling = false;

int strtobool(const char *s, bool *res)
{
  switch (s[0]) {
  case 'y':
  case 'Y':
  case '1':
    *res = true;
    break;
  case 'n':
  case 'N':
  case '0':
    *res = false;
    break;
  default:
    return -EINVAL;
  }
  return 0;
}

static void ctrl_enable(struct jzfb *jzfb)
{
  REG_LCD_CTRL = (REG_LCD_CTRL & ~LCD_CTRL_DIS) | LCD_CTRL_ENA;
}

static void ctrl_disable(struct jzfb *jzfb)
{
  unsigned int cnt;

  // Use regular disable: finishes current frame, then stops.
  REG_LCD_CTRL|= LCD_CTRL_DIS;

  // Wait 20 ms for frame to end (at 60 Hz, one frame is 17 ms).
  for(cnt=20; cnt; cnt-= 4){
    if(REG_LCD_STATE & LCD_STATE_LDD){
      break;
    }
    msleep(4);
  }

  if(!cnt){
    dev_err(&jzfb->pdev->dev, "LCD disable timeout!\n");
  }
  REG_LCD_STATE&= ~LCD_STATE_LDD;
}

static int jz4760fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue, u_int transp, struct fb_info *fb)
{
  struct jzfb *jzfb = fb->par;

  if(regno >= ARRAY_SIZE(jzfb->pseudo_palette)){
    return 1;
  }

  if(fb->var.bits_per_pixel == 16){
    ((u32 *)fb->pseudo_palette)[regno] = (red & 0xf800) | ((green & 0xfc00) >> 5) | (blue >> 11);
  }
  else{
    ((u32 *)fb->pseudo_palette)[regno] = ((red & 0xff00) << 8) | (green & 0xff00) | (blue >> 8);
  }
  return 0;
}

/* Use mmap /dev/fb can only get a non-cacheable Virtual Address. */
static int jz4760fb_mmap(struct fb_info *fb, struct vm_area_struct *vma)
{
  unsigned long start;
  unsigned long off;
  u32 len;

  printk("%s\n", __func__);
  off = vma->vm_pgoff << PAGE_SHIFT;
  //fb->fb_get_fix(&fix, PROC_CONSOLE(info), info);

  // frame buffer memory
  start = fb->fix.smem_start;
  len = PAGE_ALIGN((start & ~PAGE_MASK) + fb->fix.smem_len);
  start&= PAGE_MASK;

  if((vma->vm_end - vma->vm_start + off) > len){
    return -EINVAL;
  }
  off+= start;

  vma->vm_pgoff = off >> PAGE_SHIFT;
  vma->vm_flags|= VM_IO;

  /* Set cacheability to cacheable, write through, no write-allocate. */
  vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
  pgprot_val(vma->vm_page_prot)&= ~_CACHE_MASK;
  pgprot_val(vma->vm_page_prot)|= _CACHE_CACHABLE_NONCOHERENT;
  if(io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT, vma->vm_end - vma->vm_start, vma->vm_page_prot)){
    return -EAGAIN;
  }
  return 0;
}

static int reduce_fraction(unsigned int *num, unsigned int *denom)
{
  unsigned long d = gcd(*num, *denom);

  if(*num > 32 * d){
    return -EINVAL;
  }

  *num/= d;
  *denom/= d;
  return 0;
}

static int jz4760fb_check_var(struct fb_var_screeninfo *var, struct fb_info *fb)
{
  struct jzfb *jzfb = fb->par;
  unsigned int num, denom;

  // The minimum input size for the IPU to work is 4x4
  if(var->xres < 4){
    var->xres = 4;
  }
  if(var->yres < 4){
    var->yres = 4;
  }

  if(!allow_downscaling){
    if(var->xres > jz_panel->w){
      var->xres = jz_panel->w;
    }
    if(var->yres > jz_panel->h){
      var->yres = jz_panel->h;
    }
  }

  // Adjust the input size until we find a valid configuration
  for(num=jz_panel->w, denom=var->xres; var->xres<=MAX_XRES && reduce_fraction(&num, &denom)<0; denom++, var->xres++);
  if(var->xres > MAX_XRES){
    return -EINVAL;
  }

  for(num=jz_panel->h, denom=var->yres; var->yres<=MAX_YRES && reduce_fraction(&num, &denom)<0; denom++, var->yres++);
  if(var->yres > MAX_YRES){
    return -EINVAL;
  }

  // Reserve space for triple buffering.
  var->yres_virtual = var->yres * 3;
  var->xres_virtual = var->xres;
  var->vmode = FB_VMODE_NONINTERLACED;
  var->yoffset = 0;

  if(var->bits_per_pixel != 32 && var->bits_per_pixel != 16){
    var->bits_per_pixel = 32;
  }

  if(var->bits_per_pixel == 16){
    var->transp.length = 0;
    var->blue.length = var->red.length = 5;
    var->green.length = 6;
    var->transp.offset = 0;
    var->red.offset = 11;
    var->green.offset = 5;
    var->blue.offset = 0;
  } 
  else{
    var->transp.offset = 24;
    var->red.offset = 16;
    var->green.offset = 8;
    var->blue.offset = 0;
    var->transp.length = var->red.length = var->green.length = var->blue.length = 8;
  }

  jzfb->clear_fb = var->bits_per_pixel != fb->var.bits_per_pixel || var->xres != fb->var.xres || var->yres != fb->var.yres;
  return 0;
}

static int jzfb_wait_for_vsync(struct jzfb *jzfb)
{
  uint32_t count = jzfb->vsync_count;
  long t = wait_event_interruptible_timeout(jzfb->wait_vsync, count != jzfb->vsync_count, HZ / 10);
  return t > 0 ? 0 : (t < 0 ? (int)t : -ETIMEDOUT);
}

static void jzfb_update_frame_address(struct jzfb *jzfb)
{
  REG_IPU_Y_ADDR = (u32)virt_to_phys(lcd_frame_fb + jzfb->pan_offset);
}

static void jzfb_lcdc_enable(struct jzfb *jzfb)
{
  //clk_enable(jzfb->lpclk);
  jzfb_update_frame_address(jzfb);

  jzfb->delay_flush = 0;
  REG_LCD_STATE = 0; // Clear LCDC status

  // Enabling the LCDC too soon after the clock will hang the system. 
  // A very short delay seems to be sufficient.
  udelay(1);

  ctrl_enable(jzfb);
}

static void jzfb_foreground_resize(struct jzfb *jzfb, unsigned int xpos, unsigned int ypos, unsigned int width, unsigned int height)
{
  /*
   * NOTE:
   * Foreground change sequence:
   *   1. Change Position Registers -> LCD_OSDCTL.Change;
   *   2. LCD_OSDCTRL.Change -> descripter->Size
   * Foreground, only one of the following can be change at one time:
   *   1. F0 size;
   *  2. F0 position
   *   3. F1 size
   *  4. F1 position
   */
  REG_LCD_XYP1 = (ypos << 16) | xpos;
  REG_LCD_SIZE1 = (height << 16) | width;
}

static void jzfb_ipu_enable(struct jzfb *jzfb)
{
  //clk_enable(jzfb->ipuclk);

  // Clear the status register and enable the chip
  REG_IPU_STATUS = 0;
  REG_IPU_CTRL|= IPU_CTRL_CHIP_EN | IPU_CTRL_RUN;
}

static void jzfb_ipu_disable(struct jzfb *jzfb)
{
  unsigned int timeout = 1000;

  if(REG_IPU_CTRL & IPU_CTRL_CHIP_EN){
    REG_IPU_CTRL = REG_IPU_CTRL | IPU_CTRL_STOP;
    do{
      if(REG_IPU_STATUS & IPU_STATUS_OUT_END){
        break;
      }
      msleep(1);
    }while(--timeout);

    if(!timeout){
      dev_err(&jzfb->pdev->dev, "Timeout while disabling IPU\n");
    }
  }
  REG_IPU_CTRL&= ~IPU_CTRL_CHIP_EN;
}

static void set_downscale_bilinear_coefs(struct jzfb *jzfb, unsigned int reg, unsigned int num, unsigned int denom)
{
  unsigned int i, weight_num = denom;

  for(i=0; i<num; i++){
    u32 value;
    unsigned int weight, offset;

    weight_num = num + (weight_num - num) % (num * 2);

    /*
     * Here, "input pixel 1.0" means half of 0 and half of 1;
     * "input pixel 0.5" means all of 0; and
     * "input pixel 1.49" means almost all of 1.
     */
    weight = 512 - 512 * (weight_num - num) / (num * 2);
    weight_num += denom * 2;
    offset = (weight_num - num) / (num * 2);
    value = ((weight & 0x7FF) << 6) | (offset << 1);
    *((volatile unsigned int*)reg) = value;
  }
}

static void set_upscale_bilinear_coefs(struct jzfb *jzfb, unsigned int reg, unsigned int num, unsigned int denom)
{
  unsigned int i, weight_num = 0;

  for(i=0; i<num; i++){
    unsigned int weight = 512 - 512 * weight_num / num;
    u32 offset = 0, value;

    weight_num+= denom;
    if(weight_num >= num){
      weight_num-= num;
      offset = 1;
    }

    value = (weight & 0x7FF) << 6 | (offset << 1);
    *((volatile unsigned int*)reg) = value;
  }
}

static void set_upscale_nearest_neighbour_coefs(struct jzfb *jzfb, unsigned int reg, unsigned int num)
{
  unsigned int i, weight_num = 1;

  for(i=0; i<num; i++, weight_num++){
    u32 value, offset = weight_num / num;
    weight_num%= num;
    value = (512 << 6) | (offset << 1);
    *((volatile unsigned int*)reg) = value;
  }
}

static void set_coefs(struct jzfb *jzfb, unsigned int reg, unsigned int num, unsigned int denom)
{
  // Start programmation of the LUT
  *((volatile unsigned int*)reg) = 1;

  if(denom > num){
    set_downscale_bilinear_coefs(jzfb, reg, num, denom);
  }
  else if(denom == 1){
    set_upscale_nearest_neighbour_coefs(jzfb, reg, num);
  }
  else{
    set_upscale_bilinear_coefs(jzfb, reg, num, denom);
  }
}

static inline bool scaling_required(struct jzfb *jzfb)
{
  struct fb_var_screeninfo *var = &jzfb->fb->var;
  return var->xres != jz_panel->w || var->yres != jz_panel->h;
}

static void jzfb_ipu_configure(struct jzfb *jzfb, const struct jz4760lcd_panel_t *panel)
{
  struct fb_info *fb = jzfb->fb;
  u32 ctrl, coef_index=0, size, format = 2 << IPU_D_FMT_OUT_FMT_BIT;
  unsigned int outputW=panel->w, outputH=panel->h, xpos=0, ypos=0;

  // Enable the chip, reset all the registers
  REG_IPU_CTRL = IPU_CTRL_CHIP_EN | IPU_CTRL_RST;

  switch(jzfb->bpp){
  case 16:
    format|= 3 << IPU_D_FMT_IN_FMT_BIT;
    break;
  case 32:
  default:
    format|= 2 << IPU_D_FMT_IN_FMT_BIT;
    break;
  }
  REG_IPU_D_FMT = format;

  // Set the input height/width/stride
  size = fb->fix.line_length << IPU_IN_GS_W_BIT | fb->var.yres << IPU_IN_GS_H_BIT;
  REG_IPU_IN_GS = size;
  REG_IPU_Y_STRIDE = fb->fix.line_length;

  // Set the input address
  REG_IPU_Y_ADDR = (u32)virt_to_phys(lcd_frame_fb);

  ctrl = IPU_CTRL_CHIP_EN | IPU_CTRL_LCDC_SEL | IPU_CTRL_FM_IRQ_EN;
  if(fb->fix.type == FB_TYPE_PACKED_PIXELS){
    ctrl|= IPU_CTRL_SPKG_SEL;
  }

  if(scaling_required(jzfb)){
    unsigned int numW=panel->w, denomW=fb->var.xres, numH=panel->h, denomH=fb->var.yres;

    BUG_ON(reduce_fraction(&numW, &denomW) < 0);
    BUG_ON(reduce_fraction(&numH, &denomH) < 0);

    if(keep_aspect_ratio){
      unsigned int ratioW = (UINT_MAX >> 6) * numW / denomW, ratioH = (UINT_MAX >> 6) * numH / denomH;
      if(ratioW < ratioH){
        numH = numW;
        denomH = denomW;
      } 
      else{
        numW = numH;
        denomW = denomH;
      }
    }

    if(numW != 1 || denomW != 1){
      set_coefs(jzfb, IPU_HRSZ_COEF_LUT, numW, denomW);
      coef_index |= ((numW - 1) << 16);
      ctrl |= IPU_CTRL_HRSZ_EN;
    }

    if(numH != 1 || denomH != 1){
      set_coefs(jzfb, IPU_VRSZ_COEF_LUT, numH, denomH);
      coef_index |= numH - 1;
      ctrl|= IPU_CTRL_VRSZ_EN;
    }

    outputH = fb->var.yres * numH / denomH;
    outputW = fb->var.xres * numW / denomW;

    // If we are upscaling horizontally, the last columns of pixels
    // shall be hidden, as they usually contain garbage: the last
    // resizing coefficients, when applied to the last column of the
    // input frame, instruct the IPU to blend the pixels with the
    // ones that correspond to the next column, that is to say the
    // leftmost column of pixels of the input frame.
    if(numW > denomW && denomW != 1){
      outputW -= numW / denomW;
    }
  }

  REG_IPU_CTRL = ctrl;

  // Set the LUT index register
  REG_IPU_RSZ_COEF_INDEX = coef_index;

  // Set the output height/width/stride
  size = (outputW * 4) << IPU_OUT_GS_W_BIT | outputH << IPU_OUT_GS_H_BIT;
  REG_IPU_OUT_GS = size;
  REG_IPU_OUT_STRIDE = outputW * 4;

  // Resize Foreground1 to the output size of the IPU
  xpos = (panel->w - outputW) / 2;
  ypos = (panel->h - outputH) / 2;
  jzfb_foreground_resize(jzfb, xpos, ypos, outputW, outputH);

  dev_dbg(&jzfb->pdev->dev, "Scaling %ux%u to %ux%u\n", fb->var.xres, fb->var.yres, outputW, outputH);
  printk("%s, scaling %ux%u to %ux%u\n", __func__, fb->var.xres, fb->var.yres, outputW, outputH);
}

static void jzfb_power_up(struct jzfb *jzfb)
{
  jzfb_lcdc_enable(jzfb);
  jzfb_ipu_enable(jzfb);
}

static void jzfb_power_down(struct jzfb *jzfb)
{
  ctrl_disable(jzfb);
  //clk_disable(jzfb->lpclk);

  jzfb_ipu_disable(jzfb);
  //clk_disable(jzfb->ipuclk);
}

/*
 * (Un)blank the display.
 */
static int jz4760fb_blank(int blank_mode, struct fb_info *info)
{
  struct jzfb *jzfb = info->par;

  mutex_lock(&jzfb->lock);
  if(blank_mode == FB_BLANK_UNBLANK){
    if(!jzfb->is_enabled){
      jzfb_power_up(jzfb);
      jzfb->is_enabled = true;
    }
  }
  else{
    if(jzfb->is_enabled){
      jzfb_power_down(jzfb);
      jzfb->is_enabled = false;
    }
  }
  mutex_unlock(&jzfb->lock);
  return 0;
}

static int jz4760fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *fb)
{
  struct jzfb *jzfb = fb->par;
  uint32_t vpan = var->yoffset;

  if(var->xoffset != fb->var.xoffset){
    return -EINVAL;
  }

  jzfb->delay_flush = 8;
  jzfb->pan_offset = fb->fix.line_length * vpan;
  dma_cache_wback_inv((unsigned long)(lcd_frame_fb + jzfb->pan_offset), fb->fix.line_length * var->yres);

  /*
   * The primary use of this function is to implement double buffering.
   * Explicitly waiting for vsync and then panning doesn't work in
   * practice because the woken up process doesn't always run before the
   * next frame has already started: the time between vsync and the start
   * of the next frame is typically less than one scheduler time slice.
   * Instead, we wait for vsync here in the pan function and apply the
   * new panning setting in the vsync interrupt, so we know that the new
   * panning setting has taken effect before this function returns.
   * Note that fb->var is only updated after we return, so we need our
   * own copy of the panning setting (jzfb->pan_offset).
   */
  jzfb_wait_for_vsync(jzfb); 
  return 0;
}

static inline unsigned int words_per_line(unsigned int width, unsigned int bpp)
{
  return (bpp * width + 31) / 32;
}

static int jz4760fb_map_smem(struct fb_info *fb)
{
  void *page_virt;
  unsigned int size = PAGE_ALIGN(MAX_XRES * MAX_YRES * 4 * 3);

  dev_dbg(fb->device, "FG1: %u bytes\n", size);
  lcd_frame_fb = alloc_pages_exact(size, GFP_KERNEL);
  if(!lcd_frame_fb){
    dev_err(fb->device, "Unable to map %u bytes of screen fb memory\n", size);
    return -ENOMEM;
  }
  
  /*
   * Set page reserved so that mmap will work. This is necessary
   * since we'll be remapping normal memory.
   */
  for(page_virt=lcd_frame_fb; page_virt<lcd_frame_fb+size; page_virt+=PAGE_SIZE){
    SetPageReserved(virt_to_page(page_virt));
    clear_page(page_virt);
  }

  fb->fix.smem_start = virt_to_phys(lcd_frame_fb);
  fb->fix.smem_len = size;
  fb->screen_base = (void*)KSEG1ADDR(lcd_frame_fb);
  printk("%s, smem_base: 0x%x, smem_len:%d, screen_base:0x%x\n", __func__, 
    (unsigned int)fb->fix.smem_start, (unsigned int)fb->fix.smem_len, (unsigned int)fb->screen_base);
  return 0;
}

static void jz4760fb_unmap_smem(struct fb_info *fb)
{
  if(lcd_frame_fb){
    void *page_virt;
    void *end = lcd_frame_fb + fb->fix.smem_len;
    for(page_virt=lcd_frame_fb; page_virt<end; page_virt+= PAGE_SIZE){
      ClearPageReserved(virt_to_page(page_virt));
    }
    free_pages_exact(lcd_frame_fb, fb->fix.smem_len);
  }
}

static void jz4760fb_set_panel_mode(struct jzfb *jzfb, const struct jz4760lcd_panel_t *panel)
{
#if 0
  // Configure LCDC
  writel(panel->cfg, jzfb->base + LCD_CFG);

  // Enable IPU auto-restart
  writel(LCD_IPUR_IPUREN | (panel->blw + panel->w + panel->elw) * panel->vsw / 3, jzfb->base + LCD_IPUR);

  // Set HT / VT / HDS / HDE / VDS / VDE / HPE / VPE
  writel((panel->blw + panel->w + panel->elw) << LCD_VAT_HT_BIT | (panel->bfw + panel->h + panel->efw) << LCD_VAT_VT_BIT, jzfb->base + LCD_VAT);
  writel(panel->blw << LCD_DAH_HDS_BIT | (panel->blw + panel->w) << LCD_DAH_HDE_BIT, jzfb->base + LCD_DAH);
  writel(panel->bfw << LCD_DAV_VDS_BIT | (panel->bfw + panel->h) << LCD_DAV_VDE_BIT, jzfb->base + LCD_DAV);
  writel(panel->hsw << LCD_HSYNC_HPE_BIT, jzfb->base + LCD_HSYNC);
  writel(panel->vsw << LCD_VSYNC_VPE_BIT, jzfb->base + LCD_VSYNC);

  // Enable foreground 1, OSD mode
  writew(LCD_OSDC_F1EN | LCD_OSDC_OSDEN, jzfb->base + LCD_OSDC);

  // Enable IPU, 18/24 bpp output
  writew(LCD_OSDCTRL_IPU | LCD_OSDCTRL_OSDBPP_18_24, jzfb->base + LCD_OSDCTRL);

  // Set a black background
  writel(0, jzfb->base + LCD_BGC);
#else
  int w = 320;
  int h = 240;

  // Configure LCDC
  REG_LCD_CFG = panel->cfg;

  // Enable IPU auto-restart
  REG_LCD_IPUR = LCD_IPUR_IPUREN | (panel->blw + w + panel->elw) * panel->vsw / 3;

  // Set HT / VT / HDS / HDE / VDS / VDE / HPE / VPE
  REG_LCD_VAT = (panel->blw + w + panel->elw) << LCD_VAT_HT_BIT | (panel->bfw + h + panel->efw) << LCD_VAT_VT_BIT;
  REG_LCD_DAH = panel->blw << LCD_DAH_HDS_BIT | (panel->blw + w) << LCD_DAH_HDE_BIT;
  REG_LCD_DAV = panel->bfw << LCD_DAV_VDS_BIT | (panel->bfw + h) << LCD_DAV_VDE_BIT;
  REG_LCD_HSYNC = panel->hsw << LCD_HSYNC_HPE_BIT;
  REG_LCD_VSYNC = panel->vsw << LCD_VSYNC_VPE_BIT;

  // Enable foreground 1, OSD mode
  REG_LCD_OSDC = LCD_OSDC_F1EN | LCD_OSDC_OSDEN;

  // Enable IPU, 18/24 bpp output
  REG_LCD_OSDCTRL = LCD_OSDCTRL_IPU | LCD_OSDCTRL_OSDBPP_18_24;

  // Set a black background
  REG_LCD_BGC = 0;
#endif

  printk("%s, w:%d, h:%d\n", __func__, panel->w, panel->h);
}

static void jzfb_change_clock(struct jzfb *jzfb, const struct jz4760lcd_panel_t *panel)
{
  unsigned int rate;

  rate = panel->fclk * (panel->w + panel->elw + panel->blw) * (panel->h + panel->efw + panel->bfw);

  /* Use pixel clock for LCD panel (as opposed to TV encoder). */
  //__cpm_select_pixclk_lcd();
  //clk_set_rate(jzfb->lpclk, rate);
  //dev_dbg(&jzfb->pdev->dev, "PixClock: req %u, got %lu\n", rate, clk_get_rate(jzfb->lpclk));
}

/* set the video mode according to info->var */
static int jz4760fb_set_par(struct fb_info *info)
{
  struct fb_var_screeninfo *var = &info->var;
  struct fb_fix_screeninfo *fix = &info->fix;
  struct jzfb *jzfb = info->par;

  if(jzfb->is_enabled){
    ctrl_disable(jzfb);
    jzfb_ipu_disable(jzfb);
  }
  else{
    //clk_enable(jzfb->lpclk);
    //clk_enable(jzfb->ipuclk);
  }

  jzfb->pan_offset = 0;
  jzfb->bpp = var->bits_per_pixel;
  fix->line_length = var->xres_virtual * (var->bits_per_pixel >> 3);

  jz4760fb_set_panel_mode(jzfb, jz_panel);
  jzfb_ipu_configure(jzfb, jz_panel);

  // Clear the framebuffer to avoid artifacts
  if(jzfb->clear_fb){
    void *page_virt = lcd_frame_fb;
    unsigned int size = fix->line_length * var->yres * 3;

    for(; page_virt<lcd_frame_fb+size; page_virt+=PAGE_SIZE){
      clear_page(page_virt);
    }
    dma_cache_wback_inv((unsigned long)lcd_frame_fb, size);
  }

  if(jzfb->is_enabled){
    jzfb_ipu_enable(jzfb);
    jzfb_lcdc_enable(jzfb);
  } 
  else{
    //clk_disable(jzfb->lpclk);
    //clk_disable(jzfb->ipuclk);
  }

  fix->visual = FB_VISUAL_TRUECOLOR;
  printk("%s, xres:%d, yres:%d, bpp:%d\n", __func__, var->xres, var->yres, var->bits_per_pixel);
  return 0;
}

static void jzfb_ipu_reset(struct jzfb *jzfb)
{
  ctrl_disable(jzfb);
  //clk_enable(jzfb->ipuclk);
  jzfb_ipu_disable(jzfb);
  REG_IPU_CTRL = IPU_CTRL_CHIP_EN | IPU_CTRL_RST;

  jz4760fb_set_panel_mode(jzfb, jz_panel);
  jzfb_ipu_configure(jzfb, jz_panel);
  jzfb_ipu_enable(jzfb);
  ctrl_enable(jzfb);
}

static int jz4760fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
  struct jzfb *jzfb = info->par;

  switch(cmd){
  case EFB_FBIO_WAITFORVSYNC:
    return jzfb_wait_for_vsync(jzfb);
  default:
    return -ENOIOCTLCMD;
  }
}

static struct fb_ops jz4760fb_ops = {
  .owner          = THIS_MODULE,

  .fb_setcolreg    = jz4760fb_setcolreg,
  .fb_check_var   = jz4760fb_check_var,
  .fb_set_par     = jz4760fb_set_par,
  .fb_blank        = jz4760fb_blank,
  .fb_pan_display  = jz4760fb_pan_display,
  .fb_mmap        = jz4760fb_mmap,
  .fb_ioctl        = jz4760fb_ioctl,
};

static irqreturn_t jz4760fb_interrupt_handler(int irq, void *dev_id)
{
  struct jzfb *jzfb = dev_id;

  if(jzfb->delay_flush == 0){
      struct fb_info *fb = jzfb->fb;
      dma_cache_wback_inv((unsigned long)(lcd_frame_fb + jzfb->pan_offset), fb->fix.line_length * fb->var.yres);
  }
  else{
    jzfb->delay_flush--;
  }
  
  jzfb_update_frame_address(jzfb);
  jzfb->vsync_count++;
  wake_up_interruptible_all(&jzfb->wait_vsync);

  REG_IPU_STATUS = 0;
  return IRQ_HANDLED;
}

static ssize_t keep_aspect_ratio_show(struct device *dev, struct device_attribute *attr, char *buf)
{
  return snprintf(buf, PAGE_SIZE, "%c\n", keep_aspect_ratio ? 'Y' : 'N');
}

static ssize_t keep_aspect_ratio_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
  struct jzfb *jzfb = dev_get_drvdata(dev);
  bool new_value = false;

  if(strtobool(buf, &new_value) < 0){
    return -EINVAL;
  }

  keep_aspect_ratio = new_value;
  if(jzfb->is_enabled && scaling_required(jzfb)){
    ctrl_disable(jzfb);
    jzfb_ipu_disable(jzfb);
    jzfb_ipu_configure(jzfb, jz_panel);
    jzfb_ipu_enable(jzfb);
    jzfb_lcdc_enable(jzfb);
  }
  return count;
}

static int jz4760_fb_probe(struct platform_device *pdev)
{
  struct jzfb *jzfb;
  struct fb_info *fb;
  int ret;

  printk("steward, %s\n", __func__);
  fb = framebuffer_alloc(sizeof(struct jzfb), &pdev->dev);
  if(!fb){
    dev_err(&pdev->dev, "Failed to allocate framebuffer device\n");
    return -ENOMEM;
  }

  jzfb = fb->par;
  jzfb->pdev = pdev;
  jzfb->bpp = 32;
  init_waitqueue_head(&jzfb->wait_vsync);

  strcpy(fb->fix.id, "jz-lcd");
  fb->fix.type = FB_TYPE_PACKED_PIXELS;
  fb->fix.type_aux  = 0;

  fb->fix.xpanstep  = 1;
  fb->fix.ypanstep  = 1;
  fb->fix.ywrapstep  = 0;
  fb->fix.accel  = FB_ACCEL_NONE;
  fb->fix.visual = FB_VISUAL_TRUECOLOR;

  fb->var.nonstd = 0;
  fb->var.activate = FB_ACTIVATE_NOW;
  fb->var.height = -1;
  fb->var.width  = -1;
  fb->var.accel_flags  = FB_ACCELF_TEXT;
  fb->var.bits_per_pixel = jzfb->bpp;

  fb->var.xres = jz_panel->w;
  fb->var.yres = jz_panel->h;
  fb->var.vmode = FB_VMODE_NONINTERLACED;
  jz4760fb_check_var(&fb->var, fb);

  fb->fbops  = &jz4760fb_ops;
  fb->flags  = FBINFO_FLAG_DEFAULT;

  fb->pseudo_palette  = jzfb->pseudo_palette;
  INIT_LIST_HEAD(&fb->modelist);

  ret = jz4760fb_map_smem(fb);
  if(ret){
    goto failed;
  }

  /* Init pixel clock. */
  /*jzfb->lpclk = clk_get(&pdev->dev, "lpclk");
  if(IS_ERR(jzfb->lpclk)){
    ret = PTR_ERR(jzfb->lpclk);
    dev_err(&pdev->dev, "Failed to get pixel clock: %d\n", ret);
    goto failed;
  }

  jzfb->ipuclk = clk_get(&pdev->dev, "ipu");
  if(IS_ERR(jzfb->ipuclk)){
    ret = PTR_ERR(jzfb->ipuclk);
    dev_err(&pdev->dev, "Failed to get ipu clock: %d\n", ret);
    goto failed;
  }*/

  if(request_irq(IRQ_IPU, jz4760fb_interrupt_handler, 0, "ipu", jzfb)){
    dev_err(&pdev->dev, "Failed to request IRQ.\n");
    ret = -EBUSY;
    goto failed;
  }

  mutex_init(&jzfb->lock);

  platform_set_drvdata(pdev, jzfb);
  jzfb->fb = fb;

  /*
   * We assume the LCDC is disabled initially. If you really must have
   * video in your boot loader, you'll have to update this driver.
   */
  jzfb_change_clock(jzfb, jz_panel);
  //clk_enable(jzfb->lpclk);
  fb->fix.line_length = fb->var.xres_virtual * (fb->var.bits_per_pixel >> 3);
  jzfb->delay_flush = 0;

  jzfb_ipu_reset(jzfb);
  jzfb->is_enabled = true;
  ret = register_framebuffer(fb);
  if(ret < 0){
    dev_err(&pdev->dev, "Failed to register framebuffer device.\n");
    goto failed;
  }
  dev_info(&pdev->dev, "fb%d: %s frame buffer device, using %dK of video memory\n", fb->node, fb->fix.id, fb->fix.smem_len>>10);
  printk("%s, x:%d, y:%d\n", __func__, fb->var.xres, fb->var.yres);
  return 0;

failed:
  jz4760fb_unmap_smem(fb);
err_release_fb:
  framebuffer_release(fb);
  return ret;
}

static int jz4760_fb_remove(struct platform_device *pdev)
{
  struct jzfb *jzfb = platform_get_drvdata(pdev);

  if(jzfb->is_enabled){
    jzfb_power_down(jzfb);
  }
  return 0;
}

static struct platform_driver jz4760_fb_driver = {
  .probe  = jz4760_fb_probe,
  .remove = jz4760_fb_remove,
  .driver = {
    .name  = "jz4760-rtc",
    .owner = THIS_MODULE,
  },
};

static int __init fb_init(void)
{
  return platform_driver_register(&jz4760_fb_driver);
}

static void __exit fb_cleanup(void)
{
  platform_driver_unregister(&jz4760_fb_driver);
}

module_init(fb_init);
module_exit(fb_cleanup);

MODULE_DESCRIPTION("JZ4760 LCD frame buffer driver");
MODULE_AUTHOR("Maarten ter Huurne <maarten@treewalker.org>, Steward Fu <steward.fu@gmail.com>");
MODULE_LICENSE("GPL");

