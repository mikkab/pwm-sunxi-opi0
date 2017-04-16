#include <linux/init.h>  
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kdev_t.h>		// MKDEV definition
#include <asm/io.h>
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Igor Boguslavsky");
MODULE_DESCRIPTION("PWM driver for the Orage Pi Zero"); 
MODULE_VERSION("2.0");           
 
#define PA_CFG0_REG	  	0x01c20800 		// PORT A pin control register 
#define PA_PULL0_REG	  	PA_CFG0_REG+0x01c	// PORT A pullup control register 
#define PWM_CTRL_REG	  	0x01c21400 		// PWM control register
#define PWM_CH0_PERIOD  	PWM_CTRL_REG+0x04	// pwm0 period register
#define PWM_CH1_PERIOD  	PWM_CTRL_REG+0x08	// pwm1 period register

static struct class_attribute pwm_class_attrs[] = {
  __ATTR_NULL
};

static struct class pwm_class = {
  .name =         "pwm-sunxi-opi0",
  .owner =        THIS_MODULE,
  .class_attrs =  pwm_class_attrs,
};

struct portA_ctrl {
  unsigned int unused1: 20;
  unsigned int PA5_SELECT: 3;		// PA05 multi-function pin
  unsigned int unused2: 1;
  unsigned int PA6_SELECT: 3;		// PA06 multi-function pin
  unsigned int unused3: 5;
};

union portA_ctrl_u {
  struct portA_ctrl s;
  unsigned int initializer;
};

enum h2plus_pwm_prescale {
  PRESCALE_DIV120  = 0x00,  // Divide 24mhz clock by 120
  PRESCALE_DIV180  = 0x01,
  PRESCALE_DIV240  = 0x02,
  PRESCALE_DIV360  = 0x03,
  PRESCALE_DIV480  = 0x04,
  PRESCALE_INVx05  = 0x05,  // Invalid prescaler setting
  PRESCALE_INVx06  = 0x06,
  PRESCALE_INVx07  = 0x07,
  PRESCALE_DIV12k  = 0x08,
  PRESCALE_DIV24k  = 0x09,
  PRESCALE_DIV36k  = 0x0a,
  PRESCALE_DIV48k  = 0x0b,
  PRESCALE_DIV72k  = 0x0c,
  PRESCALE_INVx0d  = 0x0d,
  PRESCALE_INVx0e  = 0x0e,
  PRESCALE_DIV_NO  = 0x0f
};

static const __u32 clock_divider[] = {
  120, 180, 240, 360, 480, -1, -1, -1, 12000, 24000, 36000, 48000, 72000, -1, -1, 1};

struct h2plus_pwm_ctrl {
  enum h2plus_pwm_prescale pwm_ch0_prescal:4; 	// Prescalar setting, 4 bits long
  unsigned int pwm_ch0_en:1;                 	// pwm chan enable
  unsigned int pwm_ch0_act_sta:1;          	// pwm chan polarity (0=active low, 1=active high)
  unsigned int sclk_ch0_gating:1;         	// Allow clock to run 
  unsigned int pwm_ch0_mode:1;             	// Mode - 0 = cycle(running), 1=only 1 pulse */
  unsigned int pwm_ch0_pul_start:1;        	// Write 1 for mode pulse above to start
  unsigned int pwm0_bypass:1;			// Main clock bypass PWM and output on the PWM pin
  unsigned int unused1:5;              		// Bits 10-14 are unused in H2+ PWM control register
  //
  enum h2plus_pwm_prescale pwm_ch1_prescal:4;
  unsigned int pwm_ch1_en:1;                 	
  unsigned int pwm_ch1_act_sta:1;          	
  unsigned int sclk_ch1_gating:1;         	
  unsigned int pwm_ch1_mode:1;             
  unsigned int pwm_ch1_pul_start:1;        	
  unsigned int pwm1_bypass:1;			
  unsigned int unused2:3;	
  //           			
  unsigned int pwm0_rdy:1;			// CH0 Period register ready bit (1: busy, 0:ready to write)
  unsigned int pwm1_rdy:1;			// CH1 Period register ready bit
  //	
  unsigned int unused3:2;
};

union h2plus_pwm_ctrl_u {
  struct h2plus_pwm_ctrl s;
  unsigned int initializer;
};

struct h2plus_pwm_period {
  unsigned int active_cycles:16;
  unsigned int entire_cycles:16;
};

union h2plus_pwm_period_u {
  struct h2plus_pwm_period s;
  unsigned int initializer;
};

struct pwm_channel {

  unsigned int channel;
  unsigned int use_count;

  enum h2plus_pwm_prescale prescale;  
  __u8 enable, polarity, gating, mode, pulse_start, bypass;

  // PWM timing structures (entire cycles, active cycles)
  void __iomem *period_reg_addr;
  union h2plus_pwm_period_u cycles;
};

// sysfs access functions
static ssize_t pwm_run_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_polarity_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_prescale_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_entirecycles_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_activecycles_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_freqperiod_show (struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t pwm_hardware_show (struct device *dev,struct device_attribute *attr, char *buf);

static ssize_t pwm_run_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);
static ssize_t pwm_polarity_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);
static ssize_t pwm_prescale_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);
static ssize_t pwm_entirecycles_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);
static ssize_t pwm_activecycles_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);
static ssize_t pwm_freqperiod_store (struct device *dev,struct device_attribute *attr, const char *buf, size_t size);

static DEVICE_ATTR(run, 0666, pwm_run_show, pwm_run_store);
static DEVICE_ATTR(polarity, 0666, pwm_polarity_show, pwm_polarity_store);
static DEVICE_ATTR(prescale, 0666, pwm_prescale_show, pwm_prescale_store);
static DEVICE_ATTR(entirecycles, 0666, pwm_entirecycles_show, pwm_entirecycles_store);
static DEVICE_ATTR(activecycles, 0666, pwm_activecycles_show, pwm_activecycles_store);
static DEVICE_ATTR(freqperiod, 0666, pwm_freqperiod_show, pwm_freqperiod_store);
static DEVICE_ATTR(hardware, 0444, pwm_hardware_show, NULL);

static const struct attribute *pwm_attrs[] = {
  &dev_attr_run.attr,
  &dev_attr_polarity.attr,
  &dev_attr_prescale.attr,
  &dev_attr_entirecycles.attr,
  &dev_attr_activecycles.attr,
  &dev_attr_freqperiod.attr,
  &dev_attr_hardware.attr,
  NULL,
};

static const struct attribute_group pwm_attr_group = {
  .attrs = (struct attribute **) pwm_attrs
};

void __iomem *ctrl_reg_addr; 

static struct class pwm_class;

struct kobject *pwm0_kobj;
struct kobject *pwm1_kobj;

struct device *pwm0;
struct device *pwm1;

// Two available PWM channels on OPI0
static struct pwm_channel channel[2];

// Helper functions
ssize_t pwm_enable (unsigned int enable, struct pwm_channel *chan);
int update_ctrl_reg (void);

static int __init opi0_init (void) {
void __iomem *pa_cfg0_reg, *pa_pull0_reg;	// Port A pin config i& pull-up registers
union portA_ctrl_u pin_ctrl;
__u32 pin_pullup;

  if (class_register (&pwm_class)) {
    class_unregister (&pwm_class);
    return -ENODEV;
  }

  pwm0 = device_create (&pwm_class, NULL, MKDEV(0,0), &channel[0], "pwm0");
  pwm0_kobj = &pwm0 -> kobj;

  pwm1 = device_create (&pwm_class, NULL, MKDEV(0,0), &channel[1], "pwm1");
  pwm1_kobj = &pwm1 -> kobj;
	
  if (sysfs_create_group (pwm0_kobj, &pwm_attr_group)) {
    device_destroy (&pwm_class, pwm0 -> devt);
    device_destroy (&pwm_class, pwm1 -> devt);
    class_unregister (&pwm_class);
    return -ENODEV;
  }
	
  if (sysfs_create_group (pwm1_kobj, &pwm_attr_group)) {
    device_destroy (&pwm_class, pwm0 -> devt);
    device_destroy (&pwm_class, pwm1 -> devt);
    class_unregister (&pwm_class);
    return -ENODEV;
  }
   
  // Help identify individual channels based on private data
  channel[0].channel = 0;
  channel[1].channel = 1;
      
  // Map important registers into kernel address space
  ctrl_reg_addr = ioremap (PWM_CTRL_REG, 4);
  channel[0].period_reg_addr = ioremap (PWM_CH0_PERIOD, 4);
  channel[1].period_reg_addr = ioremap (PWM_CH1_PERIOD, 4);

  // Set up PA5 for PWM0 (UART0 RX by default - remap in FEX)
  // Set up PA6 for PWM1
  pa_cfg0_reg = ioremap (PA_CFG0_REG, 4);
  pin_ctrl.initializer = ioread32 (pa_cfg0_reg);
      
  // PWM is "011" in H3, "010" in A33 - try each one
  pin_ctrl.s.PA5_SELECT = 0b011;
  pin_ctrl.s.PA6_SELECT = 0b011;

  iowrite32 (pin_ctrl.initializer, pa_cfg0_reg);
  iounmap (pa_cfg0_reg);

  // Enable pull-ups
  pa_pull0_reg = ioremap (PA_PULL0_REG, 4);
  pin_pullup = ioread32 (pa_pull0_reg);
      
  // "01" -> enable pull-up
  pin_pullup |=  (1 << 10);
  pin_pullup &= ~(1 << 11);
  pin_pullup |=  (1 << 12);
  pin_pullup &= ~(1 << 13);

  iowrite32 (pin_pullup, pa_pull0_reg);
  iounmap (pa_pull0_reg);
  
  printk(KERN_INFO "[%s] initialized ok\n", pwm_class.name);
  return 0;
}
 
static void __exit opi0_exit (void) {

  // Stop PWMs
  channel[0].enable = 0;
  channel[1].enable = 0;
  update_ctrl_reg();

  iounmap (ctrl_reg_addr);
  iounmap (channel[0].period_reg_addr);
  iounmap (channel[1].period_reg_addr);

  device_destroy (&pwm_class, pwm0 -> devt);
  device_destroy (&pwm_class, pwm1 -> devt);
  class_unregister (&pwm_class);

  printk(KERN_INFO "[%s] exiting\n", pwm_class.name);
}
 
// Accessors
//
static ssize_t pwm_run_show (struct device *dev, struct device_attribute *attr, char *buf) {

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  return sprintf (buf, "%u\n", channel -> enable);
}

static ssize_t pwm_polarity_show (struct device *dev, struct device_attribute *attr, char *buf) {

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  ssize_t status = 0;
  status = sprintf (buf, "%u\n", channel -> polarity);

  return status;
}

static ssize_t pwm_prescale_show (struct device *dev, struct device_attribute *attr, char *buf) {

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  ssize_t status = 0;
  status = sprintf (buf, "%u\n", channel -> prescale);

  return status;
}

static ssize_t pwm_entirecycles_show (struct device *dev, struct device_attribute *attr, char *buf) {

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  ssize_t status;
  status = sprintf (buf, "%u\n", channel -> cycles.s.entire_cycles);

  return status;
}

static ssize_t pwm_activecycles_show (struct device *dev, struct device_attribute *attr, char *buf) {

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  ssize_t status;
  status = sprintf (buf, "%u\n", channel -> cycles.s.active_cycles);

  return status;
}

static ssize_t pwm_freqperiod_show (struct device *dev, struct device_attribute *attr, char *buf) {
ssize_t status;
unsigned int clk_freq, pwm_freq;

  const struct pwm_channel *channel = dev_get_drvdata (dev);

  clk_freq = (unsigned int) 24000000 / clock_divider[channel -> prescale];
  pwm_freq = (unsigned int) clk_freq / (channel -> cycles.s.entire_cycles + 1);

  status = sprintf (buf, "%uhz\n", pwm_freq);

  return status;
}

static ssize_t pwm_hardware_show (struct device *dev, struct device_attribute *attr, char *buf) {
ssize_t status;
void __iomem *pa_cfg0_reg;					// Port A config register
union portA_ctrl_u pin_ctrl;
union h2plus_pwm_ctrl_u pwm_ctrl;   				// PWM control register
union h2plus_pwm_period_u pwm0_period_reg, pwm1_period_reg; 	// Period registers
	
  // PA register pin states
  pa_cfg0_reg = ioremap (PA_CFG0_REG, 4);
  pin_ctrl.initializer = ioread32 (pa_cfg0_reg);
  iounmap (pa_cfg0_reg);
	
  // PWM control and periods registers
  pwm_ctrl.initializer = ioread32 (ctrl_reg_addr);
  pwm0_period_reg.initializer = ioread32 (channel[0].period_reg_addr);
  pwm1_period_reg.initializer = ioread32 (channel[1].period_reg_addr);
	
  status = sprintf (buf, "PORT A Control: 0x%x\n\
PA05: 0x%01x, PA06: 0x%01x\n\
\n\
PWM Control:0x%08x\n\
\n\
  PWM0 Channel Prescaler: 0x%04x\n\
  PWM0 Channel Enable: %d\n\
  PWM0 Channel Polarity: %d\n\
  PWM0 Channel Gating: %d\n\
  PWM0 Channel Mode: %d\n\
  PWM0 Channel Clock Bypass: %d\n\n\
\n\
  PWM1 Channel Prescaler: 0x%04x\n\
  PWM1 Channel Enable: %d\n\
  PWM1 Channel Polarity: %d\n\
  PWM1 Channel Gating: %d\n\
  PWM1 Channel Mode: %d\n\
  PWM1 Channel Clock Bypass: %d\n\n\
\n\
PWM0 Entire Cycles: 0x%04x, PWM0 Active Cycles: 0x%04x\n\
PWM1 Entire Cycles: 0x%04x, PWM1 Active Cycles: 0x%04x\n",

    pin_ctrl.initializer,
    pin_ctrl.s.PA5_SELECT, pin_ctrl.s.PA6_SELECT,
    pwm_ctrl.initializer,
    pwm_ctrl.s.pwm_ch0_prescal,
    pwm_ctrl.s.pwm_ch0_en,
    pwm_ctrl.s.pwm_ch0_act_sta,
    pwm_ctrl.s.sclk_ch0_gating,
    pwm_ctrl.s.pwm_ch0_mode,
    pwm_ctrl.s.pwm0_bypass,
    pwm_ctrl.s.pwm_ch1_prescal,
    pwm_ctrl.s.pwm_ch1_en,
    pwm_ctrl.s.pwm_ch1_act_sta,
    pwm_ctrl.s.sclk_ch1_gating,
    pwm_ctrl.s.pwm_ch1_mode,
    pwm_ctrl.s.pwm1_bypass,		    
    pwm0_period_reg.s.entire_cycles, pwm0_period_reg.s.active_cycles, 
    pwm1_period_reg.s.entire_cycles, pwm1_period_reg.s.active_cycles);
	
  return status;
}

// Modifiers
//
static ssize_t pwm_run_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {

  struct pwm_channel *channel = dev_get_drvdata (dev);

  ssize_t status = -EINVAL;

  int enable = 0;

  if (sscanf (buf, "%d", &enable)) {

    if (enable < 2) {

      if (pwm_enable (enable, channel))

	// Good status to return (the input is the string size of *buf in bytes)
        status = size;
    }
  }

  return status;
}

static ssize_t pwm_polarity_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {
struct pwm_channel *channel = dev_get_drvdata (dev);
unsigned char polarity = 0;

  if (sscanf (buf, "%hhu", &polarity)) {

    if (polarity > 0) 
      channel -> polarity = 1;
    else 
      channel -> polarity = 0;

    update_ctrl_reg();

    printk (KERN_INFO "[%s] polarity set to: %u\n", pwm_class.name, polarity);

    return size;
  }

  return -EINVAL;
}

static ssize_t pwm_prescale_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {

  struct pwm_channel *channel = dev_get_drvdata (dev);

  unsigned char prescale = 0;

  if (sscanf (buf, "%hhu", &prescale)) {

    if (prescale >= PRESCALE_DIV120 && prescale <= PRESCALE_DIV_NO) {

      // check for invalid prescale settings here
      channel -> prescale = prescale;  

      update_ctrl_reg();

      return size;
    }
  }

  return -EINVAL;
}

static ssize_t pwm_entirecycles_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {
__u16 entirecycles = 0;
union h2plus_pwm_ctrl_u ctrl_reg;

  struct pwm_channel *channel = dev_get_drvdata (dev);

  // Successful sscanf?
  if (sscanf (buf, "%hu", &entirecycles)) {

    // Could be anything between 0 and 65535
    channel -> cycles.s.entire_cycles = entirecycles;  

    // Wait until period register is ready for write
    if (channel -> channel == 0) {
      do {
        ctrl_reg.initializer = ioread32 (ctrl_reg_addr);
      }
      while (ctrl_reg.s.pwm0_rdy); // 1 if busy
    }
    else {
      do {
        ctrl_reg.initializer = ioread32 (ctrl_reg_addr);
      }
      while (ctrl_reg.s.pwm1_rdy); // 1 if busy
    }

    iowrite32 (channel -> cycles.initializer, channel -> period_reg_addr);

    printk (KERN_INFO "[%s] entire_cycles: 0x%04x active_cycles: 0x%04x\n", 
      pwm_class.name, channel -> cycles.s.entire_cycles, channel -> cycles.s.active_cycles);

    return size;
  }

  return -EINVAL;
}

static ssize_t pwm_activecycles_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {
__u16 activecycles = 0;
union h2plus_pwm_ctrl_u ctrl_reg;

  struct pwm_channel *channel = dev_get_drvdata (dev);

  if (sscanf (buf, "%hu", &activecycles)) {

    // Could be anything between 0 and 65535
    channel -> cycles.s.active_cycles = activecycles;

    // Wait until period register is ready for write
    if (channel -> channel == 0) {
      do {
        ctrl_reg.initializer = ioread32 (ctrl_reg_addr);
      }
      while (ctrl_reg.s.pwm0_rdy); // 1 if busy
    }
    else {
      do {
        ctrl_reg.initializer = ioread32 (ctrl_reg_addr);
      }
      while (ctrl_reg.s.pwm1_rdy); // 1 if busy
    }

    iowrite32 (channel -> cycles.initializer, channel -> period_reg_addr);

    printk (KERN_INFO "[%s] entire_cycles: 0x%04x active_cycles: 0x%04x\n", 
      pwm_class.name, channel -> cycles.s.entire_cycles, channel -> cycles.s.active_cycles);

    return size;
  }

  return -EINVAL;
}

static ssize_t pwm_freqperiod_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t size) {

  return -EINVAL;
}

// Helpers
ssize_t pwm_enable (unsigned int enable, struct pwm_channel *chan) {

  chan -> enable   = enable;
  chan -> polarity = 1;
  chan -> gating   = 1;
  chan -> mode     = 0;  // cycle mode
  chan -> prescale = PRESCALE_DIV240; // or PRESCALE_DIV_NO (24Mhz)
	  
  update_ctrl_reg();

  return 1;
}

int update_ctrl_reg () {
union h2plus_pwm_ctrl_u ctrl;

  ctrl.s.pwm_ch0_prescal   = channel[0].prescale;
  ctrl.s.pwm_ch0_en        = channel[0].enable; 
  ctrl.s.pwm_ch0_act_sta   = channel[0].polarity; 
  ctrl.s.sclk_ch0_gating   = channel[0].gating;
  ctrl.s.pwm_ch0_mode      = channel[0].mode; 
  ctrl.s.pwm_ch0_pul_start = channel[0].pulse_start;
  ctrl.s.pwm0_bypass       = channel[0].bypass;

  ctrl.s.pwm_ch1_prescal   = channel[1].prescale;
  ctrl.s.pwm_ch1_en        = channel[1].enable; 
  ctrl.s.pwm_ch1_act_sta   = channel[1].polarity; 
  ctrl.s.sclk_ch1_gating   = channel[1].gating;
  ctrl.s.pwm_ch1_mode      = channel[1].mode; 
  ctrl.s.pwm_ch1_pul_start = channel[1].pulse_start;
  ctrl.s.pwm1_bypass       = channel[1].bypass;

  iowrite32 (ctrl.initializer, ctrl_reg_addr);
  return 0;
}

module_init(opi0_init);
module_exit(opi0_exit);
