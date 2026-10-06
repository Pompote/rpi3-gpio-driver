// SPDX-License-Identifier: GPL-2.0-only
/*
 * rpi3_gpio_driver.c - LED + push button driver for Raspberry Pi 3
 *
 * Platform driver bound via Device Tree (see
 * dts/rpi3-gpio-led-button-overlay.dts) to two GPIO lines:
 *   - "led"    : output, drives an LED
 *   - "button" : input, interrupt-driven push button
 *
 * Userspace interface: a single misc character device /dev/rpi3gpio0
 *   - write('0' / '1') -> turns the LED off / on
 *   - read()           -> blocks until a "PRESSED\n" / "RELEASED\n" button
 *                         event; also usable with poll()/select()/epoll()
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#define RPI3_GPIO_DEBOUNCE_MS	50
#define RPI3_GPIO_EVENT_MAXLEN	16

struct rpi3_gpio_dev {
	struct gpio_desc	*led_gpio;
	struct gpio_desc	*button_gpio;
	int			irq;
	struct miscdevice	miscdev;

	struct mutex		lock;
	wait_queue_head_t	wq;
	char			event[RPI3_GPIO_EVENT_MAXLEN];
	bool			event_pending;
	unsigned long		last_irq_jiffies;
};

/* misc_open() stores the struct miscdevice pointer in file->private_data
 * *before* calling our ->open(); swap it for the enclosing driver struct
 * so every other fop can use file->private_data directly. */
static int rpi3_gpio_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;

	file->private_data = container_of(miscdev, struct rpi3_gpio_dev, miscdev);
	return 0;
}

static ssize_t rpi3_gpio_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct rpi3_gpio_dev *gdev = file->private_data;
	char value;

	if (count < 1)
		return -EINVAL;

	if (copy_from_user(&value, buf, 1))
		return -EFAULT;

	switch (value) {
	case '0':
		gpiod_set_value_cansleep(gdev->led_gpio, 0);
		break;
	case '1':
		gpiod_set_value_cansleep(gdev->led_gpio, 1);
		break;
	default:
		return -EINVAL;
	}

	return count;
}

static ssize_t rpi3_gpio_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct rpi3_gpio_dev *gdev = file->private_data;
	size_t len;
	int ret;

	if (file->f_flags & O_NONBLOCK) {
		if (!gdev->event_pending)
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(gdev->wq, gdev->event_pending);
		if (ret)
			return ret;
	}

	mutex_lock(&gdev->lock);
	len = min(count, strlen(gdev->event));
	if (copy_to_user(buf, gdev->event, len)) {
		mutex_unlock(&gdev->lock);
		return -EFAULT;
	}
	gdev->event_pending = false;
	mutex_unlock(&gdev->lock);

	return len;
}

static __poll_t rpi3_gpio_poll(struct file *file, poll_table *wait)
{
	struct rpi3_gpio_dev *gdev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &gdev->wq, wait);
	if (gdev->event_pending)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

/*
 * The VFS's dispatch vtable: read()/write()/poll() on the /dev/rpi3gpio0
 * fd resolve to file->f_op->read/write/poll, i.e. straight to the
 * functions below. Same generic dispatch code path as a real file, a
 * socket or a pipe, only this struct differs.
 */
static const struct file_operations rpi3_gpio_fops = {
	.owner	= THIS_MODULE,
	.open	= rpi3_gpio_open,
	.read	= rpi3_gpio_read,
	.write	= rpi3_gpio_write,
	.poll	= rpi3_gpio_poll,
	.llseek	= no_llseek,
};

static irqreturn_t rpi3_gpio_button_isr(int irq, void *dev_id)
{
	struct rpi3_gpio_dev *gdev = dev_id;
	int value;

	/* Software debounce: ignore edges that are too close together */
	if (time_before(jiffies, gdev->last_irq_jiffies +
			 msecs_to_jiffies(RPI3_GPIO_DEBOUNCE_MS)))
		return IRQ_HANDLED;
	gdev->last_irq_jiffies = jiffies;

	/*
	 * gpiod_get_value_cansleep() already returns the *logical* value:
	 * the button-gpios property is flagged GPIO_ACTIVE_LOW in the DT
	 * overlay, so 1 here means "pressed" regardless of the physical
	 * voltage level on the pin.
	 */
	value = gpiod_get_value_cansleep(gdev->button_gpio);

	mutex_lock(&gdev->lock);
	strscpy(gdev->event, value ? "PRESSED\n" : "RELEASED\n",
		sizeof(gdev->event));
	gdev->event_pending = true;
	mutex_unlock(&gdev->lock);

	wake_up_interruptible(&gdev->wq);

	return IRQ_HANDLED;
}

static int rpi3_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi3_gpio_dev *gdev;
	int ret;

	/*
	 * Every devm_*() call below ties its resource to gdev's lifetime:
	 * the kernel frees/releases it automatically, in reverse order, the
	 * moment probe() returns an error or the driver is unbound. No
	 * manual unwind path is needed anywhere in this function.
	 */
	gdev = devm_kzalloc(dev, sizeof(*gdev), GFP_KERNEL);
	if (!gdev)
		return -ENOMEM;

	/*
	 * dev_err_probe() logs the error (silently, if err == -EPROBE_DEFER,
	 * since that just means "retry later", not a real failure) and
	 * returns err, in one call. Used for every fallible step below.
	 */
	gdev->led_gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(gdev->led_gpio))
		return dev_err_probe(dev, PTR_ERR(gdev->led_gpio),
				     "failed to get 'led' gpio\n");

	gdev->button_gpio = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(gdev->button_gpio))
		return dev_err_probe(dev, PTR_ERR(gdev->button_gpio),
				     "failed to get 'button' gpio\n");

	ret = gpiod_to_irq(gdev->button_gpio);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to map button gpio to irq\n");
	gdev->irq = ret;

	mutex_init(&gdev->lock);
	init_waitqueue_head(&gdev->wq);

	/*
	 * No hard-irq handler: everything (debounce + gpiod read, which
	 * may sleep) happens in the threaded handler. IRQF_ONESHOT is
	 * mandatory in that case (irq stays masked until the thread
	 * finishes, so a bouncing line can't re-enter before it's handled).
	 */
	ret = devm_request_threaded_irq(dev, gdev->irq, NULL,
					 rpi3_gpio_button_isr,
					 IRQF_TRIGGER_RISING |
					 IRQF_TRIGGER_FALLING |
					 IRQF_ONESHOT,
					 dev_name(dev), gdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	/*
	 * misc_register() is a small generic kernel subsystem
	 * (drivers/char/misc.c): it allocates a minor under the shared misc
	 * major, registers the cdev, and creates /dev/rpi3gpio0 for us. No
	 * devm_ variant used here on purpose, to stay buildable on older
	 * kernels, hence the explicit misc_deregister() in remove() below.
	 */
	gdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	gdev->miscdev.name = "rpi3gpio0";
	gdev->miscdev.fops = &rpi3_gpio_fops;
	gdev->miscdev.parent = dev;

	ret = misc_register(&gdev->miscdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register misc device\n");

	platform_set_drvdata(pdev, gdev);
	dev_info(dev, "loaded, control the LED/button via /dev/%s\n",
		 gdev->miscdev.name);

	return 0;
}

/*
 * NOTE: kernels >= 6.11 moved platform_driver::remove to a void-returning
 * signature (the old int-returning one is deprecated, then removed a few
 * releases later). Adjust this prototype if building against such a tree.
 */
static int rpi3_gpio_remove(struct platform_device *pdev)
{
	struct rpi3_gpio_dev *gdev = platform_get_drvdata(pdev);

	misc_deregister(&gdev->miscdev);
	return 0;
}

/*
 * This table is used two different ways:
 *   - .of_match_table below is what the driver core actually reads at
 *     runtime to bind a DT node to this driver (plain string compare
 *     against the node's "compatible" property).
 *   - MODULE_DEVICE_TABLE() copies the same table into the compiled
 *     .ko's metadata, in a format modinfo/depmod/udev understand, so
 *     the module can be auto-loaded when a matching node appears
 *     instead of requiring a manual insmod. It plays no role in the
 *     actual matching above.
 */
static const struct of_device_id rpi3_gpio_of_match[] = {
	{ .compatible = "pompote,rpi3-led-button" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpi3_gpio_of_match);

static struct platform_driver rpi3_gpio_driver = {
	.probe	= rpi3_gpio_probe,
	.remove	= rpi3_gpio_remove,
	.driver	= {
		.name		= "rpi3-gpio-driver",
		.of_match_table	= rpi3_gpio_of_match,
	},
};

/*
 * Expands to a module_init()/module_exit() pair that respectively call
 * platform_driver_register()/_unregister() on rpi3_gpio_driver. This
 * is the actual hook into the kernel's module loader: insmod runs the
 * generated init function, which registers this struct with the
 * platform bus and returns immediately, probe() runs later, whenever
 * the bus finds a matching device.
 */
module_platform_driver(rpi3_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pierre Samperiz");
MODULE_DESCRIPTION("GPIO LED + push button driver for Raspberry Pi 3");
MODULE_VERSION("1.0");
