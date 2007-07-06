//****************************************************************************
// Copyright (C) 2001-2007 PEAK System-Technik GmbH
//
// linux@peak-system.com
// www.peak-system.com
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// Maintainer(s): Klaus Hitschler (klaus.hitschler@gmx.de)
//
// Major contributions by:
//                Edouard Tisserant (edouard.tisserant@lolitech.fr) XENOMAI
//                Laurent Bessard   (laurent.bessard@lolitech.fr)   XENOMAI
//                Oliver Hartkopp   (oliver.hartkopp@volkswagen.de) socketCAN
//                     
// Contributions: Marcel Offermans (marcel.offermans@luminis.nl)
//                Philipp Baer (philipp.baer@informatik.uni-ulm.de)
//****************************************************************************

//***************************************************************************
//
// all parts to handle the interface specific parts of pcan-dongle
//
// $Id: pcan_dongle.c 512 2007-06-01 12:06:00Z khitschler $
//
//****************************************************************************

//****************************************************************************
// INCLUDES
#include <src/pcan_common.h>     // must always be the 1st include
#include <linux/errno.h>
#include <linux/ioport.h>
#include <asm/io.h> 
#include <linux/sched.h>
#include <linux/slab.h>
#ifdef PARPORT_SUBSYSTEM
#include <linux/parport.h>
#endif
#include <linux/delay.h>

#include <linux/spinlock.h>

#include <src/pcan_dongle.h>
#include <src/pcan_sja1000.h>
#include <src/pcan_filter.h>

//****************************************************************************
// DEFINES
#define PCAN_DNG_SP_MINOR_BASE  16  // starting point of minors for SP devices
#define PCAN_DNG_EPP_MINOR_BASE 24  // starting point of minors for EPP devices
#define DNG_PORT_SIZE            4  // the address range of the dongle-port
#define ECR_PORT_SIZE            1  // size of the associated ECR register
#define DNG_DEFAULT_COUNT        4  // count of defaults for init

typedef void IRQHANDLER((*PARPORT_IRQHANDLER), int, void *, struct pt_regs *);

//****************************************************************************
// GLOBALS

//****************************************************************************
// LOCALS
static u16 dng_ports[] = {0x378, 0x278, 0x3bc, 0x2bc};
static u8  dng_irqs[]  = {7, 5, 7, 5};
static u16 dng_devices = 0;        // the number of accepted dng_devices
static u16 epp_devices = 0;        // ... epp_devices
static u16 sp_devices  = 0;        // ... sp_devices

static unsigned char nibble_decode[32] =
{
  0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
  0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
  0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
  0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7
};

//****************************************************************************
// CODE

//----------------------------------------------------------------------------
// enable and disable irqs
static void _parport_disable_irq(struct pcandev *dev)
{
  u16 _PC_ = (u16)dev->port.dng.dwPort + 2;
  outb(inb(_PC_) & ~0x10, _PC_);
}

static void _parport_enable_irq(struct pcandev *dev)
{
  u16 _PC_ = (u16)dev->port.dng.dwPort + 2;
  outb(inb(_PC_) | 0x10, _PC_);
}

// functions for SP port
static u8 pcan_dongle_sp_readreg(struct pcandev *dev, u8 port) // read a register
{
  u16 _PA_ = (u16)dev->port.dng.dwPort;
  u16 _PB_ = _PA_ + 1;
  u16 _PC_ = _PB_ + 1;
  u8  b0, b1 ;
  u8  irqEnable = inb(_PC_) & 0x10; // don't influence irqEnable
  #ifndef XENOMAI
  unsigned long flags;

  spin_lock_irqsave(&dev->port.dng.lock, flags);
  #endif

  outb((0x0B ^ 0x0D) | irqEnable, _PC_);
  outb((port & 0x1F) | 0x80,      _PA_);
  outb((0x0B ^ 0x0C) | irqEnable, _PC_);
  b1=nibble_decode[inb(_PB_)>>3];
  outb(0x40, _PA_);
  b0=nibble_decode[inb(_PB_)>>3];
  outb((0x0B ^ 0x0D) | irqEnable, _PC_);

  #ifndef XENOMAI
  spin_unlock_irqrestore(&dev->port.dng.lock, flags);
  #endif

  return  (b1 << 4) | b0 ;
}

static void pcan_dongle_writereg(struct pcandev *dev, u8 port, u8 data) // write a register
{
  u16 _PA_ = (u16)dev->port.dng.dwPort;
  u16 _PC_ = _PA_ + 2;
  u8  irqEnable = inb(_PC_) & 0x10; // don't influence irqEnable
#ifndef XENOMAI
  unsigned long flags;

  spin_lock_irqsave(&dev->port.dng.lock, flags);
#endif

  outb((0x0B ^ 0x0D) | irqEnable, _PC_);
  outb(port & 0x1F,               _PA_);
  outb((0x0B ^ 0x0C) | irqEnable, _PC_);
  outb(data,                      _PA_);
  outb((0x0B ^ 0x0D) | irqEnable, _PC_);

#ifndef XENOMAI
  spin_unlock_irqrestore(&dev->port.dng.lock, flags);
#endif
}

// functions for EPP port
static u8 pcan_dongle_epp_readreg(struct pcandev *dev, u8 port) // read a register
{
  u16 _PA_ = (u16)dev->port.dng.dwPort;
  u16 _PC_ = _PA_ + 2;
  u8  wert;
  u8  irqEnable = inb(_PC_) & 0x10; // don't influence irqEnable
#ifndef XENOMAI
  unsigned long flags;

  spin_lock_irqsave(&dev->port.dng.lock, flags);
#endif

  outb((0x0B ^ 0x0F) | irqEnable, _PC_);
  outb((port & 0x1F) | 0x80,      _PA_);
  outb((0x0B ^ 0x2E) | irqEnable, _PC_);
  wert = inb(_PA_);
  outb((0x0B ^ 0x0F) | irqEnable, _PC_);

#ifndef XENOMAI
  spin_unlock_irqrestore(&dev->port.dng.lock, flags);
#endif

  return wert;
}

#ifdef XENOMAI
static int pcan_dongle_req_irq(struct rtdm_dev_context *context)
{
  struct pcanctx_rt *ctx;
  struct pcandev *dev;
  
  ctx = (struct pcanctx_rt *)context->dev_private;
  dev = ctx->dev;
#else
static int pcan_dongle_req_irq(struct pcandev *dev)
{
#endif
  if (dev->wInitStep == 3)
  {
    #ifdef XENOMAI
    int err;
    if ((err = rtdm_irq_request(&ctx->irq_handle, ctx->irq, sja1000_irqhandler_rt,
            RTDM_IRQTYPE_SHARED | RTDM_IRQTYPE_EDGE, context->device->proc_name, ctx)))
      return err;
    #else
    #ifndef PARPORT_SUBSYSTEM
    int err;
    if ((err = request_irq(dev->port.dng.wIrq, sja1000_irqhandler, SA_INTERRUPT | SA_SHIRQ, "pcan", dev)))
      return err;
    #endif
    #endif

    dev->wInitStep++;
  }

  return 0;
}

static void pcan_dongle_free_irq(struct pcandev *dev)
{
  if (dev->wInitStep == 4)
  {
    #ifndef XENOMAI
    #ifndef PARPORT_SUBSYSTEM
    free_irq(dev->port.dng.wIrq, dev);
    #endif
    #endif

    dev->wInitStep--;
  }
}

// release and probe functions
static int pcan_dongle_cleanup(struct pcandev *dev)
{
  DPRINTK(KERN_DEBUG "%s: pcan_dongle_cleanup()\n", DEVICE_NAME);

  switch(dev->wInitStep)
  {
    case 4: pcan_dongle_free_irq(dev);
    case 3: if (dev->wType == HW_DONGLE_SJA)
               sp_devices--;
             else
               epp_devices--;
             dng_devices = sp_devices + epp_devices;
    case 2:
           #ifndef PARPORT_SUBSYSTEM
             if (dev->wType == HW_DONGLE_SJA_EPP)
             release_region(dev->port.dng.wEcr, ECR_PORT_SIZE);
           #endif
    case 1:
           #ifdef PARPORT_SUBSYSTEM
             #ifndef XENOMAI
             parport_unregister_device(dev->port.dng.pardev);
             #endif
           #else
             release_region(dev->port.dng.dwPort, DNG_PORT_SIZE);
           #endif
            
    case 0: pcan_delete_filter_chain(dev->filter);
            dev->filter = NULL;
            dev->wInitStep = 0;
  }

  return 0;
}

// to switch epp on or restore register
static void setECR(struct pcandev *dev)
{
  u16 wEcr = dev->port.dng.wEcr;

  dev->port.dng.ucOldECRContent = inb(wEcr);
  outb((dev->port.dng.ucOldECRContent & 0x1F) | 0x20, wEcr);

  if (dev->port.dng.ucOldECRContent == 0xff)
    printk(KERN_DEBUG "%s: realy ECP mode configured?\n", DEVICE_NAME);
}

static void restoreECR(struct pcandev *dev)
{
  u16 wEcr = dev->port.dng.wEcr;

  outb(dev->port.dng.ucOldECRContent, wEcr);

  DPRINTK(KERN_DEBUG "%s: restore ECR\n", DEVICE_NAME);
}

#ifdef PARPORT_SUBSYSTEM

static int pcan_dongle_probe(struct pcandev *dev) // probe for type
{
  struct parport *p;

  DPRINTK(KERN_DEBUG "%s: pcan_dongle_probe() - PARPORT_SUBSYSTEM\n", DEVICE_NAME);
  
  // probe does not probe for the sja1000 device here - this is done at sja1000_open()
  p = parport_find_base(dev->port.dng.dwPort);
  if (!p)
  {
    DPRINTK(KERN_DEBUG "found no parport\n");
    return -ENXIO;
  }
  else
  {
    #ifndef XENOMAI
    // register my device at the parport
    dev->port.dng.pardev = parport_register_device(p, "pcan", NULL, NULL, 
                                      (PARPORT_IRQHANDLER)sja1000_irqhandler, 0, (void *)dev);
              
    if (!dev->port.dng.pardev)
    {
      DPRINTK(KERN_DEBUG "found no parport device\n");
      return -ENODEV;
    }
    #endif
  }
  
  return 0;
}
#else
static int pcan_dongle_probe(struct pcandev *dev) // probe for type
{
  int result = 0;
  
  DPRINTK(KERN_DEBUG "%s: pcan_dongle_probe()\n", DEVICE_NAME);
  
  result = ___request_region(dev->port.dng.dwPort, DNG_PORT_SIZE, DEVICE_NAME);

  if (!result)
  {  
    dev->wInitStep = 1;

    if (dev->wType == HW_DONGLE_SJA_EPP)
    {
      result = ___request_region(dev->port.dng.wEcr, ECR_PORT_SIZE, DEVICE_NAME);    
      if (!result)
        dev->wInitStep = 2;
    }
  }
  
  return result;
}
#endif // PARPORT_SUBSYSTEM

// interface depended open and close
static int pcan_dongle_open(struct pcandev *dev)
{
  int result = 0;
  u16 wPort;
  
  DPRINTK(KERN_DEBUG "%s: pcan_dongle_open()\n", DEVICE_NAME);
  
  #ifndef XENOMAI
  #ifdef PARPORT_SUBSYSTEM
  result = parport_claim(dev->port.dng.pardev);
  
  if (!result)
  {
    if (dev->port.dng.pardev->port->irq == PARPORT_IRQ_NONE)
    {
      printk(KERN_ERR "%s: no irq associated to parport.\n", DEVICE_NAME);
      result = -ENXIO;
    }
  }
  else
    printk(KERN_ERR "%s: can't claim parport.\n", DEVICE_NAME);  
  #endif // PARPORT_SUBSYSTEM
  #endif
  
  // save port state
  if (!result)
  {
    wPort    = (u16)dev->port.dng.dwPort;
    
    // save old port contents
    dev->port.dng.ucOldDataContent     = inb(wPort);
    dev->port.dng.ucOldControlContent  = inb(wPort + 2);
    
    // switch to epp mode if possible
    if (dev->wType == HW_DONGLE_SJA_EPP)
      setECR(dev); 
  
    // enable irqs
    #ifdef PARPORT_SUBSYSTEM
    _parport_enable_irq(dev); // parport_enable_irq(dev->port.dng.pardev->port); not working since 2.4.18
    #else
    _parport_enable_irq(dev);
    #endif
  }  
  
  return result;
}

static int pcan_dongle_release(struct pcandev *dev)
{
  u16 wPort = (u16)dev->port.dng.dwPort;

  DPRINTK(KERN_DEBUG "%s: pcan_dongle_release()\n", DEVICE_NAME);
  
  // disable irqs
  #ifdef PARPORT_SUBSYSTEM
  _parport_disable_irq(dev); // parport_disable_irq(dev->port.dng.pardev->port); not working since 2.4.18
  #else
  _parport_disable_irq(dev);
  #endif

  if (dev->wType == HW_DONGLE_SJA_EPP)
    restoreECR(dev);
    
  // restore port state
  outb(dev->port.dng.ucOldDataContent, wPort);
  outb(dev->port.dng.ucOldControlContent, wPort + 2);
  
  #ifndef XENOMAI
  #ifdef PARPORT_SUBSYSTEM
  parport_release(dev->port.dng.pardev);
  #endif
  #endif
  
  return 0;
}

static int  pcan_dongle_init(struct pcandev *dev, u32 dwPort, u16 wIrq, char *type)
{
  int err;
  
  DPRINTK(KERN_DEBUG "%s: pcan_dongle_init(), dng_devices = %d\n", DEVICE_NAME, dng_devices);
  
  // init process wait queues
  init_waitqueue_head(&dev->read_queue);
  init_waitqueue_head(&dev->write_queue);
  
  // set this before any instructions, fill struct pcandev, part 1 
  dev->wInitStep   = 0;  
  dev->cleanup     = pcan_dongle_cleanup;
  dev->req_irq     = pcan_dongle_req_irq;
  dev->free_irq    = pcan_dongle_free_irq;
  dev->open        = pcan_dongle_open;
  dev->release     = pcan_dongle_release; 
  dev->filter      = pcan_create_filter_chain();

  spin_lock_init(&dev->port.dng.lock);

  // fill struct pcandev, 1st check if a default is set
  if (!dwPort)
  {
    // there's no default available
    if (dng_devices >= DNG_DEFAULT_COUNT)
      return -ENODEV;
    
    dev->port.dng.dwPort = dng_ports[dng_devices];
  }
  else
    dev->port.dng.dwPort = dwPort;
  
  if (!wIrq)
  {
    if (dng_devices >= DNG_DEFAULT_COUNT)
      return -ENODEV;
    
    dev->port.dng.wIrq   = dng_irqs[dng_devices];    
  }
  else
    dev->port.dng.wIrq   = wIrq;    
  
  if (dev->wType == HW_DONGLE_SJA)
  {
    dev->nMinor        = PCAN_DNG_SP_MINOR_BASE + sp_devices; 
    dev->readreg       = pcan_dongle_sp_readreg;
    dev->writereg      = pcan_dongle_writereg;
    dev->port.dng.wEcr = 0; // set to anything
  }
  else
  {
    dev->nMinor        = PCAN_DNG_EPP_MINOR_BASE + epp_devices;  
    dev->readreg       = pcan_dongle_epp_readreg;
    dev->writereg      = pcan_dongle_writereg;
    dev->port.dng.wEcr = (u16)dev->port.dng.dwPort + 0x402;
  }
    
  // is the device really available?    
  if ((err = pcan_dongle_probe(dev)) < 0)
  {
    printk(KERN_ERR "%s: failed to claim port 0x%04x\n", DEVICE_NAME, dev->port.dng.dwPort);
    return err;
  }
    
  dev->ucPhysicallyInstalled = 1;

  if (dev->wType == HW_DONGLE_SJA)
    sp_devices++;
  else
    epp_devices++;
    
  dng_devices = sp_devices + epp_devices;
  
  dev->wInitStep = 3;

  printk(KERN_INFO "%s: %s-dongle device minor %d prepared (io=0x%04x,irq=%d)\n", DEVICE_NAME, 
                               dev->type, dev->nMinor, dev->port.dng.dwPort, dev->port.dng.wIrq);
  
  return 0;
}

int pcan_create_dongle_devices(char *type, u32 io, u16 irq)
{
  int result = 0;
  struct pcandev *dev = NULL;
  
  DPRINTK(KERN_DEBUG "%s: pcan_create_dongle_devices(%s, 0x%x, %d)\n", DEVICE_NAME, type, io, irq);
  
  if ((dev = (struct pcandev *)kmalloc(sizeof(struct pcandev), GFP_KERNEL)) == NULL)
    goto fail;

  pcan_soft_init(dev, type, (!strncmp(type, "sp", 4)) ? HW_DONGLE_SJA : HW_DONGLE_SJA_EPP);
  
  dev->device_open    = sja1000_open;
  dev->device_write   = sja1000_write;
  dev->device_release = sja1000_release;
  #ifdef NETDEV_SUPPORT
  dev->netdevice_write  = sja1000_write_frame;
  #endif
  
  result = pcan_dongle_init(dev, io, irq, type);
  
  if (result)
  {
    dev->cleanup(dev);
    kfree(dev);
    goto fail;
  }
  else
  {
    list_add_tail(&dev->list, &pcan_drv.devices);  // add this device to the list        
    pcan_drv.wDeviceCount++;
  }        

  fail:
  if (result)
    DPRINTK(KERN_DEBUG "%s: pcan_create_dongle_devices() failed!\n", DEVICE_NAME);
  else
    DPRINTK(KERN_DEBUG "%s: pcan_create_dongle_devices() OK!\n", DEVICE_NAME);
    
  return result;
}


