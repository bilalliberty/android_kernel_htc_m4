/*
 * platform.c - platform 'pseudo' bus for legacy devices
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 *
 * Please see Documentation/driver-model/platform.txt for more
 * information.
 */

#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/bootmem.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include "base.h"

#define to_platform_driver(drv)	(container_of((drv), struct platform_driver, \
				 driver))

struct device platform_bus = {
	.init_name	= "platform",
};
EXPORT_SYMBOL_GPL(platform_bus);

void __weak arch_setup_pdev_archdata(struct platform_device *pdev)
{
}

struct resource *platform_get_resource(struct platform_device *dev,
				       unsigned int type, unsigned int num)
{
	int i;

	for (i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];

		if (type == resource_type(r) && num-- == 0)
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(platform_get_resource);

int platform_get_irq(struct platform_device *dev, unsigned int num)
{
	struct resource *r = platform_get_resource(dev, IORESOURCE_IRQ, num);

	return r ? r->start : -ENXIO;
}
EXPORT_SYMBOL_GPL(platform_get_irq);

struct resource *platform_get_resource_byname(struct platform_device *dev,
					      unsigned int type,
					      const char *name)
{
	int i;

	for (i = 0; i < dev->num_resources; i++) {
		struct resource *r = &dev->resource[i];

		if (type == resource_type(r) && !strcmp(r->name, name))
			return r;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(platform_get_resource_byname);

int platform_get_irq_byname(struct platform_device *dev, const char *name)
{
	struct resource *r = platform_get_resource_byname(dev, IORESOURCE_IRQ,
							  name);

	return r ? r->start : -ENXIO;
}
EXPORT_SYMBOL_GPL(platform_get_irq_byname);

int platform_add_devices(struct platform_device **devs, int num)
{
	int i, ret = 0;

	for (i = 0; i < num; i++) {
		ret = platform_device_register(devs[i]);
		if (ret) {
			while (--i >= 0)
				platform_device_unregister(devs[i]);
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(platform_add_devices);

struct platform_object {
	struct platform_device pdev;
	char name[1];
};

void platform_device_put(struct platform_device *pdev)
{
	if (pdev)
		put_device(&pdev->dev);
}
EXPORT_SYMBOL_GPL(platform_device_put);

static void platform_device_release(struct device *dev)
{
	struct platform_object *pa = container_of(dev, struct platform_object,
						  pdev.dev);

	of_device_node_put(&pa->pdev.dev);
	kfree(pa->pdev.dev.platform_data);
	kfree(pa->pdev.mfd_cell);
	kfree(pa->pdev.resource);
	kfree(pa);
}

struct platform_device *platform_device_alloc(const char *name, int id)
{
	struct platform_object *pa;

	pa = kzalloc(sizeof(struct platform_object) + strlen(name), GFP_KERNEL);
	if (pa) {
		strcpy(pa->name, name);
		pa->pdev.name = pa->name;
		pa->pdev.id = id;
		device_initialize(&pa->pdev.dev);
		pa->pdev.dev.release = platform_device_release;
		arch_setup_pdev_archdata(&pa->pdev);
	}

	return pa ? &pa->pdev : NULL;
}
EXPORT_SYMBOL_GPL(platform_device_alloc);

int platform_device_add_resources(struct platform_device *pdev,
				  const struct resource *res, unsigned int num)
{
	struct resource *r = NULL;

	if (res) {
		r = kmemdup(res, sizeof(struct resource) * num, GFP_KERNEL);
		if (!r)
			return -ENOMEM;
	}

	kfree(pdev->resource);
	pdev->resource = r;
	pdev->num_resources = num;
	return 0;
}
EXPORT_SYMBOL_GPL(platform_device_add_resources);

int platform_device_add_data(struct platform_device *pdev, const void *data,
			     size_t size)
{
	void *d = NULL;

	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d)
			return -ENOMEM;
	}

	kfree(pdev->dev.platform_data);
	pdev->dev.platform_data = d;
	return 0;
}
EXPORT_SYMBOL_GPL(platform_device_add_data);

int platform_device_add(struct platform_device *pdev)
{
	int i, ret = 0;

	if (!pdev)
		return -EINVAL;

	if (!pdev->dev.parent)
		pdev->dev.parent = &platform_bus;

	pdev->dev.bus = &platform_bus_type;

	if (pdev->id != -1)
		dev_set_name(&pdev->dev, "%s.%d", pdev->name,  pdev->id);
	else
		dev_set_name(&pdev->dev, "%s", pdev->name);

	for (i = 0; i < pdev->num_resources; i++) {
		struct resource *p, *r = &pdev->resource[i];

		if (r->name == NULL)
			r->name = dev_name(&pdev->dev);

		p = r->parent;
		if (!p) {
			if (resource_type(r) == IORESOURCE_MEM)
				p = &iomem_resource;
			else if (resource_type(r) == IORESOURCE_IO)
				p = &ioport_resource;
		}

		if (p && insert_resource(p, r)) {
			printk(KERN_ERR
			       "%s: failed to claim resource %d\n",
			       dev_name(&pdev->dev), i);
			ret = -EBUSY;
			goto failed;
		}
	}

	pr_debug("Registering platform device '%s'. Parent at %s\n",
		 dev_name(&pdev->dev), dev_name(pdev->dev.parent));

	ret = device_add(&pdev->dev);
	if (ret == 0)
		return ret;

 failed:
	while (--i >= 0) {
		struct resource *r = &pdev->resource[i];
		unsigned long type = resource_type(r);

		if (type == IORESOURCE_MEM || type == IORESOURCE_IO)
			release_resource(r);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(platform_device_add);

void platform_device_del(struct platform_device *pdev)
{
	int i;

	if (pdev) {
		device_del(&pdev->dev);

		for (i = 0; i < pdev->num_resources; i++) {
			struct resource *r = &pdev->resource[i];
			unsigned long type = resource_type(r);

			if (type == IORESOURCE_MEM || type == IORESOURCE_IO)
				release_resource(r);
		}
	}
}
EXPORT_SYMBOL_GPL(platform_device_del);

int platform_device_register(struct platform_device *pdev)
{
	device_initialize(&pdev->dev);
	arch_setup_pdev_archdata(pdev);
	return platform_device_add(pdev);
}
EXPORT_SYMBOL_GPL(platform_device_register);

void platform_device_unregister(struct platform_device *pdev)
{
	platform_device_del(pdev);
	platform_device_put(pdev);
}
EXPORT_SYMBOL_GPL(platform_device_unregister);

struct platform_device *platform_device_register_full(
		const struct platform_device_info *pdevinfo)
{
	int ret = -ENOMEM;
	struct platform_device *pdev;

	pdev = platform_device_alloc(pdevinfo->name, pdevinfo->id);
	if (!pdev)
		goto err_alloc;

	pdev->dev.parent = pdevinfo->parent;

	if (pdevinfo->dma_mask) {
		pdev->dev.dma_mask =
			kmalloc(sizeof(*pdev->dev.dma_mask), GFP_KERNEL);
		if (!pdev->dev.dma_mask)
			goto err;

		*pdev->dev.dma_mask = pdevinfo->dma_mask;
		pdev->dev.coherent_dma_mask = pdevinfo->dma_mask;
	}

	ret = platform_device_add_resources(pdev,
			pdevinfo->res, pdevinfo->num_res);
	if (ret)
		goto err;

	ret = platform_device_add_data(pdev,
			pdevinfo->data, pdevinfo->size_data);
	if (ret)
		goto err;

	ret = platform_device_add(pdev);
	if (ret) {
err:
		kfree(pdev->dev.dma_mask);

err_alloc:
		platform_device_put(pdev);
		return ERR_PTR(ret);
	}

	return pdev;
}
EXPORT_SYMBOL_GPL(platform_device_register_full);

static int platform_drv_probe(struct device *_dev)
{
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	struct platform_device *dev = to_platform_device(_dev);

	return drv->probe(dev);
}

static int platform_drv_probe_fail(struct device *_dev)
{
	return -ENXIO;
}

static int platform_drv_remove(struct device *_dev)
{
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	struct platform_device *dev = to_platform_device(_dev);

	return drv->remove(dev);
}

static void platform_drv_shutdown(struct device *_dev)
{
	struct platform_driver *drv = to_platform_driver(_dev->driver);
	struct platform_device *dev = to_platform_device(_dev);

	drv->shutdown(dev);
}

int platform_driver_register(struct platform_driver *drv)
{
	drv->driver.bus = &platform_bus_type;
	if (drv->probe)
		drv->driver.probe = platform_drv_probe;
	if (drv->remove)
		drv->driver.remove = platform_drv_remove;
	if (drv->shutdown)
		drv->driver.shutdown = platform_drv_shutdown;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(platform_driver_register);

void platform_driver_unregister(struct platform_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(platform_driver_unregister);

int __init_or_module platform_driver_probe(struct platform_driver *drv,
		int (*probe)(struct platform_device *))
{
	int retval, code;

	
	drv->driver.suppress_bind_attrs = true;

	
	drv->probe = probe;
	retval = code = platform_driver_register(drv);

	spin_lock(&drv->driver.bus->p->klist_drivers.k_lock);
	drv->probe = NULL;
	if (code == 0 && list_empty(&drv->driver.p->klist_devices.k_list))
		retval = -ENODEV;
	drv->driver.probe = platform_drv_probe_fail;
	spin_unlock(&drv->driver.bus->p->klist_drivers.k_lock);

	if (code != retval)
		platform_driver_unregister(drv);
	return retval;
}
EXPORT_SYMBOL_GPL(platform_driver_probe);

struct platform_device * __init_or_module platform_create_bundle(
			struct platform_driver *driver,
			int (*probe)(struct platform_device *),
			struct resource *res, unsigned int n_res,
			const void *data, size_t size)
{
	struct platform_device *pdev;
	int error;

	pdev = platform_device_alloc(driver->driver.name, -1);
	if (!pdev) {
		error = -ENOMEM;
		goto err_out;
	}

	error = platform_device_add_resources(pdev, res, n_res);
	if (error)
		goto err_pdev_put;

	error = platform_device_add_data(pdev, data, size);
	if (error)
		goto err_pdev_put;

	error = platform_device_add(pdev);
	if (error)
		goto err_pdev_put;

	error = platform_driver_probe(driver, probe);
	if (error)
		goto err_pdev_del;

	return pdev;

err_pdev_del:
	platform_device_del(pdev);
err_pdev_put:
	platform_device_put(pdev);
err_out:
	return ERR_PTR(error);
}
EXPORT_SYMBOL_GPL(platform_create_bundle);

static ssize_t modalias_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	struct platform_device	*pdev = to_platform_device(dev);
	int len = snprintf(buf, PAGE_SIZE, "platform:%s\n", pdev->name);

	return (len >= PAGE_SIZE) ? (PAGE_SIZE - 1) : len;
}

static struct device_attribute platform_dev_attrs[] = {
	__ATTR_RO(modalias),
	__ATTR_NULL,
};

static int platform_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct platform_device	*pdev = to_platform_device(dev);
	int rc;

	
	rc = of_device_uevent_modalias(dev,env);
	if (rc != -ENODEV)
		return rc;

	add_uevent_var(env, "MODALIAS=%s%s", PLATFORM_MODULE_PREFIX,
			pdev->name);
	return 0;
}

static const struct platform_device_id *platform_match_id(
			const struct platform_device_id *id,
			struct platform_device *pdev)
{
	while (id->name[0]) {
		if (strcmp(pdev->name, id->name) == 0) {
			pdev->id_entry = id;
			return id;
		}
		id++;
	}
	return NULL;
}

static int platform_match(struct device *dev, struct device_driver *drv)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct platform_driver *pdrv = to_platform_driver(drv);

	
	if (of_driver_match_device(dev, drv))
		return 1;

	
	if (pdrv->id_table)
		return platform_match_id(pdrv->id_table, pdev) != NULL;

	
	return (strcmp(pdev->name, drv->name) == 0);
}

#ifdef CONFIG_PM_SLEEP

static int platform_legacy_suspend(struct device *dev, pm_message_t mesg)
{
	struct platform_driver *pdrv = to_platform_driver(dev->driver);
	struct platform_device *pdev = to_platform_device(dev);
	int ret = 0;

	if (dev->driver && pdrv->suspend)
		ret = pdrv->suspend(pdev, mesg);

	return ret;
}

static int platform_legacy_resume(struct device *dev)
{
	struct platform_driver *pdrv = to_platform_driver(dev->driver);
	struct platform_device *pdev = to_platform_device(dev);
	int ret = 0;

	if (dev->driver && pdrv->resume)
		ret = pdrv->resume(pdev);

	return ret;
}

#endif 

#ifdef CONFIG_SUSPEND

int platform_pm_suspend(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->suspend)
			ret = drv->pm->suspend(dev);
	} else {
		ret = platform_legacy_suspend(dev, PMSG_SUSPEND);
	}

	return ret;
}

int platform_pm_resume(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->resume)
			ret = drv->pm->resume(dev);
	} else {
		ret = platform_legacy_resume(dev);
	}

	return ret;
}

#endif 

#ifdef CONFIG_HIBERNATE_CALLBACKS

int platform_pm_freeze(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->freeze)
			ret = drv->pm->freeze(dev);
	} else {
		ret = platform_legacy_suspend(dev, PMSG_FREEZE);
	}

	return ret;
}

int platform_pm_thaw(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->thaw)
			ret = drv->pm->thaw(dev);
	} else {
		ret = platform_legacy_resume(dev);
	}

	return ret;
}

int platform_pm_poweroff(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->poweroff)
			ret = drv->pm->poweroff(dev);
	} else {
		ret = platform_legacy_suspend(dev, PMSG_HIBERNATE);
	}

	return ret;
}

int platform_pm_restore(struct device *dev)
{
	struct device_driver *drv = dev->driver;
	int ret = 0;

	if (!drv)
		return 0;

	if (drv->pm) {
		if (drv->pm->restore)
			ret = drv->pm->restore(dev);
	} else {
		ret = platform_legacy_resume(dev);
	}

	return ret;
}

#endif 

static const struct dev_pm_ops platform_dev_pm_ops = {
	.runtime_suspend = pm_generic_runtime_suspend,
	.runtime_resume = pm_generic_runtime_resume,
	.runtime_idle = pm_generic_runtime_idle,
	USE_PLATFORM_PM_SLEEP_OPS
};

struct bus_type platform_bus_type = {
	.name		= "platform",
	.dev_attrs	= platform_dev_attrs,
	.match		= platform_match,
	.uevent		= platform_uevent,
	.pm		= &platform_dev_pm_ops,
};
EXPORT_SYMBOL_GPL(platform_bus_type);

int __init platform_bus_init(void)
{
	int error;

	early_platform_cleanup();

	error = device_register(&platform_bus);
	if (error)
		return error;
	error =  bus_register(&platform_bus_type);
	if (error)
		device_unregister(&platform_bus);
	return error;
}

#ifndef ARCH_HAS_DMA_GET_REQUIRED_MASK
u64 dma_get_required_mask(struct device *dev)
{
	u32 low_totalram = ((max_pfn - 1) << PAGE_SHIFT);
	u32 high_totalram = ((max_pfn - 1) >> (32 - PAGE_SHIFT));
	u64 mask;

	if (!high_totalram) {
		
		low_totalram = (1 << (fls(low_totalram) - 1));
		low_totalram += low_totalram - 1;
		mask = low_totalram;
	} else {
		high_totalram = (1 << (fls(high_totalram) - 1));
		high_totalram += high_totalram - 1;
		mask = (((u64)high_totalram) << 32) + 0xffffffff;
	}
	return mask;
}
EXPORT_SYMBOL_GPL(dma_get_required_mask);
#endif

static __initdata LIST_HEAD(early_platform_driver_list);
static __initdata LIST_HEAD(early_platform_device_list);

int __init early_platform_driver_register(struct early_platform_driver *epdrv,
					  char *buf)
{
	char *tmp;
	int n;

	if (!epdrv->list.next) {
		INIT_LIST_HEAD(&epdrv->list);
		list_add_tail(&epdrv->list, &early_platform_driver_list);
	}

	n = strlen(epdrv->pdrv->driver.name);
	if (buf && !strncmp(buf, epdrv->pdrv->driver.name, n)) {
		list_move(&epdrv->list, &early_platform_driver_list);

		
		if (buf[n] == '\0' || buf[n] == ',')
			epdrv->requested_id = -1;
		else {
			epdrv->requested_id = simple_strtoul(&buf[n + 1],
							     &tmp, 10);

			if (buf[n] != '.' || (tmp == &buf[n + 1])) {
				epdrv->requested_id = EARLY_PLATFORM_ID_ERROR;
				n = 0;
			} else
				n += strcspn(&buf[n + 1], ",") + 1;
		}

		if (buf[n] == ',')
			n++;

		if (epdrv->bufsize) {
			memcpy(epdrv->buffer, &buf[n],
			       min_t(int, epdrv->bufsize, strlen(&buf[n]) + 1));
			epdrv->buffer[epdrv->bufsize - 1] = '\0';
		}
	}

	return 0;
}

void __init early_platform_add_devices(struct platform_device **devs, int num)
{
	struct device *dev;
	int i;

	
	for (i = 0; i < num; i++) {
		dev = &devs[i]->dev;

		if (!dev->devres_head.next) {
			INIT_LIST_HEAD(&dev->devres_head);
			list_add_tail(&dev->devres_head,
				      &early_platform_device_list);
		}
	}
}

void __init early_platform_driver_register_all(char *class_str)
{
	parse_early_options(class_str);
}

static  __init struct platform_device *
early_platform_match(struct early_platform_driver *epdrv, int id)
{
	struct platform_device *pd;

	list_for_each_entry(pd, &early_platform_device_list, dev.devres_head)
		if (platform_match(&pd->dev, &epdrv->pdrv->driver))
			if (pd->id == id)
				return pd;

	return NULL;
}

static  __init int early_platform_left(struct early_platform_driver *epdrv,
				       int id)
{
	struct platform_device *pd;

	list_for_each_entry(pd, &early_platform_device_list, dev.devres_head)
		if (platform_match(&pd->dev, &epdrv->pdrv->driver))
			if (pd->id >= id)
				return 1;

	return 0;
}

static int __init early_platform_driver_probe_id(char *class_str,
						 int id,
						 int nr_probe)
{
	struct early_platform_driver *epdrv;
	struct platform_device *match;
	int match_id;
	int n = 0;
	int left = 0;

	list_for_each_entry(epdrv, &early_platform_driver_list, list) {
		
		if (strcmp(class_str, epdrv->class_str))
			continue;

		if (id == -2) {
			match_id = epdrv->requested_id;
			left = 1;

		} else {
			match_id = id;
			left += early_platform_left(epdrv, id);

			
			switch (epdrv->requested_id) {
			case EARLY_PLATFORM_ID_ERROR:
			case EARLY_PLATFORM_ID_UNSET:
				break;
			default:
				if (epdrv->requested_id == id)
					match_id = EARLY_PLATFORM_ID_UNSET;
			}
		}

		switch (match_id) {
		case EARLY_PLATFORM_ID_ERROR:
			pr_warning("%s: unable to parse %s parameter\n",
				   class_str, epdrv->pdrv->driver.name);
			
		case EARLY_PLATFORM_ID_UNSET:
			match = NULL;
			break;
		default:
			match = early_platform_match(epdrv, match_id);
		}

		if (match) {
			if (!match->dev.init_name && slab_is_available()) {
				if (match->id != -1)
					match->dev.init_name =
						kasprintf(GFP_KERNEL, "%s.%d",
							  match->name,
							  match->id);
				else
					match->dev.init_name =
						kasprintf(GFP_KERNEL, "%s",
							  match->name);

				if (!match->dev.init_name)
					return -ENOMEM;
			}

			if (epdrv->pdrv->probe(match))
				pr_warning("%s: unable to probe %s early.\n",
					   class_str, match->name);
			else
				n++;
		}

		if (n >= nr_probe)
			break;
	}

	if (left)
		return n;
	else
		return -ENODEV;
}

int __init early_platform_driver_probe(char *class_str,
				       int nr_probe,
				       int user_only)
{
	int k, n, i;

	n = 0;
	for (i = -2; n < nr_probe; i++) {
		k = early_platform_driver_probe_id(class_str, i, nr_probe - n);

		if (k < 0)
			break;

		n += k;

		if (user_only)
			break;
	}

	return n;
}

void __init early_platform_cleanup(void)
{
	struct platform_device *pd, *pd2;

	
	list_for_each_entry_safe(pd, pd2, &early_platform_device_list,
				 dev.devres_head) {
		list_del(&pd->dev.devres_head);
		memset(&pd->dev.devres_head, 0, sizeof(pd->dev.devres_head));
	}
}
