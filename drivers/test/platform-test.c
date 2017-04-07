#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>

static ssize_t show_test1(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	dev_info(dev, "show test1 called\n");
	return 0;
}

static ssize_t store_test1(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	dev_info(dev, "store test1 called\n");
	return 0;
}

static DEVICE_ATTR(test1, S_IRUGO | S_IWUSR, show_test1, store_test1);

static ssize_t show_test2(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	dev_info(dev, "show test2 called\n");
	return 0;
}

static ssize_t store_test2(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	dev_info(dev, "store test2 called\n");
	return 0;
}

static DEVICE_ATTR(test2, S_IRUGO | S_IWUSR, show_test2, store_test2);

static struct attribute *test_sysfs_attrs[] = {
	&dev_attr_test1.attr,
	&dev_attr_test2.attr,
	NULL,
};

static struct attribute_group test_sysfs_group = {
	.name = "test",
	.attrs = test_sysfs_attrs,
};

static int platform_test_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	dev_info(dev, "%s called", __func__);

	/* Register sysfs hooks */
	ret = sysfs_create_group(&dev->kobj, &test_sysfs_group);
	if (ret < 0) {
		dev_err(dev, "couldn't register tesy sysfs group\n");
		return ret;
	}

	return 0;
}

static int platform_test_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_info(dev, "%s called", __func__);

	return 0;
}

static struct platform_driver platform_test_driver = {
	.probe		= platform_test_probe,
	.remove		= platform_test_remove,
	.driver		= {
		.name	= "platform_test",
	},
};

static int __init platform_test_init(void)
{
	platform_device_register_simple("platform_test",
			PLATFORM_DEVID_AUTO, NULL, 0);

	return platform_driver_register(&platform_test_driver);
}
module_init(platform_test_init);

static void __exit platform_test_exit(void)
{
	platform_driver_unregister(&platform_test_driver);
}
module_exit(platform_test_exit);

MODULE_ALIAS("platform:platform_test");
MODULE_AUTHOR("Sathyanarayanan Kuppuswamy<sathyaosid@gmail.com>");
MODULE_DESCRIPTION("platform test driver");
MODULE_LICENSE("GPL");
