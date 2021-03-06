/*
 *  SPI support on GPIO for AR9331
 *
 *    sarin <sarin2.0@gmail.com>
 *    Baseg on gpio-sqwave: https://github.com/blackswift/gpio-sqwave
 *  
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/clk.h>

#include <asm/mach-ath79/ar71xx_regs.h>
#include <asm/mach-ath79/ath79.h>
#include <asm/mach-ath79/irq.h>

#include <asm/siginfo.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/slab.h>

////////////////////////////////////////////////////////////////////////////////////////////

#define DRV_NAME	"gt-spi"
#define CONTROL_FILE_NAME	"spi.control"
#define SPI_FILE_NAME	"spi.data"

////////////////////////////////////////////////////////////////////////////////////////////


#include <linux/mm.h>  /* mmap related stuff */

#ifndef VM_RESERVED
# define  VM_RESERVED   (VM_DONTEXPAND | VM_DONTDUMP)
#endif






static int timer=3;
module_param(timer, int, 0);
MODULE_PARM_DESC(timer, "system timer number (0-3)");

////////////////////////////////////////////////////////////////////////////////////////////

#define DEBUG_OUT

#ifdef	DEBUG_OUT
#define debug(fmt,args...)	printk (KERN_INFO DRV_NAME ": " fmt ,##args)
#else
#define debug(fmt,args...)
#endif	/* DEBUG_OUT */

static unsigned int _timer_frequency=200000000;
static spinlock_t	_lock;
static unsigned int	_gpio_prev=0;

////////////////////////////////////////////////////////////////////////////////////////////

#define ATH79_TIMER0_IRQ		ATH79_MISC_IRQ(0)
#define AR71XX_TIMER0_RELOAD	0x04

#define ATH79_TIMER1_IRQ		ATH79_MISC_IRQ(8)
#define AR71XX_TIMER1_RELOAD	0x98

#define ATH79_TIMER2_IRQ		ATH79_MISC_IRQ(9)
#define AR71XX_TIMER2_RELOAD	0xA0

#define ATH79_TIMER3_IRQ		ATH79_MISC_IRQ(10)
#define AR71XX_TIMER3_RELOAD	0xA8

static struct _timer_desc_struct
{
	unsigned int	irq;
	unsigned int	reload_reg;
} _timers[4]=
{
		{	ATH79_TIMER0_IRQ, AR71XX_TIMER0_RELOAD	},
		{	ATH79_TIMER1_IRQ, AR71XX_TIMER1_RELOAD	},
		{	ATH79_TIMER2_IRQ, AR71XX_TIMER2_RELOAD	},
		{	ATH79_TIMER3_IRQ, AR71XX_TIMER3_RELOAD	}
};

////////////////////////////////////////////////////////////////////////////////////////////

#define GPIO_OFFS_READ		0x04
#define GPIO_OFFS_SET		0x0C
#define GPIO_OFFS_CLEAR		0x10

////////////////////////////////////////////////////////////////////////////////////////////

void __iomem *ath79_timer_base=NULL;

void __iomem *gpio_addr=NULL;
void __iomem *gpio_readdata_addr=NULL;
void __iomem *gpio_setdataout_addr=NULL;
void __iomem *gpio_cleardataout_addr=NULL;

////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
	uint8_t *	data;
	size_t	*	len;
} _spi_data_struct;

typedef struct
{
	int					timer;
	int					irq;
	int					clk;
	unsigned int		frequency;
	int					value;
	int	cs;
	int	mosi;
	int	miso;
	uint64_t dtg_number;
	uint8_t*	data_to_transmit;
	uint8_t*	data_recived;
	//uint8_t*	recived_page;
	uint8_t	byte_recived;
	size_t	recived_len;
	size_t	data_len;
	int	bit_num;
	int	byte_num;
	uint64_t loop;
	uint8_t is_init;
	uint8_t stop;
} _timer_handler;

static _timer_handler	_handler;

struct dentry  *file1;

struct mmap_info {
	char *data;	/* the data */
	int reference;       /* how many times it is mmapped */  	
};


/* keep track of how many times it is mmapped */

void mmap_open(struct vm_area_struct *vma)
{
	struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
	info->reference++;
}

void mmap_close(struct vm_area_struct *vma)
{
	struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
	info->reference--;
}

/* nopage is called the first time a memory area is accessed which is not in memory,
 * it does the actual mapping between kernel and user space memory
 */
//struct page *mmap_nopage(struct vm_area_struct *vma, unsigned long address, int *type)	--changed
static int mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct page *page;
	struct mmap_info *info;
	info = (struct mmap_info *)vma->vm_private_data;
	if (!info->data) {
		printk("no data\n");
		return NULL;	
	}

	/* get the page */
	page = virt_to_page(info->data);
	
	/* increment the reference count of this page */
	get_page(page);
	vmf->page = page;
	return 0;
}

struct vm_operations_struct mmap_vm_ops = {
	.open =     mmap_open,
	.close =    mmap_close,
	.fault =    mmap_fault,
	//.nopage =   mmap_nopage,				//--changed
};

int my_mmap(struct file *filp, struct vm_area_struct *vma)
{
	vma->vm_ops = &mmap_vm_ops;
	vma->vm_flags |= VM_RESERVED;
	/* assign the file private data to the vm private data */
	vma->vm_private_data = filp->private_data;
	mmap_open(vma);
	return 0;
}

int my_close(struct inode *inode, struct file *filp)
{
	struct mmap_info *info = filp->private_data;
	/* obtain new memory */
	//free_page((unsigned long)info->data);
    	kfree(info);
	filp->private_data = NULL;
	return 0;
}

int my_open(struct inode *inode, struct file *filp)
{
	struct mmap_info *info = kmalloc(sizeof(struct mmap_info), GFP_KERNEL);
	/* obtain new memory */
    	info->data = (char *)_handler.data_to_transmit;//(char *)get_zeroed_page(GFP_KERNEL);
	//memcpy(info->data, _handler.data_recived, _handler.recived_len);
	//memcpy(info->data + 32, filp->f_dentry->d_name.name, strlen(filp->f_dentry->d_name.name));
	/* assign this info struct to the file */
	filp->private_data = info;
	return 0;
}

static struct dentry* in_file;
static struct dentry* output_file;

static int is_space(char symbol) {
	return (symbol == ' ') || (symbol == '\t');
}

////////////////////////////////////////////////////////////////////////////////////////////

static int is_digit(char symbol)
{
	return (symbol >= '0') && (symbol <= '9');
}

////////////////////////////////////////////////////////////////////////////////////////////



static void stop(void)
{
	unsigned long flags=0;

	spin_lock_irqsave(&_lock,flags);

	if(_handler.irq >= 0)
	{
		free_irq(_handler.irq, (void*)&_handler);
		_handler.irq=-1;

		_handler.timer=-1;

		if(_handler.clk > 0)
		{
			//	restore previous GPIO state
			if(_gpio_prev)
			{
				__raw_writel(1 << _handler.clk, gpio_setdataout_addr);
			}
			else
			{
				__raw_writel(1 << _handler.clk, gpio_cleardataout_addr);
			}
			__raw_writel(1 << _handler.mosi, gpio_cleardataout_addr);
			__raw_writel(1 << _handler.cs, gpio_setdataout_addr);

			//kfree(_handler.recived_page);
			//kfree(_handler.data_to_transmit);

			gpio_free(_handler.clk);
			gpio_free(_handler.mosi);
			gpio_free(_handler.miso);
			gpio_free(_handler.cs);
		}

		_handler.frequency=0;
		_handler.value=0;

		debug("Timer stopped.\n");
	}

	spin_unlock_irqrestore(&_lock,flags);
}

////////////////////////////////////////////////////////////////////////////////////////////


static irqreturn_t timer_interrupt(int irq, void* dev_id) {
	_timer_handler* handler=(_timer_handler*)dev_id;

	if (handler->byte_num == handler->data_len) {
		return(IRQ_HANDLED);
	}

	int val = 0;

	if(handler && (handler->irq == irq)) {
		size_t byte_num = handler->recived_len + handler->byte_num;
		if (!handler->is_init) {
			if (handler->value) {
				handler->value = 0;
				handler->is_init = 1;
			} else {
				gpio_set_value(handler->cs, 1);
				handler->value = 1;
				handler->stop = 0;
			}
		} else {
			if (!handler->value && handler->byte_num == 0 && handler->bit_num == 0) {
				gpio_set_value(handler->cs, 0);
				val = handler->data_to_transmit[byte_num] & 0x80;
				gpio_set_value(handler->mosi, val);
				handler->byte_recived = gpio_get_value(handler->miso);
				handler->bit_num++;
				handler->value = 1;
			} else {
				gpio_set_value(handler->clk, handler->value);
				if(handler->value) {
					handler->value = 0;
				}
				else {
					if (handler->byte_num < handler->data_len && handler->bit_num < 8) {
						val = handler->data_to_transmit[byte_num] & (1 << (7 - handler->bit_num));
						gpio_set_value(handler->mosi, val);
						handler->byte_recived <<= 1;
						handler->byte_recived += gpio_get_value(handler->miso);
						handler->bit_num++;
					} else if (!handler->stop) {
						handler->stop = 1;
						return(IRQ_HANDLED);
					}
					handler->value = 1;
				}
			}

			if (handler->bit_num == 8 && handler->stop) {
				handler->bit_num = 0;
				handler->data_recived[byte_num] = handler->byte_recived;
				handler->byte_num++;
				if (handler->byte_num == handler->data_len) {
					gpio_set_value(handler->cs, 1);
					handler->dtg_number++;
					//memcpy((handler->recived_page + handler->recived_len), handler->data_recived, handler->data_len);
					if (handler->loop) {
						handler->recived_len += handler->byte_num;
						handler->byte_num = 0;
						handler->is_init = 0;
						handler->value = 0;
					} else {
						
					}
					handler->loop--;
				}
			}
		}
	}

	return(IRQ_HANDLED);
}

static int add_irq(void* data) {
	int err=0;
	int irq_number=_timers[timer].irq;

	debug("Adding IRQ %d handler\n",irq_number);

	err=request_irq(irq_number, timer_interrupt, 0, DRV_NAME, data);

	if(!err)
	{
		debug("Got IRQ %d.\n", irq_number);
		return irq_number;
	}
	else
	{
		debug("Timer IRQ handler: trouble requesting IRQ %d error %d\n",irq_number, err);
	}

    return -1;
}



static const struct file_operations spi_fops = {
	.open = my_open,
	.release = my_close,
	.mmap = my_mmap,
};


////////////////////////////////////////////////////////////////////////////////////////////

static int start(uint32_t clk, uint32_t mosi, uint32_t miso, uint32_t cs, uint32_t frequency, size_t data_len, uint64_t loop) {
	unsigned int timeout=0;
	int irq=-1;
	unsigned long flags=0;

	stop();

	spin_lock_irqsave(&_lock,flags);
	// need some time (10 ms) before first IRQ - even after "lock"?!
	__raw_writel(_timer_frequency/100, ath79_timer_base+_timers[timer].reload_reg);

	irq=add_irq(&_handler);

	if(irq >= 0) {
		_handler.timer=timer;
		_handler.irq=irq;

		gpio_request(clk, DRV_NAME);
		gpio_request(mosi, DRV_NAME);
		gpio_request(miso, DRV_NAME);
		gpio_request(cs, DRV_NAME);
		if(gpio_direction_output(clk, 0) == 0) {
			debug("Got CLK at %d.\n", clk);
			if(gpio_direction_output(mosi, 0) == 0) {
				debug("Got MOSI at %d.\n", mosi);
				if (gpio_direction_input(miso) == 0) {
					debug("Got MISO at %d.\n", miso);
					debug("MISO val: %d.\n", gpio_get_value(miso));
					if (gpio_direction_output(cs, 1) == 0) {
						debug("Got CS at %d.\n", cs);
						_handler.clk=clk;

						// save current GPIO state
						_gpio_prev=__raw_readl(gpio_readdata_addr+GPIO_OFFS_READ) & (1 << clk);

						timeout=_timer_frequency/frequency/2;
						__raw_writel(timeout, ath79_timer_base+_timers[timer].reload_reg);

						_handler.frequency=frequency;
						_handler.cs = cs;
						_handler.	mosi = mosi;
						_handler.	miso = miso;
						_handler.data_len = data_len;
						_handler.loop = loop;
						_handler.byte_num = 0;
						_handler.bit_num = 0;
						_handler.recived_len = 0;
						_handler.value = 0;
						_handler.byte_recived = 0;
						_handler.is_init = 0;
						_handler.dtg_number = 0;

						//size_t size = _handler.loop + 1 * _handler.data_len;

						//debug("allocating %u bytes for gt-spi input\n", size);
		
						//_handler.recived_page = kmalloc((size, GFP_KERNEL);
						//_handler.data_recived = (uint8_t*) kmalloc(size, GFP_KERNEL);
						//_handler.data_to_transmit = _handler.data_recived;
												
						
						debug("New frequency: %u.\n", frequency);
						spin_unlock_irqrestore(&_lock,flags);
						return 0;
					} else {
						debug("Can't set CS %d as output.\n", cs);
					}
				} else {
					debug("Can't set MISO %d as input.\n", miso);
				}
			} else {
				debug("Can't set MOSI %d as output.\n", mosi);
			}
		} else {
			debug("Can't set CLK %d as output.\n", clk);
		}
	}

	spin_unlock_irqrestore(&_lock,flags);

	stop();
	return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static ssize_t run_command(struct file *file, const char __user *buf,
                                size_t count, loff_t *ppos)
{
	char buffer[512];
	char line[64];
	char* in_pos=NULL;
	char* end=NULL;
	char* out_pos=NULL;

	// clk mosi miso cs frequency [bytes_num rep_cnt]
	// 21 22 24 19 1
	// 21 22 24 19 1 3 5

	if(count > 512)
		return -EINVAL;	//	file is too big

	copy_from_user(buffer, buf, count);
	buffer[count]=0;

	debug("Command is found (%u bytes length):\n%s\n",count,buffer);
	
	int i;
	for (i = 0; i < _handler.data_len; i++) {
		debug("recived byte %d\t%d", i, _handler.data_recived[i]);
	}

	in_pos=buffer;
	end=in_pos+count-1;

	while(in_pos < end) {
		uint32_t clk = 0;
		uint32_t mosi = 0;
		uint32_t miso = 0;
		uint32_t cs = 0;
		uint32_t frequency = 0;
		size_t bytes_num;
		unsigned int loop;

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace
		if(in_pos >= end) break;

		if(*in_pos == '-') {
			stop();
			return count;
		}
		else if(*in_pos == '?') {
			if(_handler.frequency) {
				printk(KERN_INFO DRV_NAME " is running on GPIO %d with frequency %u Hz (system timer %d).\n",
						_handler.clk,_handler.frequency,_handler.timer);
			} else {
				printk(KERN_INFO DRV_NAME " is not running (timer %d selected).\n",timer);
			}

			break;
		}

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%d", &clk);
		}
		else {
			printk(KERN_INFO DRV_NAME " can't read CLK GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%d", &mosi);
		}
		else {
			printk(KERN_INFO DRV_NAME " can't read MOSI GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%d", &miso);
		}
		else {
			printk(KERN_INFO DRV_NAME " can't read MISO GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%d", &cs);
		}
		else {
			printk(KERN_INFO DRV_NAME " can't read CS GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%u", &frequency);
		} else {
			printk(KERN_INFO DRV_NAME " can't read frequency.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%u", &bytes_num);
		} else {
			bytes_num = 1;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0])) {
			sscanf(line, "%u", &loop);
		} else {
			loop = 0;
		}

		if((clk > 0) && (frequency > 0) && (mosi > 0) && (miso > 0) && (cs > 0)) {
			if ((clk != mosi) && (clk != miso) && (clk != cs) && (mosi != miso) && (cs != mosi) && (cs != miso)) {
				if(start(clk, mosi, miso, cs, frequency, bytes_num, (uint64_t)loop) >= 0) {
					debug("Started!\n");
					break;
				}
			}
		}

		printk(KERN_INFO DRV_NAME " can't start.\n");
		return 0;
	}

	return count;
}

////////////////////////////////////////////////////////////////////////////////////////////

static const struct file_operations irq_fops =
{
	.write = run_command,
	
};

////////////////////////////////////////////////////////////////////////////////////////////

struct clk	//	defined in clock.c
{
	unsigned long rate;
};

static int __init mymodule_init(void)
{
	struct clk* ahb_clk=clk_get(NULL,"ahb");
	if(ahb_clk)
	{
		_timer_frequency=ahb_clk->rate;
	}

 	ath79_timer_base=ioremap_nocache(AR71XX_RESET_BASE, AR71XX_RESET_SIZE);

 	gpio_addr=ioremap_nocache(AR71XX_GPIO_BASE, AR71XX_GPIO_SIZE);

    	gpio_readdata_addr     = gpio_addr + GPIO_OFFS_READ;
    	gpio_setdataout_addr   = gpio_addr + GPIO_OFFS_SET;
    	gpio_cleardataout_addr = gpio_addr + GPIO_OFFS_CLEAR;

	_handler.timer=-1;
	_handler.irq=-1;
	_handler.data_to_transmit = (uint8_t*) get_zeroed_page(GFP_KERNEL);
	int i;
	for (i = 0; i < 16; i++) {
		_handler.data_to_transmit[i] = 0xFF;
	}
	_handler.data_recived = _handler.data_to_transmit;
	_handler.data_len = 1;
	_handler.byte_num = 0;
	_handler.bit_num = 0;


	output_file = 0;

	in_file=debugfs_create_file(CONTROL_FILE_NAME, 0200, NULL, NULL, &irq_fops);
	output_file=debugfs_create_file(SPI_FILE_NAME, 0200, NULL, NULL, &spi_fops);

	debug("System timer #%d frequency is %d Hz.\n",timer,_timer_frequency);
	printk(KERN_INFO DRV_NAME " is waiting for commands in file /sys/kernel/debug/" CONTROL_FILE_NAME ".\n");

    return 0;
}

static void __exit mymodule_exit(void)
{
	stop();
	free_page(_handler.data_to_transmit);
	debugfs_remove(in_file);
	debugfs_remove(output_file);

	return;
}

module_init(mymodule_init);
module_exit(mymodule_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sarin");
