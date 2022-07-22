#pragma once

#include <fs/fd.h>
#include <fs/vfs.h>
#include <lib/types.h>

struct cdev {
	struct file_ops *fops;
	void *private_data;
};


// Used by the fd open function.
int cdev_open(struct vfs_node *node, struct file_handle *file);

int cdev_register(dev_t dev, struct cdev *cdev);
int cdev_unregister(dev_t dev);
